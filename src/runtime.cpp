#include "luvcoro/runtime.hpp"

#include <cstdlib>
#include <atomic>
#include <functional>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace luvcoro {
namespace {

thread_local uv_runtime* tls_current_runtime = nullptr;
std::mutex global_thread_pool_mutex;
std::optional<unsigned int> configured_thread_pool_size;
constexpr unsigned int kMaxThreadPoolSize = 1024;

void set_thread_pool_size_env(unsigned int size) {
  const auto value = std::to_string(size);
#ifdef _WIN32
  if (_putenv_s("UV_THREADPOOL_SIZE", value.c_str()) != 0) {
    throw std::runtime_error("failed to set UV_THREADPOOL_SIZE");
  }
#else
  if (setenv("UV_THREADPOOL_SIZE", value.c_str(), 1) != 0) {
    throw std::runtime_error("failed to set UV_THREADPOOL_SIZE");
  }
#endif
}

void configure_thread_pool(const uv_runtime_options& options) {
  if (options.thread_pool_size == 0) {
    return;
  }

  if (options.thread_pool_size > kMaxThreadPoolSize) {
    throw std::runtime_error("UV_THREADPOOL_SIZE must be between 1 and 1024");
  }

  std::lock_guard<std::mutex> lock(global_thread_pool_mutex);
  if (configured_thread_pool_size.has_value() &&
      *configured_thread_pool_size != options.thread_pool_size) {
    throw std::runtime_error(
        "libuv thread pool size is process-global and was already configured differently");
  }

  set_thread_pool_size_env(options.thread_pool_size);
  configured_thread_pool_size = options.thread_pool_size;
}

struct stop_wait_state : std::enable_shared_from_this<stop_wait_state> {
  uv_runtime* runtime = nullptr;
  std::coroutine_handle<> continuation{};
  std::atomic<bool> resumed = false;
  stop_wait_result result = stop_wait_result::requested;

  void resume_on_runtime() {
    if (resumed.exchange(true, std::memory_order_acq_rel)) {
      return;
    }

    auto self = shared_from_this();
    runtime->post([self = std::move(self)]() {
      auto continuation = self->continuation;
      self->continuation = {};
      if (continuation) {
        continuation.resume();
      }
    });
  }
};

struct timer_wait_state : std::enable_shared_from_this<timer_wait_state> {
  struct timer_ctx {
    std::shared_ptr<timer_wait_state> owner{};
    uv_timer_t timer{};
    bool closing = false;
  };

  uv_runtime* runtime = nullptr;
  std::coroutine_handle<> continuation{};
  timer_ctx* ctx = nullptr;
  std::atomic<bool> resumed = false;
  bool completed = false;
  bool cancelled = false;
  int status = 0;

  void start(std::chrono::milliseconds delay) {
    if (completed) {
      return;
    }

    auto* new_ctx = new timer_ctx{};
    new_ctx->owner = shared_from_this();
    std::memset(&new_ctx->timer, 0, sizeof(uv_timer_t));

    int rc = uv_timer_init(runtime->loop(), &new_ctx->timer);
    if (rc < 0) {
      delete new_ctx;
      fail(rc);
      return;
    }

    new_ctx->timer.data = new_ctx;
    ctx = new_ctx;
    rc = uv_timer_start(
        &new_ctx->timer,
        [](uv_timer_t* handle) {
          auto* ctx = static_cast<timer_ctx*>(handle->data);
          ctx->owner->fire();
        },
        static_cast<uint64_t>(delay.count()),
        0);
    if (rc < 0) {
      close_timer();
      fail(rc);
    }
  }

  void fire() {
    auto keep_alive = shared_from_this();
    if (completed) {
      return;
    }

    completed = true;
    status = 0;
    close_timer();
    resume_on_runtime();
  }

  void cancel() {
    auto keep_alive = shared_from_this();
    if (completed) {
      return;
    }

    completed = true;
    cancelled = true;
    close_timer();
    resume_on_runtime();
  }

  void fail(int rc) {
    auto keep_alive = shared_from_this();
    if (completed) {
      return;
    }

    completed = true;
    status = rc;
    close_timer();
    resume_on_runtime();
  }

  void resume_on_runtime() {
    if (resumed.exchange(true, std::memory_order_acq_rel)) {
      return;
    }

    auto self = shared_from_this();
    runtime->post([self = std::move(self)]() {
      auto continuation = self->continuation;
      self->continuation = {};
      if (continuation) {
        continuation.resume();
      }
    });
  }

