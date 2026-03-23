#pragma once

#include <atomic>
#include <chrono>
#include <coroutine>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#include <coro/coro.hpp>
#include <coro/detail/task_self_deleting.hpp>
#include <uv.h>

#include "luvcoro/error.hpp"

namespace luvcoro {

struct uv_runtime_options {
  unsigned int thread_pool_size = 0;
};

class uv_runtime {
 public:
  uv_runtime();
  explicit uv_runtime(uv_runtime_options options);
  ~uv_runtime();

  uv_runtime(const uv_runtime&) = delete;
  uv_runtime& operator=(const uv_runtime&) = delete;

  uv_loop_t* loop() noexcept { return &loop_; }
  auto options() const noexcept -> const uv_runtime_options& { return options_; }
  bool is_loop_thread() const noexcept {
    return std::this_thread::get_id() == loop_thread_.get_id();
  }

  void post(std::function<void()> fn);

  template <typename Fn>
  auto call(Fn&& fn) -> decltype(fn(std::declval<uv_loop_t*>())) {
    using result_t = decltype(fn(std::declval<uv_loop_t*>()));

    if (is_loop_thread()) {
      if constexpr (std::is_void_v<result_t>) {
        fn(&loop_);
        return;
      } else {
        return fn(&loop_);
      }
    }

    auto promise = std::make_shared<std::promise<result_t>>();
    auto future = promise->get_future();

    post([this, promise, fn = std::forward<Fn>(fn)]() mutable {
      try {
        if constexpr (std::is_void_v<result_t>) {
          fn(&loop_);
          promise->set_value();
        } else {
          promise->set_value(fn(&loop_));
        }
      } catch (...) {
        promise->set_exception(std::current_exception());
      }
    });

    return future.get();
  }

 private:
  static void on_async(uv_async_t* handle);
  void drain();

  uv_loop_t loop_{};
  uv_async_t async_{};
  std::thread loop_thread_{};

  std::mutex queue_mutex_;
  std::queue<std::function<void()>> queue_;
  bool shutting_down_ = false;
  uv_runtime_options options_{};
};

class runtime_scope {
 public:
  explicit runtime_scope(uv_runtime& runtime) noexcept;
  ~runtime_scope();

  runtime_scope(const runtime_scope&) = delete;
  runtime_scope& operator=(const runtime_scope&) = delete;
  runtime_scope(runtime_scope&&) = delete;
  runtime_scope& operator=(runtime_scope&&) = delete;

 private:
  uv_runtime* previous_;
};

auto try_current_runtime() noexcept -> uv_runtime*;
auto current_runtime() -> uv_runtime&;

enum class stop_wait_result {
  requested,
  cancelled,
};

class steady_timer;
coro::task<void> wait_for_stop(uv_runtime& runtime, std::stop_token stop_token);
coro::task<stop_wait_result> wait_for_stop(uv_runtime& runtime,
                                           std::stop_token stop_token,
                                           std::stop_token cancel_token);
coro::task<void> sleep_for(uv_runtime& runtime,
                           std::chrono::milliseconds delay);
coro::task<void> sleep_for(uv_runtime& runtime,
                           std::chrono::milliseconds delay,
                           std::stop_token stop_token);
coro::task<void> sleep_until(uv_runtime& runtime,
                             std::chrono::steady_clock::time_point deadline);
coro::task<void> sleep_until(uv_runtime& runtime,
                             std::chrono::steady_clock::time_point deadline,
                             std::stop_token stop_token);

using detached_exception_handler =
    std::function<void(std::string_view, std::exception_ptr)>;

class periodic_task {
 public:
  periodic_task() = default;
  explicit periodic_task(std::shared_ptr<std::stop_source> stop_source) noexcept
      : stop_source_(std::move(stop_source)) {}

  explicit operator bool() const noexcept {
    return static_cast<bool>(stop_source_);
  }

  auto valid() const noexcept -> bool {
    return static_cast<bool>(*this);
  }

  void cancel() noexcept {
    if (stop_source_ != nullptr) {
      stop_source_->request_stop();
    }
  }

  auto stop_requested() const noexcept -> bool {
    return stop_source_ != nullptr && stop_source_->stop_requested();
  }

  auto stop_token() const noexcept -> std::stop_token {
    if (stop_source_ == nullptr) {
      return {};
    }
    return stop_source_->get_token();
  }

