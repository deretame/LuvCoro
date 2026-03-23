#pragma once

#include <coroutine>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>

#include <coro/coro.hpp>

#include "luvcoro/error.hpp"
#include "luvcoro/runtime.hpp"

namespace luvcoro {

namespace detail {

inline void require_runtime_thread(uv_runtime& runtime, const char* op_name) {
  if (!runtime.is_loop_thread()) {
    throw std::runtime_error(std::string(op_name) +
                             " must be called on the bound runtime thread");
  }
}

template <typename Waiter>
void resume_waiter(uv_runtime& runtime, const std::shared_ptr<Waiter>& waiter) {
  if (waiter == nullptr || waiter->continuation == nullptr) {
    return;
  }

  if (runtime.is_loop_thread()) {
    waiter->continuation.resume();
    return;
  }

  runtime.post([waiter]() {
    if (waiter->continuation != nullptr) {
      waiter->continuation.resume();
    }
  });
}

template <typename Waiter>
void resume_waiter_noexcept(uv_runtime& runtime, const std::shared_ptr<Waiter>& waiter) noexcept {
  try {
    resume_waiter(runtime, waiter);
  } catch (...) {
  }
}

template <typename Waiter>
void prune_inactive(std::deque<std::shared_ptr<Waiter>>& waiters) {
  while (!waiters.empty() && !waiters.front()->active) {
    waiters.pop_front();
  }
}

}  // namespace detail

class async_mutex;
class local_async_mutex;

class async_mutex_guard {
 public:
  async_mutex_guard() = default;
  explicit async_mutex_guard(async_mutex& mutex) noexcept;
  ~async_mutex_guard();

  async_mutex_guard(const async_mutex_guard&) = delete;
  auto operator=(const async_mutex_guard&) -> async_mutex_guard& = delete;

  async_mutex_guard(async_mutex_guard&& other) noexcept;
  auto operator=(async_mutex_guard&& other) noexcept -> async_mutex_guard&;

  auto owns_lock() const noexcept -> bool { return mutex_ != nullptr; }
  void unlock();

 private:
  async_mutex* mutex_ = nullptr;
};

class local_async_mutex_guard {
 public:
  local_async_mutex_guard() = default;
  explicit local_async_mutex_guard(local_async_mutex& mutex) noexcept;
  ~local_async_mutex_guard();

  local_async_mutex_guard(const local_async_mutex_guard&) = delete;
  auto operator=(const local_async_mutex_guard&) -> local_async_mutex_guard& = delete;

  local_async_mutex_guard(local_async_mutex_guard&& other) noexcept;
  auto operator=(local_async_mutex_guard&& other) noexcept -> local_async_mutex_guard&;

  auto owns_lock() const noexcept -> bool { return mutex_ != nullptr; }
  void unlock();

 private:
  local_async_mutex* mutex_ = nullptr;
};

class async_mutex {
 public:
  async_mutex();
  explicit async_mutex(uv_runtime& runtime);

  auto lock(std::stop_token stop_token = {}) -> coro::task<void>;
  auto scoped_lock(std::stop_token stop_token = {}) -> coro::task<async_mutex_guard>;
  auto try_lock() -> bool;
  void unlock();

 private:
  struct waiter {
    std::coroutine_handle<> continuation{};
    std::optional<std::stop_callback<std::function<void()>>> stop_callback{};
    bool active = true;
    bool completed = false;
    bool cancelled = false;
  };

  void cancel_waiter(const std::shared_ptr<waiter>& waiter) noexcept;

  uv_runtime* runtime_ = nullptr;
  std::mutex mutex_{};
  bool locked_ = false;
  std::deque<std::shared_ptr<waiter>> waiters_{};
};

class local_async_mutex {
 public:
  local_async_mutex();
  explicit local_async_mutex(uv_runtime& runtime);

  auto lock(std::stop_token stop_token = {}) -> coro::task<void>;
  auto scoped_lock(std::stop_token stop_token = {}) -> coro::task<local_async_mutex_guard>;
  auto try_lock() -> bool;
  void unlock();

 private:
  struct waiter {
    std::coroutine_handle<> continuation{};
    std::optional<std::stop_callback<std::function<void()>>> stop_callback{};
    bool active = true;
    bool completed = false;
    bool cancelled = false;
  };

  void cancel_waiter(const std::shared_ptr<waiter>& waiter) noexcept;