  void close_timer() {
    if (ctx == nullptr || ctx->closing) {
      return;
    }

    auto* closing_ctx = ctx;
    closing_ctx->closing = true;
    uv_timer_stop(&closing_ctx->timer);
    uv_close(reinterpret_cast<uv_handle_t*>(&closing_ctx->timer), [](uv_handle_t* handle) {
      auto* ctx = static_cast<timer_ctx*>(handle->data);
      if (ctx->owner != nullptr && ctx->owner->ctx == ctx) {
        ctx->owner->ctx = nullptr;
      }
      delete ctx;
    });
  }
};

}  // namespace

uv_runtime::uv_runtime() : uv_runtime(uv_runtime_options{}) {}

uv_runtime::uv_runtime(uv_runtime_options options) : options_(options) {
  configure_thread_pool(options_);

  int rc = uv_loop_init(&loop_);
  if (rc < 0) {
    throw_uv_error(rc, "uv_loop_init");
  }

  rc = uv_async_init(&loop_, &async_, &uv_runtime::on_async);
  if (rc < 0) {
    uv_loop_close(&loop_);
    throw_uv_error(rc, "uv_async_init");
  }
  async_.data = this;

  loop_thread_ = std::thread([this]() {
    runtime_scope scope(*this);
    uv_run(&loop_, UV_RUN_DEFAULT);
    uv_loop_close(&loop_);
  });
}

uv_runtime::~uv_runtime() {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (shutting_down_) {
      if (loop_thread_.joinable()) {
        loop_thread_.join();
      }
      return;
    }
    shutting_down_ = true;
  }

  auto done = std::make_shared<std::promise<void>>();
  auto future = done->get_future();

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.push([this, done]() {
      uv_walk(
          &loop_,
          [](uv_handle_t* handle, void*) {
            if (!uv_is_closing(handle)) {
              uv_close(handle, nullptr);
            }
          },
          nullptr);
      done->set_value();
    });
  }

  uv_async_send(&async_);
  future.wait();

  if (loop_thread_.joinable()) {
    loop_thread_.join();
  }
}

void uv_runtime::post(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (shutting_down_) {
      throw std::runtime_error("uv_runtime is shutting down");
    }
    queue_.push(std::move(fn));
  }

  int rc = uv_async_send(&async_);
  if (rc < 0) {
    throw_uv_error(rc, "uv_async_send");
  }
}

void uv_runtime::on_async(uv_async_t* handle) {
  auto* self = static_cast<uv_runtime*>(handle->data);
  self->drain();
}

void uv_runtime::drain() {
  std::queue<std::function<void()>> local;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    local.swap(queue_);
  }

  while (!local.empty()) {
    auto fn = std::move(local.front());
    local.pop();
    fn();
  }
}

runtime_scope::runtime_scope(uv_runtime& runtime) noexcept : previous_(tls_current_runtime) {
  tls_current_runtime = &runtime;
}

runtime_scope::~runtime_scope() {
  tls_current_runtime = previous_;
}

auto try_current_runtime() noexcept -> uv_runtime* {
  return tls_current_runtime;
}

auto current_runtime() -> uv_runtime& {
  if (tls_current_runtime == nullptr) {
    throw std::runtime_error("no current luvcoro::uv_runtime bound to this thread");
  }
  return *tls_current_runtime;
}

coro::task<void> wait_for_stop(uv_runtime& runtime, std::stop_token stop_token) {
  struct awaiter {
    uv_runtime& runtime;
    std::stop_token stop_token;
    std::shared_ptr<stop_wait_state> state{std::make_shared<stop_wait_state>()};
    using callback_t = std::function<void()>;
    std::optional<std::stop_callback<callback_t>> callback{};

    awaiter(uv_runtime& runtime, std::stop_token stop_token)
        : runtime(runtime), stop_token(stop_token) {
      state->runtime = &runtime;
    }

    bool await_ready() const noexcept { return stop_token.stop_requested(); }

    bool await_suspend(std::coroutine_handle<> h) {
      if (stop_token.stop_requested()) {
        return false;
      }

      state->continuation = h;
      callback.emplace(stop_token, callback_t([state = state]() {
                         state->result = stop_wait_result::requested;
                         state->resume_on_runtime();
                       }));
      return !state->resumed.load(std::memory_order_acquire);
    }

    void await_resume() const noexcept {}
  };

  co_await awaiter{runtime, stop_token};
}

