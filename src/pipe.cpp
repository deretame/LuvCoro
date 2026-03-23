#include "luvcoro/pipe.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "luvcoro/error.hpp"
#include "stream_ops.hpp"

namespace luvcoro {
namespace {

template <typename Fn>
using stop_callback_t = std::stop_callback<std::decay_t<Fn>>;

#ifdef _WIN32
auto duplicate_file_descriptor(uv_file fd) -> uv_file {
  const int dup_fd = _dup(fd);
  if (dup_fd < 0) {
    throw_uv_error(uv_translate_sys_error(errno), "_dup");
  }
  return dup_fd;
}

void close_file_descriptor_noexcept(uv_file fd) noexcept {
  if (fd >= 0) {
    _close(fd);
  }
}
#else
auto duplicate_file_descriptor(uv_file fd) -> uv_file {
  const int dup_fd = dup(fd);
  if (dup_fd < 0) {
    throw_uv_error(uv_translate_sys_error(errno), "dup");
  }
  return dup_fd;
}

void close_file_descriptor_noexcept(uv_file fd) noexcept {
  if (fd >= 0) {
    close(fd);
  }
}
#endif

auto get_pipe_name(uv_runtime& runtime,
                   const uv_pipe_t* handle,
                   int (*getter)(const uv_pipe_t*, char*, size_t*)) -> std::string {
  return runtime.call([handle, getter](uv_loop_t*) {
    std::size_t size = 256;
    while (true) {
      std::string out(size, '\0');
      std::size_t needed = out.size();
      const int rc = getter(handle, out.data(), &needed);
      if (rc == 0) {
        out.resize(needed);
        return out;
      }
      if (rc != UV_ENOBUFS) {
        throw_uv_error(rc, "uv_pipe_get*name");
      }
      size = needed > size ? needed : size * 2;
    }
  });
}

}  // namespace

pipe_client::pipe_client() : pipe_client(current_runtime()) {}

pipe_client::pipe_client(uv_runtime& runtime, bool ipc)
    : runtime_(&runtime), ipc_(ipc) {
  handle_ = runtime_->call([ipc](uv_loop_t* loop) {
    auto* handle = new uv_pipe_t{};
    const int rc = uv_pipe_init(loop, handle, ipc ? 1 : 0);
    if (rc < 0) {
      delete handle;
      throw_uv_error(rc, "uv_pipe_init");
    }
    return handle;
  });
}

pipe_client::pipe_client(uv_runtime& runtime,
                         uv_pipe_t* handle,
                         bool connected,
                         bool ipc) noexcept
    : runtime_(&runtime), handle_(handle), connected_(connected), ipc_(ipc) {}

pipe_client::~pipe_client() { close(); }

pipe_client::pipe_client(pipe_client&& other) noexcept
    : runtime_(other.runtime_),
      handle_(other.handle_),
      connected_(other.connected_),
      ipc_(other.ipc_) {
  other.runtime_ = nullptr;
  other.handle_ = nullptr;
  other.connected_ = false;
  other.ipc_ = false;
}

pipe_client& pipe_client::operator=(pipe_client&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  close();

  runtime_ = other.runtime_;
  handle_ = other.handle_;
  connected_ = other.connected_;
  ipc_ = other.ipc_;

  other.runtime_ = nullptr;
  other.handle_ = nullptr;
  other.connected_ = false;
  other.ipc_ = false;

  return *this;
}

void pipe_client::close() {
  if (runtime_ == nullptr || handle_ == nullptr) {
    return;
  }

  uv_pipe_t* handle = handle_;
  handle_ = nullptr;
  connected_ = false;

  try {
    runtime_->post([handle]() {
      if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(handle))) {
        uv_close(
            reinterpret_cast<uv_handle_t*>(handle),
            [](uv_handle_t* h) { delete reinterpret_cast<uv_pipe_t*>(h); });
      }
    });
  } catch (...) {
  }
}