  uv_runtime* runtime_ = nullptr;
  bool locked_ = false;
  std::deque<std::shared_ptr<waiter>> waiters_{};
};

class async_semaphore {
 public:
  explicit async_semaphore(std::ptrdiff_t initial = 0);
  async_semaphore(uv_runtime& runtime, std::ptrdiff_t initial = 0);

  auto acquire(std::stop_token stop_token = {}) -> coro::task<void>;
  auto try_acquire() -> bool;
  void release(std::ptrdiff_t count = 1);
  auto available() const -> std::ptrdiff_t;

 private:
  struct waiter {
    std::coroutine_handle<> continuation{};
    std::optional<std::stop_callback<std::function<void()>>> stop_callback{};
    bool active = true;
    bool completed = false;
    bool cancelled = false;
  };

  void cancel_waiter(const std::shared_ptr<waiter>& waiter) noexcept;

  uv_runtime* runtime_ = nullptr;
  mutable std::mutex mutex_{};
  std::ptrdiff_t permits_ = 0;
  std::deque<std::shared_ptr<waiter>> waiters_{};
};

class local_async_semaphore {
 public:
  explicit local_async_semaphore(std::ptrdiff_t initial = 0);
  local_async_semaphore(uv_runtime& runtime, std::ptrdiff_t initial = 0);

  auto acquire(std::stop_token stop_token = {}) -> coro::task<void>;
  auto try_acquire() -> bool;
  void release(std::ptrdiff_t count = 1);
  auto available() const -> std::ptrdiff_t;

 private:
  struct waiter {
    std::coroutine_handle<> continuation{};
    std::optional<std::stop_callback<std::function<void()>>> stop_callback{};
    bool active = true;
    bool completed = false;
    bool cancelled = false;
  };

  void cancel_waiter(const std::shared_ptr<waiter>& waiter) noexcept;

  uv_runtime* runtime_ = nullptr;
  std::ptrdiff_t permits_ = 0;
  std::deque<std::shared_ptr<waiter>> waiters_{};
};

class local_async_condition_variable {
 public:
  local_async_condition_variable();
  explicit local_async_condition_variable(uv_runtime& runtime);

  auto wait(local_async_mutex& mutex, std::stop_token stop_token = {}) -> coro::task<void>;

  template <typename Predicate>
  auto wait(local_async_mutex& mutex,
            Predicate&& predicate,
            std::stop_token stop_token = {}) -> coro::task<void> {
    while (!std::invoke(predicate)) {
      co_await wait(mutex, stop_token);
    }
  }

  void notify_one();
  void notify_all();

 private:
  struct waiter {
    std::coroutine_handle<> continuation{};
    std::optional<std::stop_callback<std::function<void()>>> stop_callback{};
    bool active = true;
    bool completed = false;
    bool cancelled = false;
  };

  void cancel_waiter(const std::shared_ptr<waiter>& waiter) noexcept;

  uv_runtime* runtime_ = nullptr;
  std::deque<std::shared_ptr<waiter>> waiters_{};
};

class async_condition_variable {
 public:
  async_condition_variable();
  explicit async_condition_variable(uv_runtime& runtime);

  auto wait(async_mutex& mutex, std::stop_token stop_token = {}) -> coro::task<void>;

  template <typename Predicate>
  auto wait(async_mutex& mutex, Predicate&& predicate, std::stop_token stop_token = {})
      -> coro::task<void> {
    while (!std::invoke(predicate)) {
      co_await wait(mutex, stop_token);
    }
  }

  void notify_one();
  void notify_all();

 private:
  struct waiter {
    std::coroutine_handle<> continuation{};
    std::optional<std::stop_callback<std::function<void()>>> stop_callback{};
    bool active = true;
    bool completed = false;
    bool cancelled = false;
  };

  void cancel_waiter(const std::shared_ptr<waiter>& waiter) noexcept;

  uv_runtime* runtime_ = nullptr;
  std::mutex mutex_{};
  std::deque<std::shared_ptr<waiter>> waiters_{};
};

class local_async_event {
 public:
  local_async_event();
  explicit local_async_event(uv_runtime& runtime, bool signaled = false);

  auto wait(std::stop_token stop_token = {}) -> coro::task<void>;
  void set();
  void reset();
  auto is_set() const -> bool;

 private:
  struct waiter {
    std::coroutine_handle<> continuation{};
    std::optional<std::stop_callback<std::function<void()>>> stop_callback{};
    bool active = true;
    bool completed = false;
    bool cancelled = false;
  };