coro::task<stop_wait_result> wait_for_stop(uv_runtime& runtime,
                                           std::stop_token stop_token,
                                           std::stop_token cancel_token) {
  struct awaiter {
    uv_runtime& runtime;
    std::stop_token stop_token;
    std::stop_token cancel_token;
    std::shared_ptr<stop_wait_state> state{std::make_shared<stop_wait_state>()};
    using callback_t = std::function<void()>;
    std::optional<std::stop_callback<callback_t>> stop_callback{};
    std::optional<std::stop_callback<callback_t>> cancel_callback{};

    awaiter(uv_runtime& runtime, std::stop_token stop_token, std::stop_token cancel_token)
        : runtime(runtime), stop_token(stop_token), cancel_token(cancel_token) {
      state->runtime = &runtime;
    }

    bool await_ready() const noexcept {
      return stop_token.stop_requested() || cancel_token.stop_requested();
    }

    bool await_suspend(std::coroutine_handle<> h) {
      if (stop_token.stop_requested()) {
        state->result = stop_wait_result::requested;
        return false;
      }
      if (cancel_token.stop_requested()) {
        state->result = stop_wait_result::cancelled;
        return false;
      }

      state->continuation = h;
      stop_callback.emplace(stop_token, callback_t([state = state]() {
                              state->result = stop_wait_result::requested;
                              state->resume_on_runtime();
                            }));
      cancel_callback.emplace(cancel_token, callback_t([state = state]() {
                                state->result = stop_wait_result::cancelled;
                                state->resume_on_runtime();
                              }));
      return !state->resumed.load(std::memory_order_acquire);
    }

    auto await_resume() const noexcept -> stop_wait_result {
      if (stop_token.stop_requested()) {
        return stop_wait_result::requested;
      }
      if (cancel_token.stop_requested() &&
          !state->resumed.load(std::memory_order_acquire)) {
        return stop_wait_result::cancelled;
      }
      return state->result;
    }
  };

  co_return co_await awaiter{runtime, stop_token, cancel_token};
}

coro::task<void> sleep_for(uv_runtime& runtime, std::chrono::milliseconds delay) {
  co_await sleep_for(runtime, delay, {});
}

coro::task<void> sleep_for(uv_runtime& runtime,
                           std::chrono::milliseconds delay,
                           std::stop_token stop_token) {
  struct awaiter {
    uv_runtime& runtime;
    std::chrono::milliseconds delay;
    std::stop_token stop_token;
    std::shared_ptr<timer_wait_state> state{std::make_shared<timer_wait_state>()};
    using callback_t = std::function<void()>;
    std::optional<std::stop_callback<callback_t>> stop_callback{};

    awaiter(uv_runtime& runtime, std::chrono::milliseconds delay, std::stop_token stop_token)
        : runtime(runtime), delay(delay), stop_token(stop_token) {
      state->runtime = &runtime;
    }

    bool await_ready() const noexcept {
      return delay.count() <= 0 && !stop_token.stop_requested();
    }

    bool await_suspend(std::coroutine_handle<> h) {
      if (stop_token.stop_requested()) {
        state->cancelled = true;
        state->completed = true;
        return false;
      }

      state->continuation = h;
      if (stop_token.stop_possible()) {
        stop_callback.emplace(stop_token, callback_t([state = state]() {
                                state->runtime->post([state]() {
                                  state->cancel();
                                });
                              }));
      }

      runtime.post([state = state, delay = delay]() {
        state->start(delay);
      });

      return !state->resumed.load(std::memory_order_acquire);
    }

    void await_resume() {
      if (state->cancelled || stop_token.stop_requested()) {
        throw operation_cancelled();
      }
      if (state->status < 0) {
        throw_uv_error(state->status, "uv_timer_start");
      }
    }
  };

  co_await awaiter{runtime, delay, stop_token};
}

coro::task<void> sleep_until(uv_runtime& runtime,
                             std::chrono::steady_clock::time_point deadline) {
  co_await sleep_until(runtime, deadline, {});
}

coro::task<void> sleep_until(uv_runtime& runtime,
                             std::chrono::steady_clock::time_point deadline,
                             std::stop_token stop_token) {
  const auto now = std::chrono::steady_clock::now();
  if (deadline <= now) {
    if (stop_token.stop_requested()) {
      throw operation_cancelled();
    }
    co_return;
  }

  co_await sleep_for(
      runtime,
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now),
      stop_token);
}

steady_timer::steady_timer() : steady_timer(current_runtime()) {}

steady_timer::steady_timer(uv_runtime& runtime) : runtime_(&runtime) {}

auto steady_timer::wait() -> coro::task<void> {
  co_await sleep_until(*runtime_, expiry_, cancel_source_.get_token());
}

}  // namespace luvcoro