coro::task<void> pipe_client::connect(const std::string& name,
                                      std::stop_token stop_token) {
  if (handle_ == nullptr) {
    throw std::runtime_error("pipe handle is null");
  }
  if (connected_) {
    throw std::runtime_error("pipe client is already connected");
  }

  struct state {
    pipe_client* client = nullptr;
    std::string name;
    uv_connect_t req{};
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

  struct connect_awaiter {
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
              op_state->client->runtime_->post([op_state]() {
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

      shared_state->client->runtime_->post([op_state = shared_state]() {
        if (op_state->completed.load(std::memory_order_acquire)) {
          return;
        }

        op_state->started = true;
        uv_pipe_connect(
            &op_state->req,
            op_state->client->handle_,
            op_state->name.c_str(),
            [](uv_connect_t* req, int connect_status) {
              auto* self = static_cast<state*>(req->data);
              self->status = connect_status;
              self->complete();
            });
      });

      return true;
    }

    void await_resume() const {
      if (shared_state->status == UV_ECANCELED ||
          (shared_state->cancel_requested && shared_state->status == 0)) {
        throw operation_cancelled();
      }
      if (shared_state->status < 0) {
        throw_uv_error(shared_state->status, "uv_pipe_connect");
      }
    }
  };

  auto shared_state = std::make_shared<state>();
  shared_state->client = this;
  shared_state->name = name;
  co_await connect_awaiter{std::move(shared_state), stop_token};
  connected_ = true;
}

coro::task<void> pipe_client::open(uv_file fd, bool readable, std::stop_token stop_token) {
  (void)readable;
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }
  if (fd < 0) {
    throw std::runtime_error("pipe file descriptor is invalid");
  }

  if (handle_ != nullptr) {
    close();
  }

  const uv_file dup_fd = duplicate_file_descriptor(fd);
  try {
    handle_ = runtime_->call([this, dup_fd](uv_loop_t* loop) {
      auto* handle = new uv_pipe_t{};
      const int init_rc = uv_pipe_init(loop, handle, ipc_ ? 1 : 0);
      if (init_rc < 0) {
        delete handle;
        throw_uv_error(init_rc, "uv_pipe_init");
      }

      const int open_rc = uv_pipe_open(handle, dup_fd);
      if (open_rc < 0) {
        delete handle;
        throw_uv_error(open_rc, "uv_pipe_open");
      }

      return handle;
    });
  } catch (...) {
    close_file_descriptor_noexcept(dup_fd);
    throw;
  }

  connected_ = true;
  co_return;
}

coro::task<std::size_t> pipe_client::write(std::string_view data,
                                           std::stop_token stop_token) {
  if (!connected_ || handle_ == nullptr) {
    throw std::runtime_error("pipe client is not connected");
  }

  co_return co_await detail::stream_write(
      *runtime_, handle_, std::string(data), stop_token);
}

coro::task<std::size_t> pipe_client::write_some(std::span<const char> data,
                                                std::stop_token stop_token) {
  if (!connected_ || handle_ == nullptr) {
    throw std::runtime_error("pipe client is not connected");
  }

  co_return co_await detail::stream_write_some(*runtime_, handle_, data, stop_token);
}

coro::task<std::string> pipe_client::read_some(std::size_t max_bytes,
                                               std::stop_token stop_token) {
  if (!connected_ || handle_ == nullptr) {
    throw std::runtime_error("pipe client is not connected");
  }

  co_return co_await detail::stream_read_some(*runtime_, handle_, max_bytes, stop_token);
}

coro::task<std::size_t> pipe_client::read_some_into(std::span<char> buffer,
                                                    std::stop_token stop_token) {
  if (!connected_ || handle_ == nullptr) {
    throw std::runtime_error("pipe client is not connected");
  }

  co_return co_await detail::stream_read_some_into(*runtime_, handle_, buffer, stop_token);
}

coro::task<std::string> pipe_client::read_exact(std::size_t total_bytes,
                                                std::stop_token stop_token) {
  std::string out;
  out.reserve(total_bytes);

  while (out.size() < total_bytes) {
    if (stop_token.stop_requested()) {
      throw operation_cancelled();
    }
    auto chunk = co_await read_some(total_bytes - out.size(), stop_token);
    if (chunk.empty()) {
      break;
    }
    out += chunk;
  }

  co_return out;
}

coro::task<std::size_t> pipe_client::read_exact_into(std::span<char> buffer,
                                                     std::stop_token stop_token) {
  std::size_t total = 0;

  while (total < buffer.size()) {
    if (stop_token.stop_requested()) {
      throw operation_cancelled();
    }
    auto nread = co_await read_some_into(buffer.subspan(total), stop_token);
    if (nread == 0) {
      break;
    }
    total += nread;
  }

  co_return total;
}

coro::task<void> pipe_client::shutdown(std::stop_token stop_token) {
  if (!connected_ || handle_ == nullptr) {
    throw std::runtime_error("pipe client is not connected");
  }

  co_await detail::stream_shutdown(*runtime_, handle_, stop_token);
}

auto pipe_client::local_name() const -> std::string {
  if (runtime_ == nullptr || handle_ == nullptr) {
    throw std::runtime_error("pipe handle is null");
  }

  return get_pipe_name(*runtime_, handle_, uv_pipe_getsockname);
}

auto pipe_client::peer_name() const -> std::string {
  if (runtime_ == nullptr || handle_ == nullptr) {
    throw std::runtime_error("pipe handle is null");
  }

  return get_pipe_name(*runtime_, handle_, uv_pipe_getpeername);
}

pipe_server::pipe_server() : pipe_server(current_runtime()) {}

pipe_server::pipe_server(uv_runtime& runtime, bool ipc)
    : runtime_(&runtime), ipc_(ipc) {
  handle_ = runtime_->call([this, ipc](uv_loop_t* loop) {
    auto* handle = new uv_pipe_t{};
    const int rc = uv_pipe_init(loop, handle, ipc ? 1 : 0);
    if (rc < 0) {
      delete handle;
      throw_uv_error(rc, "uv_pipe_init");
    }
    handle->data = this;
    return handle;
  });
}

pipe_server::~pipe_server() { close(); }

void pipe_server::close() {
  if (runtime_ == nullptr || handle_ == nullptr) {
    return;
  }

  uv_pipe_t* server_handle = handle_;
  handle_ = nullptr;
  bound_ = false;
  listening_ = false;

  try {
    runtime_->call([this, server_handle](uv_loop_t*) {
      if (accept_waiter_) {
        accept_status_ = UV_ECANCELED;
        auto waiter = accept_waiter_;
        accept_waiter_ = {};
        waiter.resume();
      }

      for (auto* client : pending_clients_) {
        if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(client))) {
          uv_close(
              reinterpret_cast<uv_handle_t*>(client),
              [](uv_handle_t* h) { delete reinterpret_cast<uv_pipe_t*>(h); });
        }
      }
      pending_clients_.clear();

      if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(server_handle))) {
        uv_close(
            reinterpret_cast<uv_handle_t*>(server_handle),
            [](uv_handle_t* h) { delete reinterpret_cast<uv_pipe_t*>(h); });
      }
    });
  } catch (...) {
  }
}