  void cancel_waiter(const std::shared_ptr<waiter>& waiter) noexcept;

  uv_runtime* runtime_ = nullptr;
  bool signaled_ = false;
  std::deque<std::shared_ptr<waiter>> waiters_{};
};

inline async_mutex_guard::async_mutex_guard(async_mutex& mutex) noexcept : mutex_(&mutex) {}

inline async_mutex_guard::~async_mutex_guard() {
  if (mutex_ != nullptr) {
    try {
      mutex_->unlock();
    } catch (...) {
    }
  }
}

inline async_mutex_guard::async_mutex_guard(async_mutex_guard&& other) noexcept
    : mutex_(other.mutex_) {
  other.mutex_ = nullptr;
}

inline auto async_mutex_guard::operator=(async_mutex_guard&& other) noexcept
    -> async_mutex_guard& {
  if (this == &other) {
    return *this;
  }

  if (mutex_ != nullptr) {
    try {
      mutex_->unlock();
    } catch (...) {
    }
  }

  mutex_ = other.mutex_;
  other.mutex_ = nullptr;
  return *this;
}

inline void async_mutex_guard::unlock() {
  if (mutex_ == nullptr) {
    return;
  }

  auto* mutex = mutex_;
  mutex_ = nullptr;
  mutex->unlock();
}

inline local_async_mutex_guard::local_async_mutex_guard(local_async_mutex& mutex) noexcept
    : mutex_(&mutex) {}

inline local_async_mutex_guard::~local_async_mutex_guard() {
  if (mutex_ != nullptr) {
    try {
      mutex_->unlock();
    } catch (...) {
    }
  }
}

inline local_async_mutex_guard::local_async_mutex_guard(local_async_mutex_guard&& other) noexcept
    : mutex_(other.mutex_) {
  other.mutex_ = nullptr;
}

inline auto local_async_mutex_guard::operator=(local_async_mutex_guard&& other) noexcept
    -> local_async_mutex_guard& {
  if (this == &other) {
    return *this;
  }

  if (mutex_ != nullptr) {
    try {
      mutex_->unlock();
    } catch (...) {
    }
  }

  mutex_ = other.mutex_;
  other.mutex_ = nullptr;
  return *this;
}

inline void local_async_mutex_guard::unlock() {
  if (mutex_ == nullptr) {
    return;
  }

  auto* mutex = mutex_;
  mutex_ = nullptr;
  mutex->unlock();
}

inline async_mutex::async_mutex() : async_mutex(current_runtime()) {}

inline async_mutex::async_mutex(uv_runtime& runtime) : runtime_(&runtime) {}

inline void async_mutex::cancel_waiter(const std::shared_ptr<waiter>& waiter) noexcept {
  if (waiter == nullptr) {
    return;
  }

  bool should_resume = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!waiter->active || waiter->completed) {
      return;
    }
    waiter->active = false;
    waiter->completed = true;
    waiter->cancelled = true;
    should_resume = waiter->continuation != nullptr;
  }

  if (should_resume) {
    detail::resume_waiter_noexcept(*runtime_, waiter);
  }
}

inline auto async_mutex::lock(std::stop_token stop_token) -> coro::task<void> {
  struct awaiter {
    async_mutex* owner = nullptr;
    std::stop_token stop_token{};
    std::shared_ptr<waiter> waiter_state = std::make_shared<waiter>();
    bool acquired = false;

    bool await_ready() {
      std::lock_guard<std::mutex> lock(owner->mutex_);
      detail::prune_inactive(owner->waiters_);
      if (!owner->locked_ && owner->waiters_.empty()) {
        owner->locked_ = true;
        acquired = true;
        waiter_state->completed = true;
        return true;
      }
      return false;
    }

    bool await_suspend(std::coroutine_handle<> h) {
      if (stop_token.stop_requested()) {
        waiter_state->completed = true;
        waiter_state->cancelled = true;
        return false;
      }

      waiter_state->continuation = h;
      {
        std::lock_guard<std::mutex> lock(owner->mutex_);
        detail::prune_inactive(owner->waiters_);
        if (!owner->locked_ && owner->waiters_.empty()) {
          owner->locked_ = true;
          acquired = true;
          waiter_state->completed = true;
          return false;
        }

        owner->waiters_.push_back(waiter_state);
      }

      if (stop_token.stop_possible()) {
        waiter_state->stop_callback.emplace(
            stop_token,
            std::function<void()>([owner = owner, waiter_state = waiter_state]() noexcept {
              owner->cancel_waiter(waiter_state);
            }));
      }

      {
        std::lock_guard<std::mutex> lock(owner->mutex_);
        return !waiter_state->completed;
      }
    }

    void await_resume() {
      waiter_state->stop_callback.reset();
      if (waiter_state->cancelled) {
        throw operation_cancelled();
      }
    }
  };

  co_await awaiter{this, stop_token, std::make_shared<waiter>(), false};
}

