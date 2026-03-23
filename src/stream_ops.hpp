#pragma once

#include <atomic>
#include <coroutine>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <uv.h>

#include "luvcoro/error.hpp"
#include "luvcoro/runtime.hpp"

namespace luvcoro::detail {

template <typename Fn>
using stop_callback_t = std::stop_callback<std::decay_t<Fn>>;

template <typename StreamHandle>
auto as_stream(StreamHandle* handle) -> uv_stream_t* {
  return reinterpret_cast<uv_stream_t*>(handle);
}

template <typename StreamHandle>
auto stream_write(uv_runtime& runtime,
                  StreamHandle* stream,
                  std::string data,
                  std::stop_token stop_token) -> coro::task<std::size_t> {
  if (data.empty()) {
    co_return 0;
  }

  struct state {
    uv_runtime* runtime = nullptr;
    uv_stream_t* stream = nullptr;
    std::string data;
    uv_write_t req{};
    std::coroutine_handle<> continuation{};
    std::shared_ptr<state> keep_alive{};
    std::optional<stop_callback_t<std::function<void()>>> stop_callback{};
    std::atomic<bool> completed = false;
    int status = 0;
    bool cancel_requested = false;
    bool started = false;

    void complete() {
      if (completed.exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      auto h = continuation;
      continuation = {};
      if (h) {
        h.resume();
      }
      stop_callback.reset();
      keep_alive.reset();
    }
  };

  struct awaiter {
    std::shared_ptr<state> shared_state;
    std::stop_token stop_token;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) {
      if (stop_token.stop_requested()) {
        shared_state->cancel_requested = true;
        shared_state->status = UV_ECANCELED;
        return false;
      }

      shared_state->continuation = h;
      shared_state->keep_alive = shared_state;
      shared_state->req.data = shared_state.get();

      if (stop_token.stop_possible()) {
        shared_state->stop_callback.emplace(stop_token, std::function<void()>(
            [op_state = shared_state]() {
              op_state->runtime->post([op_state]() {
                if (op_state->completed.load(std::memory_order_acquire)) {
                  return;
                }
                op_state->cancel_requested = true;
                if (!op_state->started) {
                  op_state->status = UV_ECANCELED;
                  op_state->complete();
                  return;
                }
                const int rc =
                    uv_cancel(reinterpret_cast<uv_req_t*>(&op_state->req));
                if (rc < 0 && rc != UV_EBUSY && rc != UV_ENOSYS) {
                  op_state->status = UV_ECANCELED;
                  op_state->complete();
                }
              });
            }));
      }

      shared_state->runtime->post([op_state = shared_state]() {
        if (op_state->completed.load(std::memory_order_acquire)) {
          return;
        }

        auto buf = uv_buf_init(op_state->data.data(),
                               static_cast<unsigned int>(op_state->data.size()));
        op_state->started = true;
        int rc = uv_write(
            &op_state->req, op_state->stream, &buf, 1,
            [](uv_write_t* req, int write_status) {
              auto* self = static_cast<state*>(req->data);
              self->status = write_status;
              self->complete();
            });

        if (rc < 0) {
          op_state->status = rc;
          op_state->complete();
        }
      });

      return true;
    }

    auto await_resume() const -> std::size_t {
      if (shared_state->status == UV_ECANCELED ||
          (shared_state->cancel_requested && shared_state->status == 0)) {
        throw operation_cancelled();
      }
      if (shared_state->status < 0) {
        throw_uv_error(shared_state->status, "uv_write");
      }
      return shared_state->data.size();
    }
  };

  auto shared_state = std::make_shared<state>();
  shared_state->runtime = &runtime;
  shared_state->stream = as_stream(stream);
  shared_state->data = std::move(data);
  co_return co_await awaiter{std::move(shared_state), stop_token};
}

template <typename StreamHandle>
auto stream_write_some(uv_runtime& runtime,
                       StreamHandle* stream,
                       std::span<const char> data,
                       std::stop_token stop_token) -> coro::task<std::size_t> {
  if (data.empty()) {
    co_return 0;
  }

  co_return co_await stream_write(
      runtime, stream, std::string(data.data(), data.size()), stop_token);
}

template <typename StreamHandle>
auto stream_read_some_into(uv_runtime& runtime,
                           StreamHandle* stream,
                           std::span<char> buffer,
                           std::stop_token stop_token) -> coro::task<std::size_t> {
  if (buffer.empty()) {
    co_return 0;
  }

  struct state {
    uv_runtime* runtime = nullptr;
    uv_stream_t* stream = nullptr;
    std::span<char> buffer;
    std::coroutine_handle<> continuation{};
    std::shared_ptr<state> keep_alive{};
    std::optional<stop_callback_t<std::function<void()>>> stop_callback{};
    std::atomic<bool> completed = false;
    int status = 0;
    std::size_t nread = 0;
    bool cancel_requested = false;

    void complete() {
      if (completed.exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      auto h = continuation;
      continuation = {};
      if (h) {
        h.resume();
      }
      stop_callback.reset();
      keep_alive.reset();
    }
  };

  struct awaiter {
    std::shared_ptr<state> shared_state;
    std::stop_token stop_token;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) {
      if (stop_token.stop_requested()) {
        shared_state->cancel_requested = true;
        shared_state->status = UV_ECANCELED;
        return false;
      }

      shared_state->continuation = h;
      shared_state->keep_alive = shared_state;

      if (stop_token.stop_possible()) {
        shared_state->stop_callback.emplace(stop_token, std::function<void()>(
            [op_state = shared_state]() {
              op_state->runtime->post([op_state]() {
                if (op_state->completed.load(std::memory_order_acquire)) {
                  return;
                }
                op_state->cancel_requested = true;
                if (op_state->stream->data == op_state.get()) {
                  uv_read_stop(op_state->stream);
                  op_state->stream->data = nullptr;
                }
                op_state->status = UV_ECANCELED;
                op_state->complete();
              });
            }));
      }

      shared_state->runtime->post([op_state = shared_state]() {
        if (op_state->completed.load(std::memory_order_acquire)) {
          return;
        }

        if (op_state->stream->data != nullptr) {
          op_state->status = UV_EBUSY;
          op_state->complete();
          return;
        }

        op_state->stream->data = op_state.get();

        int rc = uv_read_start(
            op_state->stream,
            [](uv_handle_t* handle, size_t, uv_buf_t* buf) {
              auto* self = static_cast<state*>(handle->data);
              buf->base = self->buffer.data();
              buf->len = static_cast<unsigned int>(self->buffer.size());
            },
            [](uv_stream_t* stream, ssize_t nread, const uv_buf_t*) {
              if (nread == 0) {
                return;
              }

              auto* self = static_cast<state*>(stream->data);
              if (self == nullptr) {
                return;
              }

              if (nread > 0) {
                self->status = 0;
                self->nread = static_cast<std::size_t>(nread);
              } else if (nread == UV_EOF) {
                self->status = UV_EOF;
                self->nread = 0;
              } else {
                self->status = static_cast<int>(nread);
                self->nread = 0;
              }

              uv_read_stop(stream);
              stream->data = nullptr;
              self->complete();
            });

        if (rc < 0) {
          op_state->stream->data = nullptr;
          op_state->status = rc;
          op_state->complete();
        }
      });

      return true;
    }

    auto await_resume() const -> std::size_t {
      if (shared_state->status == UV_ECANCELED ||
          (shared_state->cancel_requested && shared_state->status == 0)) {
        throw operation_cancelled();
      }
      if (shared_state->status < 0 && shared_state->status != UV_EOF) {
        throw_uv_error(shared_state->status, "uv_read_start");
      }
      return shared_state->nread;
    }
  };

  auto shared_state = std::make_shared<state>();
  shared_state->runtime = &runtime;
  shared_state->stream = as_stream(stream);
  shared_state->buffer = buffer;
  co_return co_await awaiter{std::move(shared_state), stop_token};
}

template <typename StreamHandle>
auto stream_read_some(uv_runtime& runtime,
                      StreamHandle* stream,
                      std::size_t max_bytes,
                      std::stop_token stop_token) -> coro::task<std::string> {
  if (max_bytes == 0) {
    co_return std::string{};
  }

  std::string out(max_bytes == 0 ? 1 : max_bytes, '\0');
  auto nread = co_await stream_read_some_into(
      runtime, stream, std::span<char>(out.data(), out.size()), stop_token);
  out.resize(nread);
  co_return out;
}

template <typename StreamHandle>
auto stream_shutdown(uv_runtime& runtime,
                     StreamHandle* stream,
                     std::stop_token stop_token) -> coro::task<void> {
  struct state {
    uv_runtime* runtime = nullptr;
    uv_stream_t* stream = nullptr;
    uv_shutdown_t req{};
    std::coroutine_handle<> continuation{};
    std::shared_ptr<state> keep_alive{};
    std::optional<stop_callback_t<std::function<void()>>> stop_callback{};
    std::atomic<bool> completed = false;
    int status = 0;
    bool cancel_requested = false;
    bool started = false;

    void complete() {
      if (completed.exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      auto h = continuation;
      continuation = {};
      if (h) {
        h.resume();
      }
      stop_callback.reset();
      keep_alive.reset();
    }
  };

  struct awaiter {
    std::shared_ptr<state> shared_state;
    std::stop_token stop_token;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) {
      if (stop_token.stop_requested()) {
        shared_state->cancel_requested = true;
        shared_state->status = UV_ECANCELED;
        return false;
      }

      shared_state->continuation = h;
      shared_state->keep_alive = shared_state;
      shared_state->req.data = shared_state.get();

      if (stop_token.stop_possible()) {
        shared_state->stop_callback.emplace(stop_token, std::function<void()>(
            [op_state = shared_state]() {
              op_state->runtime->post([op_state]() {
                if (op_state->completed.load(std::memory_order_acquire)) {
                  return;
                }
                op_state->cancel_requested = true;
                if (!op_state->started) {
                  op_state->status = UV_ECANCELED;
                  op_state->complete();
                  return;
                }
                const int rc =
                    uv_cancel(reinterpret_cast<uv_req_t*>(&op_state->req));
                if (rc < 0 && rc != UV_EBUSY && rc != UV_ENOSYS) {
                  op_state->status = UV_ECANCELED;
                  op_state->complete();
                }
              });
            }));
      }

      shared_state->runtime->post([op_state = shared_state]() {
        if (op_state->completed.load(std::memory_order_acquire)) {
          return;
        }

        op_state->started = true;
        const int rc = uv_shutdown(
            &op_state->req, op_state->stream,
            [](uv_shutdown_t* req, int status) {
              auto* self = static_cast<state*>(req->data);
              self->status = status;
              self->complete();
            });
        if (rc < 0) {
          op_state->status = rc;
          op_state->complete();
        }
      });

      return true;
    }

    auto await_resume() const -> void {
      if (shared_state->status == UV_ECANCELED ||
          (shared_state->cancel_requested && shared_state->status == 0)) {
        throw operation_cancelled();
      }
      if (shared_state->status < 0) {
        throw_uv_error(shared_state->status, "uv_shutdown");
      }
    }
  };

  auto shared_state = std::make_shared<state>();
  shared_state->runtime = &runtime;
  shared_state->stream = as_stream(stream);
  co_await awaiter{std::move(shared_state), stop_token};
}

}  // namespace luvcoro::detail