 private:
  std::shared_ptr<std::stop_source> stop_source_{};
};

template <typename Fn>
decltype(auto) with_runtime(uv_runtime& runtime, Fn&& fn) {
  runtime_scope scope(runtime);
  return std::forward<Fn>(fn)();
}

template <typename Awaitable>
auto run_with_runtime(uv_runtime& runtime, Awaitable&& awaitable) {
  return with_runtime(runtime, [&]() {
    return coro::sync_wait(std::forward<Awaitable>(awaitable));
  });
}

namespace detail {

template <typename Fn>
concept blocking_token_invocable =
    requires(std::decay_t<Fn>& fn, std::stop_token stop_token) {
      fn(stop_token);
    };

template <typename Fn>
concept blocking_plain_invocable = requires(std::decay_t<Fn>& fn) {
  fn();
};

template <typename Result>
struct blocking_result_storage {
  std::optional<Result> value{};
};

template <>
struct blocking_result_storage<void> {};

template <typename Fn, bool UseStopToken>
struct blocking_result;

template <typename Fn>
struct blocking_result<Fn, true> {
  using type = std::invoke_result_t<std::decay_t<Fn>&, std::stop_token>;
};

template <typename Fn>
struct blocking_result<Fn, false> {
  using type = std::invoke_result_t<std::decay_t<Fn>&>;
};

template <typename Fn>
using blocking_result_t = typename blocking_result<Fn, blocking_token_invocable<Fn>>::type;

template <typename Fn>
decltype(auto) invoke_blocking(std::decay_t<Fn>& fn, std::stop_token stop_token) {
  if constexpr (blocking_token_invocable<Fn>) {
    return std::invoke(fn, stop_token);
  } else {
    return std::invoke(fn);
  }
}

}  // namespace detail

template <typename Fn>
auto blocking(uv_runtime& runtime, std::stop_token stop_token, Fn&& fn)
    -> coro::task<detail::blocking_result_t<Fn>> {
  static_assert(detail::blocking_token_invocable<Fn> ||
                    detail::blocking_plain_invocable<Fn>,
                "luvcoro::blocking requires a callable invocable as fn() or fn(std::stop_token)");

  using fn_t = std::decay_t<Fn>;
  using result_t = detail::blocking_result_t<Fn>;

  static_assert(!std::is_reference_v<result_t>,
                "luvcoro::blocking does not support reference return types");

  struct state {
    uv_runtime* runtime = nullptr;
    std::stop_token stop_token{};
    fn_t fn;
    uv_work_t req{};
    std::coroutine_handle<> continuation{};
    std::shared_ptr<state> keep_alive{};
    std::optional<std::stop_callback<std::function<void()>>> stop_callback{};
    std::atomic<bool> completed = false;
    std::atomic<bool> work_started = false;
    std::exception_ptr exception{};
    int queue_status = 0;
    int after_status = 0;
    bool queue_started = false;
    bool cancelled = false;
    detail::blocking_result_storage<result_t> result{};

    state(uv_runtime* runtime, std::stop_token stop_token, fn_t fn)
        : runtime(runtime),
          stop_token(stop_token),
          fn(std::move(fn)) {}

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

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) {
      if (shared_state->stop_token.stop_requested()) {
        shared_state->cancelled = true;
        return false;
      }

      shared_state->continuation = h;
      shared_state->keep_alive = shared_state;
      shared_state->req.data = shared_state.get();

      try {
        if (shared_state->stop_token.stop_possible()) {
          shared_state->stop_callback.emplace(shared_state->stop_token,
                                             std::function<void()>([op_state = shared_state]() {
                                               op_state->runtime->post([op_state]() {
                                                 if (op_state->completed.load(
                                                         std::memory_order_acquire)) {
                                                   return;
                                                 }

                                                 if (!op_state->queue_started) {
                                                   op_state->cancelled = true;
                                                   op_state->complete();
                                                   return;
                                                 }

                                                 const int rc = uv_cancel(
                                                     reinterpret_cast<uv_req_t*>(&op_state->req));
                                                 if (rc == 0) {
                                                   return;
                                                 }
                                                 if (rc == UV_EBUSY || rc == UV_ENOSYS) {
                                                   return;
                                                 }
                                               });
                                             }));
        }

        shared_state->runtime->post([op_state = shared_state]() {
          if (op_state->completed.load(std::memory_order_acquire)) {
            return;
          }

          op_state->queue_started = true;
          int rc = uv_queue_work(
              op_state->runtime->loop(), &op_state->req,
              [](uv_work_t* req) {
                auto* raw = static_cast<state*>(req->data);
                raw->work_started.store(true, std::memory_order_release);
                try {
                  if constexpr (std::is_void_v<result_t>) {
                    detail::invoke_blocking<Fn>(raw->fn, raw->stop_token);
                  } else {
                    raw->result.value.emplace(
                        detail::invoke_blocking<Fn>(raw->fn, raw->stop_token));
                  }
                } catch (...) {
                  raw->exception = std::current_exception();
                }
              },
              [](uv_work_t* req, int status) {
                auto* raw = static_cast<state*>(req->data);
                raw->after_status = status;
                if (status == UV_ECANCELED) {
                  raw->cancelled = true;
                }
                raw->complete();
              });

          if (rc < 0) {
            op_state->queue_status = rc;
            op_state->complete();
          }
        });
      } catch (...) {
        shared_state->exception = std::current_exception();
        return false;
      }

      return true;
    }

    auto await_resume() -> result_t {
      auto state = std::move(shared_state);

      if (state->cancelled || state->after_status == UV_ECANCELED) {
        throw operation_cancelled();
      }
      if (state->queue_status < 0) {
        throw_uv_error(state->queue_status, "uv_queue_work");
      }
      if (state->after_status < 0) {
        throw_uv_error(state->after_status, "uv_queue_work");
      }
      if (state->exception != nullptr) {
        std::rethrow_exception(state->exception);
      }

      if constexpr (std::is_void_v<result_t>) {
        return;
      } else {
        return std::move(*state->result.value);
      }
    }
  };