inline auto async_mutex::scoped_lock(std::stop_token stop_token)
    -> coro::task<async_mutex_guard> {
  co_await lock(stop_token);
  co_return async_mutex_guard(*this);
}

inline auto async_mutex::try_lock() -> bool {
  std::lock_guard<std::mutex> lock(mutex_);
  detail::prune_inactive(waiters_);
  if (locked_ || !waiters_.empty()) {
    return false;
  }

  locked_ = true;
  return true;
}

inline void async_mutex::unlock() {
  std::shared_ptr<waiter> next_waiter;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!locked_) {
      throw std::runtime_error("async_mutex is not locked");
    }

    detail::prune_inactive(waiters_);
    if (waiters_.empty()) {
      locked_ = false;
      return;
    }

    next_waiter = std::move(waiters_.front());
    waiters_.pop_front();
    next_waiter->active = false;
    next_waiter->completed = true;
  }

  detail::resume_waiter(*runtime_, next_waiter);
}

inline local_async_mutex::local_async_mutex() : local_async_mutex(current_runtime()) {}

inline local_async_mutex::local_async_mutex(uv_runtime& runtime) : runtime_(&runtime) {}

inline void local_async_mutex::cancel_waiter(const std::shared_ptr<waiter>& waiter) noexcept {
  if (waiter == nullptr || runtime_ == nullptr) {
    return;
  }

  try {
    runtime_->post([waiter]() {
      if (!waiter->active || waiter->completed) {
        return;
      }
      waiter->active = false;
      waiter->completed = true;
      waiter->cancelled = true;
      if (waiter->continuation != nullptr) {
        waiter->continuation.resume();
      }
    });
  } catch (...) {
  }
}

inline auto local_async_mutex::lock(std::stop_token stop_token) -> coro::task<void> {
  struct awaiter {
    local_async_mutex* owner = nullptr;
    std::stop_token stop_token{};
    std::shared_ptr<waiter> waiter_state = std::make_shared<waiter>();

    bool await_ready() {
      detail::require_runtime_thread(*owner->runtime_, "local_async_mutex::lock");
      detail::prune_inactive(owner->waiters_);
      if (!owner->locked_ && owner->waiters_.empty()) {
        owner->locked_ = true;
        waiter_state->completed = true;
        return true;
      }
      return false;
    }

    bool await_suspend(std::coroutine_handle<> h) {
      detail::require_runtime_thread(*owner->runtime_, "local_async_mutex::lock");
      if (stop_token.stop_requested()) {
        waiter_state->completed = true;
        waiter_state->cancelled = true;
        return false;
      }

      waiter_state->continuation = h;
      detail::prune_inactive(owner->waiters_);
      if (!owner->locked_ && owner->waiters_.empty()) {
        owner->locked_ = true;
        waiter_state->completed = true;
        return false;
      }

      owner->waiters_.push_back(waiter_state);
      if (stop_token.stop_possible()) {
        waiter_state->stop_callback.emplace(
            stop_token,
            std::function<void()>([owner = owner, waiter_state = waiter_state]() noexcept {
              owner->cancel_waiter(waiter_state);
            }));
      }

      return !waiter_state->completed;
    }

    void await_resume() {
      waiter_state->stop_callback.reset();
      if (waiter_state->cancelled) {
        throw operation_cancelled();
      }
    }
  };

  co_await awaiter{this, stop_token, std::make_shared<waiter>()};
}

inline auto local_async_mutex::scoped_lock(std::stop_token stop_token)
    -> coro::task<local_async_mutex_guard> {
  co_await lock(stop_token);
  co_return local_async_mutex_guard(*this);
}

inline auto local_async_mutex::try_lock() -> bool {
  detail::require_runtime_thread(*runtime_, "local_async_mutex::try_lock");
  detail::prune_inactive(waiters_);
  if (locked_ || !waiters_.empty()) {
    return false;
  }

  locked_ = true;
  return true;
}