coro::task<void> pipe_server::bind(const std::string& name,
                                   std::stop_token stop_token) {
  if (handle_ == nullptr) {
    throw std::runtime_error("pipe server handle is null");
  }
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  runtime_->call([this, name](uv_loop_t*) {
    const int rc = uv_pipe_bind(handle_, name.c_str());
    if (rc < 0) {
      throw_uv_error(rc, "uv_pipe_bind");
    }
    bound_ = true;
  });
  co_return;
}

coro::task<void> pipe_server::set_pending_instances(int count,
                                                    std::stop_token stop_token) {
  if (handle_ == nullptr) {
    throw std::runtime_error("pipe server handle is null");
  }
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  runtime_->call([this, count](uv_loop_t*) { uv_pipe_pending_instances(handle_, count); });
  co_return;
}

coro::task<void> pipe_server::listen(int backlog, std::stop_token stop_token) {
  if (handle_ == nullptr) {
    throw std::runtime_error("pipe server handle is null");
  }
  if (!bound_) {
    throw std::runtime_error("pipe server is not bound");
  }
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  runtime_->call([this, backlog](uv_loop_t*) {
    const int rc =
        uv_listen(reinterpret_cast<uv_stream_t*>(handle_), backlog, &pipe_server::on_connection);
    if (rc < 0) {
      throw_uv_error(rc, "uv_listen");
    }
    listening_ = true;
  });
  co_return;
}

