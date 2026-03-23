#pragma once

#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <stop_token>
#include <type_traits>
#include <utility>

#include <coro/coro.hpp>
#include <coro/detail/task_self_deleting.hpp>

#include "luvcoro/error.hpp"
#include "luvcoro/message.hpp"
#include "luvcoro/runtime.hpp"

namespace luvcoro {

class task_scope {
 public:
  task_scope();
  explicit task_scope(uv_runtime& runtime);

  auto token() const noexcept -> std::stop_token { return state_->stop_source.get_token(); }
  void request_stop() noexcept { state_->stop_source.request_stop(); }

  auto join(std::stop_token stop_token = {}) -> coro::task<void>;

  auto cancel_and_join() -> coro::task<void> {
    request_stop();
    co_await join();
  }

  void spawn(coro::task<void> task) { spawn_impl(std::move(task)); }

  template <typename Factory>
  requires requires(std::decay_t<Factory>& factory, std::stop_token stop_token) {
    { factory(stop_token) } -> std::same_as<coro::task<void>>;
  }
  void spawn(Factory&& factory) {
    spawn_impl(std::forward<Factory>(factory)(token()));
  }

 private:
  struct state {
    explicit state(uv_runtime& runtime) : runtime(&runtime), completions(runtime) {}

    uv_runtime* runtime = nullptr;
    std::mutex mutex{};
    std::size_t active = 0;
    std::exception_ptr first_exception{};
    std::stop_source stop_source{};
    message_channel<std::monostate> completions;
  };

  std::shared_ptr<state> state_;

  void spawn_impl(coro::task<void> task);
  static auto run_child(std::shared_ptr<state> state,
                        coro::task<void> task) -> coro::task<void>;
};

inline task_scope::task_scope() : task_scope(current_runtime()) {}

inline task_scope::task_scope(uv_runtime& runtime) : state_(std::make_shared<state>(runtime)) {}

inline auto task_scope::join(std::stop_token stop_token) -> coro::task<void> {
  bool cancelled = false;
  std::optional<std::stop_callback<std::function<void()>>> stop_callback{};

  if (stop_token.stop_possible()) {
    auto state = state_;
    stop_callback.emplace(stop_token, std::function<void()>([state]() noexcept {
                            state->stop_source.request_stop();
                            try {
                              state->completions.send(std::monostate{});
                            } catch (...) {
                            }
                          }));
  }

  while (true) {
    if (stop_token.stop_requested()) {
      cancelled = true;
      request_stop();
    }

    std::exception_ptr exception;
    bool done = false;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      if (state_->active == 0) {
        exception = state_->first_exception;
        done = true;
      }
    }

    if (done) {
      if (exception != nullptr) {
        std::rethrow_exception(exception);
      }
      if (cancelled) {
        throw operation_cancelled();
      }
      co_return;
    }

    try {
      (void)co_await state_->completions.recv();
    } catch (...) {
      std::lock_guard<std::mutex> lock(state_->mutex);
      if (state_->active == 0) {
        if (state_->first_exception != nullptr) {
          std::rethrow_exception(state_->first_exception);
        }
        if (cancelled) {
          throw operation_cancelled();
        }
        co_return;
      }
      throw;
    }
  }
}

inline void task_scope::spawn_impl(coro::task<void> task) {
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    ++state_->active;
  }

  auto detached = coro::detail::make_task_self_deleting(
      run_child(state_, std::move(task)));
  auto handle = detached.handle();
  if (!handle.done()) {
    handle.resume();
  }
}

inline auto task_scope::run_child(std::shared_ptr<state> state,
                                  coro::task<void> task) -> coro::task<void> {
  try {
    co_await task;
  } catch (...) {
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->first_exception == nullptr) {
        state->first_exception = std::current_exception();
      }
    }
    state->stop_source.request_stop();
  }

  bool became_empty = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    --state->active;
    became_empty = state->active == 0;
  }

  if (became_empty) {
    state->completions.close();
    co_return;
  }

  co_await state->completions.async_send(std::monostate{});
}

template <typename... Tasks>
auto run_scoped(uv_runtime& runtime, Tasks&&... tasks) -> coro::task<void> {
  task_scope scope(runtime);
  (scope.spawn(std::forward<Tasks>(tasks)), ...);
  co_await scope.join();
}

template <typename... Tasks>
auto run_scoped(uv_runtime& runtime,
                std::stop_token stop_token,
                Tasks&&... tasks) -> coro::task<void> {
  task_scope scope(runtime);
  (scope.spawn(std::forward<Tasks>(tasks)), ...);
  co_await scope.join(stop_token);
}

template <typename... Tasks>
auto run_scoped(Tasks&&... tasks) -> coro::task<void> {
  co_await run_scoped(current_runtime(), std::forward<Tasks>(tasks)...);
}

template <typename... Tasks>
auto run_scoped(std::stop_token stop_token, Tasks&&... tasks) -> coro::task<void> {
  co_await run_scoped(current_runtime(), stop_token, std::forward<Tasks>(tasks)...);
}

}  // namespace luvcoro