inline void local_async_mutex::unlock() {
  detail::require_runtime_thread(*runtime_, "local_async_mutex::unlock");
  if (!locked_) {
    throw std::runtime_error("local_async_mutex is not locked");
  }

  detail::prune_inactive(waiters_);
  if (waiters_.empty()) {
    locked_ = false;
    return;
  }

  auto next_waiter = std::move(waiters_.front());
  waiters_.pop_front();
  next_waiter->active = false;
  next_waiter->completed = true;
  if (next_waiter->continuation != nullptr) {
    next_waiter->continuation.resume();
  }
}

inline async_semaphore::async_semaphore(std::ptrdiff_t initial)
    : async_semaphore(current_runtime(), initial) {}

inline async_semaphore::async_semaphore(uv_runtime& runtime, std::ptrdiff_t initial)
    : runtime_(&runtime), permits_(initial) {
  if (initial < 0) {
    throw std::invalid_argument("async_semaphore initial count must be >= 0");
  }
}

inline void async_semaphore::cancel_waiter(const std::shared_ptr<waiter>& waiter) noexcept {
  if (waiter == nullptr) {
    return;
  }

  bool should_resume = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!waiter->active || waiter->completed) {
      return;
    }
    waiter->active = false;
    waiter->completed = true;
    waiter->cancelled = true;
    should_resume = waiter->continuation != nullptr;
  }

  if (should_resume) {
    detail::resume_waiter_noexcept(*runtime_, waiter);
  }
}

inline auto async_semaphore::acquire(std::stop_token stop_token) -> coro::task<void> {
  struct awaiter {
    async_semaphore* owner = nullptr;
    std::stop_token stop_token{};
    std::shared_ptr<waiter> waiter_state = std::make_shared<waiter>();

    bool await_ready() {
      std::lock_guard<std::mutex> lock(owner->mutex_);
      detail::prune_inactive(owner->waiters_);
      if (owner->permits_ > 0) {
        --owner->permits_;
        waiter_state->completed = true;
        return true;
      }
      return false;
    }

    bool await_suspend(std::coroutine_handle<> h) {
      if (stop_token.stop_requested()) {
        waiter_state->completed = true;
        waiter_state->cancelled = true;
        return false;
      }

      waiter_state->continuation = h;
      {
        std::lock_guard<std::mutex> lock(owner->mutex_);
        detail::prune_inactive(owner->waiters_);
        if (owner->permits_ > 0) {
          --owner->permits_;
          waiter_state->completed = true;
          return false;
        }

        owner->waiters_.push_back(waiter_state);
      }

      if (stop_token.stop_possible()) {
        waiter_state->stop_callback.emplace(
            stop_token,
            std::function<void()>([owner = owner, waiter_state = waiter_state]() noexcept {
              owner->cancel_waiter(waiter_state);
            }));
      }

      {
        std::lock_guard<std::mutex> lock(owner->mutex_);
        return !waiter_state->completed;
      }
    }

    void await_resume() {
      waiter_state->stop_callback.reset();
      if (waiter_state->cancelled) {
        throw operation_cancelled();
      }
    }
  };

  co_await awaiter{this, stop_token, std::make_shared<waiter>()};
}

inline auto async_semaphore::try_acquire() -> bool {
  std::lock_guard<std::mutex> lock(mutex_);
  detail::prune_inactive(waiters_);
  if (permits_ <= 0) {
    return false;
  }

  --permits_;
  return true;
}

inline void async_semaphore::release(std::ptrdiff_t count) {
  if (count <= 0) {
    throw std::invalid_argument("async_semaphore release count must be > 0");
  }

  std::vector<std::shared_ptr<waiter>> ready_waiters;
  ready_waiters.reserve(static_cast<std::size_t>(count));

  {
    std::lock_guard<std::mutex> lock(mutex_);
    detail::prune_inactive(waiters_);
    while (count > 0 && !waiters_.empty()) {
      auto waiter_state = std::move(waiters_.front());
      waiters_.pop_front();
      waiter_state->active = false;
      waiter_state->completed = true;
      ready_waiters.push_back(std::move(waiter_state));
      detail::prune_inactive(waiters_);
      --count;
    }

    permits_ += count;
  }

  for (const auto& waiter_state : ready_waiters) {
    detail::resume_waiter(*runtime_, waiter_state);
  }
}