void pipe_server::on_connection(uv_stream_t* server, int status) {
  auto* self = static_cast<pipe_server*>(server->data);
  self->on_connection_impl(status);
}

void pipe_server::on_connection_impl(int status) {
  if (handle_ == nullptr) {
    return;
  }

  if (status < 0) {
    accept_status_ = status;
    if (accept_waiter_) {
      auto waiter = accept_waiter_;
      accept_waiter_ = {};
      waiter.resume();
    }
    return;
  }

  auto* client = new uv_pipe_t{};
  int rc = uv_pipe_init(runtime_->loop(), client, ipc_ ? 1 : 0);
  if (rc < 0) {
    delete client;
    accept_status_ = rc;
    if (accept_waiter_) {
      auto waiter = accept_waiter_;
      accept_waiter_ = {};
      waiter.resume();
    }
    return;
  }

  rc = uv_accept(reinterpret_cast<uv_stream_t*>(handle_),
                 reinterpret_cast<uv_stream_t*>(client));
  if (rc < 0) {
    uv_close(
        reinterpret_cast<uv_handle_t*>(client),
        [](uv_handle_t* h) { delete reinterpret_cast<uv_pipe_t*>(h); });
    accept_status_ = rc;
    if (accept_waiter_) {
      auto waiter = accept_waiter_;
      accept_waiter_ = {};
      waiter.resume();
    }
    return;
  }

  pending_clients_.push_back(client);
  accept_status_ = 0;
  if (accept_waiter_) {
    auto waiter = accept_waiter_;
    accept_waiter_ = {};
    waiter.resume();
  }
}

coro::task<pipe_client> pipe_server::accept(std::stop_token stop_token) {
  if (!listening_ || handle_ == nullptr) {
    throw std::runtime_error("pipe server is not listening");
  }

  struct accept_awaiter {
    pipe_server& server;
    std::stop_token stop_token;
    std::coroutine_handle<> continuation{};
    std::optional<stop_callback_t<std::function<void()>>> stop_callback{};
    int status = 0;

    bool await_ready() const noexcept {
      return stop_token.stop_requested() || !server.pending_clients_.empty() ||
             server.accept_status_ < 0;
    }

    bool await_suspend(std::coroutine_handle<> h) {
      continuation = h;
      if (stop_token.stop_requested()) {
        status = UV_ECANCELED;
        return false;
      }
      if (server.accept_waiter_) {
        status = UV_EBUSY;
        return false;
      }
      if (stop_token.stop_possible()) {
        stop_callback.emplace(stop_token, std::function<void()>(
            [this]() {
              server.runtime_->post([this]() {
                if (server.accept_waiter_ == continuation) {
                  server.accept_waiter_ = {};
                  server.accept_status_ = UV_ECANCELED;
                  auto waiter = continuation;
                  continuation = {};
                  if (waiter) {
                    waiter.resume();
                  }
                }
              });
            }));
      }
      server.accept_waiter_ = h;
      return true;
    }

    auto await_resume() -> pipe_client {
      stop_callback.reset();
      if (status == UV_ECANCELED || stop_token.stop_requested()) {
        throw operation_cancelled();
      }
      if (status < 0) {
        throw_uv_error(status, "pipe_server::accept");
      }

      if (server.accept_status_ < 0) {
        const int rc = server.accept_status_;
        server.accept_status_ = 0;
        if (rc == UV_ECANCELED) {
          throw operation_cancelled();
        }
        throw_uv_error(rc, "uv_accept");
      }

      if (server.pending_clients_.empty()) {
        throw std::runtime_error("accept resumed without pending pipe client");
      }

      uv_pipe_t* client_handle = server.pending_clients_.front();
      server.pending_clients_.pop_front();
      return pipe_client(*server.runtime_, client_handle, true, server.ipc_);
    }
  };

  co_return co_await accept_awaiter{*this, stop_token};
}

auto pipe_server::local_name() const -> std::string {
  if (runtime_ == nullptr || handle_ == nullptr) {
    throw std::runtime_error("pipe server handle is null");
  }

  return get_pipe_name(*runtime_, handle_, uv_pipe_getsockname);
}

}  // namespace luvcoro