  auto shared_state =
      std::make_shared<state>(&runtime, stop_token, std::forward<Fn>(fn));
  co_return co_await awaiter{std::move(shared_state)};
}

template <typename Fn>
auto blocking(uv_runtime& runtime, Fn&& fn) -> coro::task<detail::blocking_result_t<Fn>> {
  co_return co_await blocking(runtime, std::stop_token{}, std::forward<Fn>(fn));
}

template <typename Fn>
auto blocking(std::stop_token stop_token, Fn&& fn)
    -> coro::task<detail::blocking_result_t<Fn>> {
  co_return co_await blocking(current_runtime(), stop_token, std::forward<Fn>(fn));
}

template <typename Fn>
auto blocking(Fn&& fn) -> coro::task<detail::blocking_result_t<Fn>> {
  co_return co_await blocking(current_runtime(), std::stop_token{}, std::forward<Fn>(fn));
}

namespace detail {

template <typename Fn>
concept periodic_task_factory =
    requires(std::decay_t<Fn>& fn) {
      { fn() } -> std::same_as<coro::task<void>>;
    } ||
    requires(std::decay_t<Fn>& fn, std::stop_token token) {
      { fn(token) } -> std::same_as<coro::task<void>>;
    };

template <typename Fn>
auto make_spawn_task_impl(std::decay_t<Fn> callable) -> coro::task<void> {
  co_await std::move(callable)();
}

template <typename Fn>
auto make_spawn_task(Fn&& fn) -> coro::task<void> {
  return make_spawn_task_impl<std::decay_t<Fn>>(std::forward<Fn>(fn));
}

inline void default_spawn_exception_handler(std::string_view name,
                                            std::exception_ptr exception) noexcept {
  try {
    if (exception != nullptr) {
      std::rethrow_exception(exception);
    }
    std::cerr << "luvcoro detached task";
    if (!name.empty()) {
      std::cerr << " [" << name << "]";
    }
    std::cerr << " failed with an unknown exception" << std::endl;
  } catch (const std::exception& ex) {
    std::cerr << "luvcoro detached task";
    if (!name.empty()) {
      std::cerr << " [" << name << "]";
    }
    std::cerr << " failed: " << ex.what() << std::endl;
  } catch (...) {
    std::cerr << "luvcoro detached task";
    if (!name.empty()) {
      std::cerr << " [" << name << "]";
    }
    std::cerr << " failed with a non-standard exception" << std::endl;
  }
}

inline auto make_spawn_logged_task(std::string name,
                                   detached_exception_handler handler,
                                   coro::task<void> task) -> coro::task<void> {
  try {
    co_await task;
  } catch (...) {
    try {
      if (handler) {
        handler(name, std::current_exception());
      }
    } catch (...) {
    }
  }
}

template <typename Rep, typename Period>
inline auto make_spawn_after_task(std::chrono::duration<Rep, Period> delay,
                                  coro::task<void> task) -> coro::task<void> {
  co_await sleep_for(current_runtime(),
                     std::chrono::duration_cast<std::chrono::milliseconds>(delay));
  co_await task;
}

template <typename Clock, typename Duration>
inline auto make_spawn_at_task(std::chrono::time_point<Clock, Duration> deadline,
                               coro::task<void> task) -> coro::task<void> {
  const auto now = Clock::now();
  if (deadline > now) {
    const auto delay =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    co_await sleep_for(current_runtime(), delay);
  }
  co_await task;
}

template <typename Fn>
requires periodic_task_factory<Fn>
auto make_spawn_every_task_impl(std::chrono::milliseconds interval,
                                std::shared_ptr<std::stop_source> stop_source,
                                std::decay_t<Fn> callable) -> coro::task<void> {
  auto stop_token = stop_source->get_token();

  try {
    while (!stop_token.stop_requested()) {
      co_await sleep_for(current_runtime(), interval, stop_token);
      if (stop_token.stop_requested()) {
        break;
      }

      if constexpr (requires(decltype(callable)& op, std::stop_token token) {
                      { op(token) } -> std::same_as<coro::task<void>>;
                    }) {
        co_await callable(stop_token);
      } else {
        co_await callable();
      }
    }
  } catch (const operation_cancelled&) {
    co_return;
  }
}

template <typename Fn>
requires periodic_task_factory<Fn>
auto make_spawn_every_task(std::chrono::milliseconds interval,
                           std::shared_ptr<std::stop_source> stop_source,
                           Fn&& fn) -> coro::task<void> {
  return make_spawn_every_task_impl<Fn>(
      interval, std::move(stop_source), std::forward<Fn>(fn));
}

template <typename Fn>
requires periodic_task_factory<Fn>
auto make_spawn_every_immediate_task_impl(std::chrono::milliseconds interval,
                                          std::shared_ptr<std::stop_source> stop_source,
                                          std::decay_t<Fn> callable)
    -> coro::task<void> {
  auto stop_token = stop_source->get_token();

  try {
    while (!stop_token.stop_requested()) {
      if constexpr (requires(decltype(callable)& op, std::stop_token token) {
                      { op(token) } -> std::same_as<coro::task<void>>;
                    }) {
        co_await callable(stop_token);
      } else {
        co_await callable();
      }
      if (stop_token.stop_requested()) {
        break;
      }

      co_await sleep_for(current_runtime(), interval, stop_token);
    }
  } catch (const operation_cancelled&) {
    co_return;
  }
}

template <typename Fn>
requires periodic_task_factory<Fn>
auto make_spawn_every_immediate_task(std::chrono::milliseconds interval,
                                     std::shared_ptr<std::stop_source> stop_source,
                                     Fn&& fn) -> coro::task<void> {
  return make_spawn_every_immediate_task_impl<Fn>(
      interval, std::move(stop_source), std::forward<Fn>(fn));
}

template <typename Fn>
requires periodic_task_factory<Fn>
auto make_spawn_every_fixed_rate_task_impl(std::chrono::milliseconds interval,
                                           std::shared_ptr<std::stop_source> stop_source,
                                           std::decay_t<Fn> callable)
    -> coro::task<void> {
  auto stop_token = stop_source->get_token();
  auto next_deadline = std::chrono::steady_clock::now() + interval;

  try {
    while (!stop_token.stop_requested()) {
      auto now = std::chrono::steady_clock::now();
      while (next_deadline <= now) {
        next_deadline += interval;
      }

      co_await sleep_until(current_runtime(), next_deadline, stop_token);
      if (stop_token.stop_requested()) {
        break;
      }

      if constexpr (requires(decltype(callable)& op, std::stop_token token) {
                      { op(token) } -> std::same_as<coro::task<void>>;
                    }) {
        co_await callable(stop_token);
      } else {
        co_await callable();
      }
      next_deadline += interval;
    }
  } catch (const operation_cancelled&) {
    co_return;
  }
}

template <typename Fn>
requires periodic_task_factory<Fn>
auto make_spawn_every_fixed_rate_task(std::chrono::milliseconds interval,
                                      std::shared_ptr<std::stop_source> stop_source,
                                      Fn&& fn) -> coro::task<void> {
  return make_spawn_every_fixed_rate_task_impl<Fn>(
      interval, std::move(stop_source), std::forward<Fn>(fn));
}

inline auto spawn_detached(uv_runtime& runtime, coro::task<void> task) noexcept -> bool {
  auto detached = coro::detail::make_task_self_deleting(std::move(task));
  auto handle = detached.handle();

  if (runtime.is_loop_thread()) {
    if (!handle.done()) {
      handle.resume();
    }
    return true;
  }

  try {
    runtime.post([handle]() mutable {
      if (!handle.done()) {
        handle.resume();
      }
    });
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace detail

inline auto spawn(uv_runtime& runtime, coro::task<void> task) noexcept -> bool {
  return detail::spawn_detached(runtime, std::move(task));
}

inline auto spawn(coro::task<void> task) noexcept -> bool {
  auto* runtime = try_current_runtime();
  if (runtime == nullptr) {
    return false;
  }

  return detail::spawn_detached(*runtime, std::move(task));
}

template <typename Fn>
requires std::same_as<std::invoke_result_t<std::decay_t<Fn>&>, coro::task<void>>
inline auto spawn(uv_runtime& runtime, Fn&& fn) noexcept -> bool {
  return spawn(runtime, detail::make_spawn_task(std::forward<Fn>(fn)));
}

template <typename Fn>
requires std::same_as<std::invoke_result_t<std::decay_t<Fn>&>, coro::task<void>>
inline auto spawn(Fn&& fn) noexcept -> bool {
  return spawn(detail::make_spawn_task(std::forward<Fn>(fn)));
}

inline auto spawn_logged(uv_runtime& runtime,
                         std::string_view name,
                         coro::task<void> task,
                         detached_exception_handler handler =
                             detail::default_spawn_exception_handler) noexcept
    -> bool {
  return spawn(runtime,
               detail::make_spawn_logged_task(
                   std::string(name), std::move(handler), std::move(task)));
}

inline auto spawn_logged(std::string_view name,
                         coro::task<void> task,
                         detached_exception_handler handler =
                             detail::default_spawn_exception_handler) noexcept
    -> bool {
  return spawn(detail::make_spawn_logged_task(
      std::string(name), std::move(handler), std::move(task)));
}

template <typename Fn>
requires std::same_as<std::invoke_result_t<std::decay_t<Fn>&>, coro::task<void>>
inline auto spawn_logged(uv_runtime& runtime,
                         std::string_view name,
                         Fn&& fn,
                         detached_exception_handler handler =
                             detail::default_spawn_exception_handler) noexcept
    -> bool {
  return spawn_logged(runtime,
                      name,
                      detail::make_spawn_task(std::forward<Fn>(fn)),
                      std::move(handler));
}

template <typename Fn>
requires std::same_as<std::invoke_result_t<std::decay_t<Fn>&>, coro::task<void>>
inline auto spawn_logged(std::string_view name,
                         Fn&& fn,
                         detached_exception_handler handler =
                             detail::default_spawn_exception_handler) noexcept
    -> bool {
  return spawn_logged(name,
                      detail::make_spawn_task(std::forward<Fn>(fn)),
                      std::move(handler));
}

template <typename Rep, typename Period>
inline auto spawn_after(uv_runtime& runtime,
                        std::chrono::duration<Rep, Period> delay,
                        coro::task<void> task) noexcept -> bool {
  return spawn(runtime, detail::make_spawn_after_task(delay, std::move(task)));
}

template <typename Rep, typename Period>
inline auto spawn_after(std::chrono::duration<Rep, Period> delay,
                        coro::task<void> task) noexcept -> bool {
  return spawn(detail::make_spawn_after_task(delay, std::move(task)));
}

template <typename Rep, typename Period, typename Fn>
requires std::same_as<std::invoke_result_t<std::decay_t<Fn>&>, coro::task<void>>
inline auto spawn_after(uv_runtime& runtime,
                        std::chrono::duration<Rep, Period> delay,
                        Fn&& fn) noexcept -> bool {
  return spawn_after(runtime,
                     delay,
                     detail::make_spawn_task(std::forward<Fn>(fn)));
}

template <typename Rep, typename Period, typename Fn>
requires std::same_as<std::invoke_result_t<std::decay_t<Fn>&>, coro::task<void>>
inline auto spawn_after(std::chrono::duration<Rep, Period> delay,
                        Fn&& fn) noexcept -> bool {
  return spawn_after(delay, detail::make_spawn_task(std::forward<Fn>(fn)));
}

template <typename Clock, typename Duration>
inline auto spawn_at(uv_runtime& runtime,
                     std::chrono::time_point<Clock, Duration> deadline,
                     coro::task<void> task) noexcept -> bool {
  return spawn(runtime, detail::make_spawn_at_task(deadline, std::move(task)));
}

template <typename Clock, typename Duration>
inline auto spawn_at(std::chrono::time_point<Clock, Duration> deadline,
                     coro::task<void> task) noexcept -> bool {
  return spawn(detail::make_spawn_at_task(deadline, std::move(task)));
}

template <typename Clock, typename Duration, typename Fn>
requires std::same_as<std::invoke_result_t<std::decay_t<Fn>&>, coro::task<void>>
inline auto spawn_at(uv_runtime& runtime,
                     std::chrono::time_point<Clock, Duration> deadline,
                     Fn&& fn) noexcept -> bool {
  return spawn_at(runtime,
                  deadline,
                  detail::make_spawn_task(std::forward<Fn>(fn)));
}

template <typename Clock, typename Duration, typename Fn>
requires std::same_as<std::invoke_result_t<std::decay_t<Fn>&>, coro::task<void>>
inline auto spawn_at(std::chrono::time_point<Clock, Duration> deadline,
                     Fn&& fn) noexcept -> bool {
  return spawn_at(deadline, detail::make_spawn_task(std::forward<Fn>(fn)));
}

template <typename Rep, typename Period>
inline auto spawn_logged_after(uv_runtime& runtime,
                               std::string_view name,
                               std::chrono::duration<Rep, Period> delay,
                               coro::task<void> task,
                               detached_exception_handler handler =
                                   detail::default_spawn_exception_handler) noexcept
    -> bool {
  return spawn_logged(runtime,
                      name,
                      detail::make_spawn_after_task(delay, std::move(task)),
                      std::move(handler));
}

template <typename Rep, typename Period>
inline auto spawn_logged_after(std::string_view name,
                               std::chrono::duration<Rep, Period> delay,
                               coro::task<void> task,
                               detached_exception_handler handler =
                                   detail::default_spawn_exception_handler) noexcept
    -> bool {
  return spawn_logged(name,
                      detail::make_spawn_after_task(delay, std::move(task)),
                      std::move(handler));
}

template <typename Rep, typename Period, typename Fn>
requires std::same_as<std::invoke_result_t<std::decay_t<Fn>&>, coro::task<void>>
inline auto spawn_logged_after(uv_runtime& runtime,
                               std::string_view name,
                               std::chrono::duration<Rep, Period> delay,
                               Fn&& fn,
                               detached_exception_handler handler =
                                   detail::default_spawn_exception_handler) noexcept
    -> bool {
  return spawn_logged_after(runtime,
                            name,
                            delay,
                            detail::make_spawn_task(std::forward<Fn>(fn)),
                            std::move(handler));
}

template <typename Rep, typename Period, typename Fn>
requires std::same_as<std::invoke_result_t<std::decay_t<Fn>&>, coro::task<void>>
inline auto spawn_logged_after(std::string_view name,
                               std::chrono::duration<Rep, Period> delay,
                               Fn&& fn,
                               detached_exception_handler handler =
                                   detail::default_spawn_exception_handler) noexcept
    -> bool {
  return spawn_logged_after(name,
                            delay,
                            detail::make_spawn_task(std::forward<Fn>(fn)),
                            std::move(handler));
}

template <typename Clock, typename Duration>
inline auto spawn_logged_at(uv_runtime& runtime,
                            std::string_view name,
                            std::chrono::time_point<Clock, Duration> deadline,
                            coro::task<void> task,
                            detached_exception_handler handler =
                                detail::default_spawn_exception_handler) noexcept
    -> bool {
  return spawn_logged(runtime,
                      name,
                      detail::make_spawn_at_task(deadline, std::move(task)),
                      std::move(handler));
}

template <typename Clock, typename Duration>
inline auto spawn_logged_at(std::string_view name,
                            std::chrono::time_point<Clock, Duration> deadline,
                            coro::task<void> task,
                            detached_exception_handler handler =
                                detail::default_spawn_exception_handler) noexcept
    -> bool {
  return spawn_logged(name,
                      detail::make_spawn_at_task(deadline, std::move(task)),
                      std::move(handler));
}

template <typename Clock, typename Duration, typename Fn>
requires std::same_as<std::invoke_result_t<std::decay_t<Fn>&>, coro::task<void>>
inline auto spawn_logged_at(uv_runtime& runtime,
                            std::string_view name,
                            std::chrono::time_point<Clock, Duration> deadline,
                            Fn&& fn,
                            detached_exception_handler handler =
                                detail::default_spawn_exception_handler) noexcept
    -> bool {
  return spawn_logged_at(runtime,
                         name,
                         deadline,
                         detail::make_spawn_task(std::forward<Fn>(fn)),
                         std::move(handler));
}

template <typename Clock, typename Duration, typename Fn>
requires std::same_as<std::invoke_result_t<std::decay_t<Fn>&>, coro::task<void>>
inline auto spawn_logged_at(std::string_view name,
                            std::chrono::time_point<Clock, Duration> deadline,
                            Fn&& fn,
                            detached_exception_handler handler =
                                detail::default_spawn_exception_handler) noexcept
    -> bool {
  return spawn_logged_at(name,
                         deadline,
                         detail::make_spawn_task(std::forward<Fn>(fn)),
                         std::move(handler));
}

template <typename Rep, typename Period, typename Fn>
requires detail::periodic_task_factory<Fn>
inline auto spawn_every(uv_runtime& runtime,
                        std::chrono::duration<Rep, Period> interval,
                        Fn&& fn) -> periodic_task {
  const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(interval);
  if (delay.count() <= 0) {
    throw std::invalid_argument("luvcoro::spawn_every requires a positive interval");
  }

  auto stop_source = std::make_shared<std::stop_source>();
  if (!spawn(runtime,
             detail::make_spawn_every_task(
                 delay, stop_source, std::forward<Fn>(fn)))) {
    return {};
  }

  return periodic_task(std::move(stop_source));
}

template <typename Rep, typename Period, typename Fn>
requires detail::periodic_task_factory<Fn>
inline auto spawn_every(std::chrono::duration<Rep, Period> interval,
                        Fn&& fn) -> periodic_task {
  auto* runtime = try_current_runtime();
  if (runtime == nullptr) {
    return {};
  }

  return spawn_every(*runtime, interval, std::forward<Fn>(fn));
}

template <typename Rep, typename Period, typename Fn>
requires detail::periodic_task_factory<Fn>
inline auto spawn_every_immediate(uv_runtime& runtime,
                                  std::chrono::duration<Rep, Period> interval,
                                  Fn&& fn) -> periodic_task {
  const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(interval);
  if (delay.count() <= 0) {
    throw std::invalid_argument(
        "luvcoro::spawn_every_immediate requires a positive interval");
  }

  auto stop_source = std::make_shared<std::stop_source>();
  if (!spawn(runtime,
             detail::make_spawn_every_immediate_task(
                 delay, stop_source, std::forward<Fn>(fn)))) {
    return {};
  }

  return periodic_task(std::move(stop_source));
}

template <typename Rep, typename Period, typename Fn>
requires detail::periodic_task_factory<Fn>
inline auto spawn_every_immediate(std::chrono::duration<Rep, Period> interval,
                                  Fn&& fn) -> periodic_task {
  auto* runtime = try_current_runtime();
  if (runtime == nullptr) {
    return {};
  }

  return spawn_every_immediate(*runtime, interval, std::forward<Fn>(fn));
}

template <typename Rep, typename Period, typename Fn>
requires detail::periodic_task_factory<Fn>
inline auto spawn_every_fixed_rate(uv_runtime& runtime,
                                   std::chrono::duration<Rep, Period> interval,
                                   Fn&& fn) -> periodic_task {
  const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(interval);
  if (delay.count() <= 0) {
    throw std::invalid_argument(
        "luvcoro::spawn_every_fixed_rate requires a positive interval");
  }

  auto stop_source = std::make_shared<std::stop_source>();
  if (!spawn(runtime,
             detail::make_spawn_every_fixed_rate_task(
                 delay, stop_source, std::forward<Fn>(fn)))) {
    return {};
  }

  return periodic_task(std::move(stop_source));
}

template <typename Rep, typename Period, typename Fn>
requires detail::periodic_task_factory<Fn>
inline auto spawn_every_fixed_rate(std::chrono::duration<Rep, Period> interval,
                                   Fn&& fn) -> periodic_task {
  auto* runtime = try_current_runtime();
  if (runtime == nullptr) {
    return {};
  }

  return spawn_every_fixed_rate(*runtime, interval, std::forward<Fn>(fn));
}

template <typename Rep, typename Period, typename Fn>
requires detail::periodic_task_factory<Fn>
inline auto spawn_logged_every(uv_runtime& runtime,
                               std::string_view name,
                               std::chrono::duration<Rep, Period> interval,
                               Fn&& fn,
                               detached_exception_handler handler =
                                   detail::default_spawn_exception_handler) -> periodic_task {
  const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(interval);
  if (delay.count() <= 0) {
    throw std::invalid_argument(
        "luvcoro::spawn_logged_every requires a positive interval");
  }

  auto stop_source = std::make_shared<std::stop_source>();
  if (!spawn_logged(runtime,
                    name,
                    detail::make_spawn_every_task(
                        delay, stop_source, std::forward<Fn>(fn)),
                    std::move(handler))) {
    return {};
  }

  return periodic_task(std::move(stop_source));
}

template <typename Rep, typename Period, typename Fn>
requires detail::periodic_task_factory<Fn>
inline auto spawn_logged_every(std::string_view name,
                               std::chrono::duration<Rep, Period> interval,
                               Fn&& fn,
                               detached_exception_handler handler =
                                   detail::default_spawn_exception_handler) -> periodic_task {
  auto* runtime = try_current_runtime();
  if (runtime == nullptr) {
    return {};
  }

  return spawn_logged_every(
      *runtime, name, interval, std::forward<Fn>(fn), std::move(handler));
}

template <typename Rep, typename Period, typename Fn>
requires detail::periodic_task_factory<Fn>
inline auto spawn_logged_every_immediate(
    uv_runtime& runtime,
    std::string_view name,
    std::chrono::duration<Rep, Period> interval,
    Fn&& fn,
    detached_exception_handler handler = detail::default_spawn_exception_handler)
    -> periodic_task {
  const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(interval);
  if (delay.count() <= 0) {
    throw std::invalid_argument(
        "luvcoro::spawn_logged_every_immediate requires a positive interval");
  }

  auto stop_source = std::make_shared<std::stop_source>();
  if (!spawn_logged(runtime,
                    name,
                    detail::make_spawn_every_immediate_task(
                        delay, stop_source, std::forward<Fn>(fn)),
                    std::move(handler))) {
    return {};
  }

  return periodic_task(std::move(stop_source));
}

template <typename Rep, typename Period, typename Fn>
requires detail::periodic_task_factory<Fn>
inline auto spawn_logged_every_immediate(
    std::string_view name,
    std::chrono::duration<Rep, Period> interval,
    Fn&& fn,
    detached_exception_handler handler = detail::default_spawn_exception_handler)
    -> periodic_task {
  auto* runtime = try_current_runtime();
  if (runtime == nullptr) {
    return {};
  }

  return spawn_logged_every_immediate(
      *runtime, name, interval, std::forward<Fn>(fn), std::move(handler));
}

template <typename Rep, typename Period, typename Fn>
requires detail::periodic_task_factory<Fn>
inline auto spawn_logged_every_fixed_rate(
    uv_runtime& runtime,
    std::string_view name,
    std::chrono::duration<Rep, Period> interval,
    Fn&& fn,
    detached_exception_handler handler = detail::default_spawn_exception_handler)
    -> periodic_task {
  const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(interval);
  if (delay.count() <= 0) {
    throw std::invalid_argument(
        "luvcoro::spawn_logged_every_fixed_rate requires a positive interval");
  }

  auto stop_source = std::make_shared<std::stop_source>();
  if (!spawn_logged(runtime,
                    name,
                    detail::make_spawn_every_fixed_rate_task(
                        delay, stop_source, std::forward<Fn>(fn)),
                    std::move(handler))) {
    return {};
  }

  return periodic_task(std::move(stop_source));
}

template <typename Rep, typename Period, typename Fn>
requires detail::periodic_task_factory<Fn>
inline auto spawn_logged_every_fixed_rate(
    std::string_view name,
    std::chrono::duration<Rep, Period> interval,
    Fn&& fn,
    detached_exception_handler handler = detail::default_spawn_exception_handler)
    -> periodic_task {
  auto* runtime = try_current_runtime();
  if (runtime == nullptr) {
    return {};
  }

  return spawn_logged_every_fixed_rate(
      *runtime, name, interval, std::forward<Fn>(fn), std::move(handler));
}

inline coro::task<void> wait_for_stop(std::stop_token stop_token) {
  co_await wait_for_stop(current_runtime(), stop_token);
}

inline coro::task<stop_wait_result> wait_for_stop(std::stop_token stop_token,
                                                  std::stop_token cancel_token) {
  co_return co_await wait_for_stop(current_runtime(), stop_token, cancel_token);
}

inline coro::task<void> sleep_for(std::chrono::milliseconds delay) {
  co_await sleep_for(current_runtime(), delay);
}
inline coro::task<void> sleep_for(std::chrono::milliseconds delay,
                                  std::stop_token stop_token) {
  co_await sleep_for(current_runtime(), delay, stop_token);
}

template <typename Rep, typename Period>
inline coro::task<void> sleep_for(uv_runtime& runtime,
                                  std::chrono::duration<Rep, Period> delay) {
  co_await sleep_for(runtime,
                     std::chrono::duration_cast<std::chrono::milliseconds>(delay));
}

template <typename Rep, typename Period>
inline coro::task<void> sleep_for(std::chrono::duration<Rep, Period> delay) {
  co_await sleep_for(current_runtime(),
                     std::chrono::duration_cast<std::chrono::milliseconds>(delay));
}

template <typename Rep, typename Period>
inline coro::task<void> sleep_for(uv_runtime& runtime,
                                  std::chrono::duration<Rep, Period> delay,
                                  std::stop_token stop_token) {
  co_await sleep_for(runtime,
                     std::chrono::duration_cast<std::chrono::milliseconds>(delay),
                     stop_token);
}

template <typename Rep, typename Period>
inline coro::task<void> sleep_for(std::chrono::duration<Rep, Period> delay,
                                  std::stop_token stop_token) {
  co_await sleep_for(current_runtime(),
                     std::chrono::duration_cast<std::chrono::milliseconds>(delay),
                     stop_token);
}

inline coro::task<void> sleep_until(std::chrono::steady_clock::time_point deadline) {
  co_await sleep_until(current_runtime(), deadline);
}
inline coro::task<void> sleep_until(std::chrono::steady_clock::time_point deadline,
                                    std::stop_token stop_token) {
  co_await sleep_until(current_runtime(), deadline, stop_token);
}

template <typename Clock, typename Duration>
inline coro::task<void> sleep_until(uv_runtime& runtime,
                                    std::chrono::time_point<Clock, Duration> deadline) {
  const auto now = Clock::now();
  if (deadline <= now) {
    co_return;
  }

  const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  co_await sleep_for(runtime, delay);
}

template <typename Clock, typename Duration>
inline coro::task<void> sleep_until(std::chrono::time_point<Clock, Duration> deadline) {
  co_await sleep_until(current_runtime(), deadline);
}

template <typename Clock, typename Duration>
inline coro::task<void> sleep_until(uv_runtime& runtime,
                                    std::chrono::time_point<Clock, Duration> deadline,
                                    std::stop_token stop_token) {
  const auto now = Clock::now();
  if (deadline <= now) {
    if (stop_token.stop_requested()) {
      throw operation_cancelled();
    }
    co_return;
  }

  const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  co_await sleep_for(runtime, delay, stop_token);
}

template <typename Clock, typename Duration>
inline coro::task<void> sleep_until(std::chrono::time_point<Clock, Duration> deadline,
                                    std::stop_token stop_token) {
  co_await sleep_until(current_runtime(), deadline, stop_token);
}

class steady_timer {
 public:
  steady_timer();
  explicit steady_timer(uv_runtime& runtime);

  template <typename Rep, typename Period>
  void expires_after(std::chrono::duration<Rep, Period> delay) {
    expires_at(std::chrono::steady_clock::now() +
               std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay));
  }

  template <typename Duration>
  void expires_at(std::chrono::time_point<std::chrono::steady_clock, Duration> deadline) {
    expiry_ = std::chrono::time_point_cast<std::chrono::steady_clock::duration>(deadline);
    cancel_source_ = std::stop_source{};
  }

  auto expiry() const noexcept -> std::chrono::steady_clock::time_point { return expiry_; }

  auto wait() -> coro::task<void>;
  void cancel() noexcept { cancel_source_.request_stop(); }

 private:
  uv_runtime* runtime_ = nullptr;
  std::chrono::steady_clock::time_point expiry_{std::chrono::steady_clock::now()};
  std::stop_source cancel_source_{};
};

}  // namespace luvcoro