inline auto async_semaphore::available() const -> std::ptrdiff_t {
  std::lock_guard<std::mutex> lock(mutex_);
  return permits_;
}

inline local_async_semaphore::local_async_semaphore(std::ptrdiff_t initial)
    : local_async_semaphore(current_runtime(), initial) {}

inline local_async_semaphore::local_async_semaphore(uv_runtime& runtime,
                                                    std::ptrdiff_t initial)
    : runtime_(&runtime), permits_(initial) {
  if (initial < 0) {
    throw std::invalid_argument("local_async_semaphore initial count must be >= 0");
  }
}

inline void local_async_semaphore::cancel_waiter(const std::shared_ptr<waiter>& waiter) noexcept {
  if (waiter == nullptr || runtime_ == nullptr) {
    return;
  }

  try {
    runtime_->post([waiter]() {
      if (!waiter->active || waiter->completed) {
        return;
      }
      waiter->active = false;
      waiter->completed = true;
      waiter->cancelled = true;
      if (waiter->continuation != nullptr) {
        waiter->continuation.resume();
      }
    });
  } catch (...) {
  }
}

inline auto local_async_semaphore::acquire(std::stop_token stop_token) -> coro::task<void> {
  struct awaiter {
    local_async_semaphore* owner = nullptr;
    std::stop_token stop_token{};
    std::shared_ptr<waiter> waiter_state = std::make_shared<waiter>();

    bool await_ready() {
      detail::require_runtime_thread(*owner->runtime_, "local_async_semaphore::acquire");
      detail::prune_inactive(owner->waiters_);
      if (owner->permits_ > 0) {
        --owner->permits_;
        waiter_state->completed = true;
        return true;
      }
      return false;
    }

    bool await_suspend(std::coroutine_handle<> h) {
      detail::require_runtime_thread(*owner->runtime_, "local_async_semaphore::acquire");
      if (stop_token.stop_requested()) {
        waiter_state->completed = true;
        waiter_state->cancelled = true;
        return false;
      }

      waiter_state->continuation = h;
      detail::prune_inactive(owner->waiters_);
      if (owner->permits_ > 0) {
        --owner->permits_;
        waiter_state->completed = true;
        return false;
      }

      owner->waiters_.push_back(waiter_state);
      if (stop_token.stop_possible()) {
        waiter_state->stop_callback.emplace(
            stop_token,
            std::function<void()>([owner = owner, waiter_state = waiter_state]() noexcept {
              owner->cancel_waiter(waiter_state);
            }));
      }

      return !waiter_state->completed;
    }

    void await_resume() {
      waiter_state->stop_callback.reset();
      if (waiter_state->cancelled) {
        throw operation_cancelled();
      }
    }
  };

  co_await awaiter{this, stop_token, std::make_shared<waiter>()};
}

inline auto local_async_semaphore::try_acquire() -> bool {
  detail::require_runtime_thread(*runtime_, "local_async_semaphore::try_acquire");
  detail::prune_inactive(waiters_);
  if (permits_ <= 0) {
    return false;
  }

  --permits_;
  return true;
}

inline void local_async_semaphore::release(std::ptrdiff_t count) {
  detail::require_runtime_thread(*runtime_, "local_async_semaphore::release");
  if (count <= 0) {
    throw std::invalid_argument("local_async_semaphore release count must be > 0");
  }

  detail::prune_inactive(waiters_);
  while (count > 0 && !waiters_.empty()) {
    auto waiter_state = std::move(waiters_.front());
    waiters_.pop_front();
    waiter_state->active = false;
    waiter_state->completed = true;
    if (waiter_state->continuation != nullptr) {
      waiter_state->continuation.resume();
    }
    detail::prune_inactive(waiters_);
    --count;
  }

  permits_ += count;
}

inline auto local_async_semaphore::available() const -> std::ptrdiff_t {
  detail::require_runtime_thread(*runtime_, "local_async_semaphore::available");
  return permits_;
}

inline local_async_condition_variable::local_async_condition_variable()
    : local_async_condition_variable(current_runtime()) {}

inline local_async_condition_variable::local_async_condition_variable(uv_runtime& runtime)
    : runtime_(&runtime) {}

inline void local_async_condition_variable::cancel_waiter(
    const std::shared_ptr<waiter>& waiter) noexcept {
  if (waiter == nullptr || runtime_ == nullptr) {
    return;
  }

  try {
    runtime_->post([waiter]() {
      if (!waiter->active || waiter->completed) {
        return;
      }
      waiter->active = false;
      waiter->completed = true;
      waiter->cancelled = true;
      if (waiter->continuation != nullptr) {
        waiter->continuation.resume();
      }
    });
  } catch (...) {
  }
}

inline auto local_async_condition_variable::wait(local_async_mutex& mutex,
                                                 std::stop_token stop_token)
    -> coro::task<void> {
  detail::require_runtime_thread(*runtime_, "local_async_condition_variable::wait");
  auto waiter_state = std::make_shared<waiter>();
  bool cancelled = false;

  waiters_.push_back(waiter_state);
  if (stop_token.stop_possible()) {
    waiter_state->stop_callback.emplace(
        stop_token,
        std::function<void()>([this, waiter_state]() noexcept {
          cancel_waiter(waiter_state);
        }));
  }

  mutex.unlock();

  struct awaiter {
    std::shared_ptr<waiter> waiter_state{};

    bool await_ready() const noexcept { return waiter_state->completed; }

    bool await_suspend(std::coroutine_handle<> h) {
      waiter_state->continuation = h;
      return !waiter_state->completed;
    }

    void await_resume() {
      waiter_state->stop_callback.reset();
      if (waiter_state->cancelled) {
        throw operation_cancelled();
      }
    }
  };

  try {
    co_await awaiter{waiter_state};
  } catch (const operation_cancelled&) {
    cancelled = true;
  }

  co_await mutex.lock();
  if (cancelled) {
    throw operation_cancelled();
  }
}

inline void local_async_condition_variable::notify_one() {
  detail::require_runtime_thread(*runtime_, "local_async_condition_variable::notify_one");
  detail::prune_inactive(waiters_);
  if (waiters_.empty()) {
    return;
  }

  auto waiter_state = std::move(waiters_.front());
  waiters_.pop_front();
  waiter_state->active = false;
  waiter_state->completed = true;
  if (waiter_state->continuation != nullptr) {
    waiter_state->continuation.resume();
  }
}

inline void local_async_condition_variable::notify_all() {
  detail::require_runtime_thread(*runtime_, "local_async_condition_variable::notify_all");
  detail::prune_inactive(waiters_);
  while (!waiters_.empty()) {
    auto waiter_state = std::move(waiters_.front());
    waiters_.pop_front();
    waiter_state->active = false;
    waiter_state->completed = true;
    if (waiter_state->continuation != nullptr) {
      waiter_state->continuation.resume();
    }
    detail::prune_inactive(waiters_);
  }
}

inline async_condition_variable::async_condition_variable()
    : async_condition_variable(current_runtime()) {}

inline async_condition_variable::async_condition_variable(uv_runtime& runtime)
    : runtime_(&runtime) {}

inline void async_condition_variable::cancel_waiter(
    const std::shared_ptr<waiter>& waiter_state) noexcept {
  if (waiter_state == nullptr) {
    return;
  }

  bool should_resume = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!waiter_state->active || waiter_state->completed) {
      return;
    }
    waiter_state->active = false;
    waiter_state->completed = true;
    waiter_state->cancelled = true;
    should_resume = waiter_state->continuation != nullptr;
  }

  if (should_resume) {
    detail::resume_waiter_noexcept(*runtime_, waiter_state);
  }
}

inline auto async_condition_variable::wait(async_mutex& mutex,
                                           std::stop_token stop_token) -> coro::task<void> {
  auto waiter_state = std::make_shared<waiter>();
  bool cancelled = false;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    waiters_.push_back(waiter_state);
  }

  if (stop_token.stop_possible()) {
    waiter_state->stop_callback.emplace(
        stop_token,
        std::function<void()>([this, waiter_state]() noexcept {
          cancel_waiter(waiter_state);
        }));
  }

  mutex.unlock();

  struct awaiter {
    async_condition_variable* owner = nullptr;
    std::shared_ptr<waiter> waiter_state{};

    bool await_ready() const {
      std::lock_guard<std::mutex> lock(owner->mutex_);
      return waiter_state->completed;
    }

    bool await_suspend(std::coroutine_handle<> h) {
      waiter_state->continuation = h;
      std::lock_guard<std::mutex> lock(owner->mutex_);
      return !waiter_state->completed;
    }

    void await_resume() {
      waiter_state->stop_callback.reset();
      if (waiter_state->cancelled) {
        throw operation_cancelled();
      }
    }
  };

  try {
    co_await awaiter{this, waiter_state};
  } catch (const operation_cancelled&) {
    cancelled = true;
  }

  co_await mutex.lock();
  if (cancelled) {
    throw operation_cancelled();
  }
}

inline void async_condition_variable::notify_one() {
  std::shared_ptr<waiter> ready_waiter;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    detail::prune_inactive(waiters_);
    if (waiters_.empty()) {
      return;
    }

    ready_waiter = std::move(waiters_.front());
    waiters_.pop_front();
    ready_waiter->active = false;
    ready_waiter->completed = true;
  }

  detail::resume_waiter(*runtime_, ready_waiter);
}

inline void async_condition_variable::notify_all() {
  std::vector<std::shared_ptr<waiter>> ready_waiters;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    detail::prune_inactive(waiters_);
    while (!waiters_.empty()) {
      auto waiter_state = std::move(waiters_.front());
      waiters_.pop_front();
      waiter_state->active = false;
      waiter_state->completed = true;
      ready_waiters.push_back(std::move(waiter_state));
      detail::prune_inactive(waiters_);
    }
  }

  for (const auto& waiter_state : ready_waiters) {
    detail::resume_waiter(*runtime_, waiter_state);
  }
}

inline local_async_event::local_async_event() : local_async_event(current_runtime()) {}

inline local_async_event::local_async_event(uv_runtime& runtime, bool signaled)
    : runtime_(&runtime), signaled_(signaled) {}

inline void local_async_event::cancel_waiter(const std::shared_ptr<waiter>& waiter) noexcept {
  if (waiter == nullptr || runtime_ == nullptr) {
    return;
  }

  try {
    runtime_->post([waiter]() {
      if (!waiter->active || waiter->completed) {
        return;
      }
      waiter->active = false;
      waiter->completed = true;
      waiter->cancelled = true;
      if (waiter->continuation != nullptr) {
        waiter->continuation.resume();
      }
    });
  } catch (...) {
  }
}

inline auto local_async_event::wait(std::stop_token stop_token) -> coro::task<void> {
  struct awaiter {
    local_async_event* owner = nullptr;
    std::stop_token stop_token{};
    std::shared_ptr<waiter> waiter_state = std::make_shared<waiter>();

    bool await_ready() {
      detail::require_runtime_thread(*owner->runtime_, "local_async_event::wait");
      detail::prune_inactive(owner->waiters_);
      if (owner->signaled_) {
        waiter_state->completed = true;
        return true;
      }
      return false;
    }

    bool await_suspend(std::coroutine_handle<> h) {
      detail::require_runtime_thread(*owner->runtime_, "local_async_event::wait");
      if (stop_token.stop_requested()) {
        waiter_state->completed = true;
        waiter_state->cancelled = true;
        return false;
      }

      waiter_state->continuation = h;
      detail::prune_inactive(owner->waiters_);
      if (owner->signaled_) {
        waiter_state->completed = true;
        return false;
      }

      owner->waiters_.push_back(waiter_state);
      if (stop_token.stop_possible()) {
        waiter_state->stop_callback.emplace(
            stop_token,
            std::function<void()>([owner = owner, waiter_state = waiter_state]() noexcept {
              owner->cancel_waiter(waiter_state);
            }));
      }

      return !waiter_state->completed;
    }

    void await_resume() {
      waiter_state->stop_callback.reset();
      if (waiter_state->cancelled) {
        throw operation_cancelled();
      }
    }
  };

  co_await awaiter{this, stop_token, std::make_shared<waiter>()};
}

inline void local_async_event::set() {
  detail::require_runtime_thread(*runtime_, "local_async_event::set");
  signaled_ = true;

  detail::prune_inactive(waiters_);
  while (!waiters_.empty()) {
    auto waiter_state = std::move(waiters_.front());
    waiters_.pop_front();
    waiter_state->active = false;
    waiter_state->completed = true;
    if (waiter_state->continuation != nullptr) {
      waiter_state->continuation.resume();
    }
    detail::prune_inactive(waiters_);
  }
}

inline void local_async_event::reset() {
  detail::require_runtime_thread(*runtime_, "local_async_event::reset");
  signaled_ = false;
}

inline auto local_async_event::is_set() const -> bool {
  detail::require_runtime_thread(*runtime_, "local_async_event::is_set");
  return signaled_;
}

}  // namespace luvcoro
