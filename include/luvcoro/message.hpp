#pragma once

#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <coro/coro.hpp>
#include <uv.h>

#include "luvcoro/error.hpp"
#include "luvcoro/runtime.hpp"

namespace luvcoro {

template <typename T>
class message_channel {
 private:
  struct state;
  struct timer_state;

 public:
  class blocking_sentinel {};

  class async_message {
   public:
    async_message() = default;

    auto operator co_await() const {
      struct awaiter {
        std::shared_ptr<bool> stop_requested;
        coro::task<std::optional<T>> task;

        bool await_ready() const noexcept {
          return task.is_ready();
        }

        auto await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept
            -> std::coroutine_handle<> {
          task.promise().continuation(awaiting_coroutine);
          return task.handle();
        }

        auto await_resume() -> std::optional<T> {
          auto result = std::move(task.promise()).result();
          if (!result.has_value() && stop_requested != nullptr) {
            *stop_requested = true;
          }
          return result;
        }
      };

      if (timeout_.has_value()) {
        if (stop_token_.stop_possible()) {
          return awaiter{
              stop_requested_,
              message_channel<T>::recv_until_stop_task(
                  state_, stop_token_, *timeout_)};
        }

        return awaiter{
            stop_requested_,
            message_channel<T>::recv_for_task(state_, *timeout_)};
      }

      return awaiter{
          stop_requested_,
          message_channel<T>::recv_optional_task(state_)};
    }

   private:
    friend class message_channel;

    async_message(std::shared_ptr<typename message_channel<T>::state> state,
                  std::shared_ptr<bool> stop_requested,
                  std::optional<std::chrono::milliseconds> timeout,
                  std::stop_token stop_token)
        : state_(std::move(state)),
          stop_requested_(std::move(stop_requested)),
          timeout_(timeout),
          stop_token_(std::move(stop_token)) {}

    std::shared_ptr<typename message_channel<T>::state> state_{};
    std::shared_ptr<bool> stop_requested_{};
    std::optional<std::chrono::milliseconds> timeout_{};
    std::stop_token stop_token_{};
  };

  class blocking_iterator {
   public:
    using iterator_category = std::input_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = T;
    using reference = const T&;
    using pointer = const T*;

    blocking_iterator() = default;

    explicit blocking_iterator(std::shared_ptr<typename message_channel<T>::state> state)
        : state_(std::move(state)) {
      fetch_next();
    }

    auto operator*() const -> reference {
      return *current_;
    }

    auto operator->() const -> pointer {
      return std::addressof(*current_);
    }

    auto operator++() -> blocking_iterator& {
      fetch_next();
      return *this;
    }

    void operator++(int) {
      (void)operator++();
    }

    friend auto operator==(const blocking_iterator& it, blocking_sentinel) noexcept
        -> bool {
      return it.state_ == nullptr;
    }

    friend auto operator!=(const blocking_iterator& it, blocking_sentinel s) noexcept
        -> bool {
      return !(it == s);
    }

    friend auto operator==(blocking_sentinel s, const blocking_iterator& it) noexcept
        -> bool {
      return it == s;
    }

    friend auto operator!=(blocking_sentinel s, const blocking_iterator& it) noexcept
        -> bool {
      return !(it == s);
    }

   private:
    void fetch_next() {
      current_.reset();
      if (state_ == nullptr) {
        return;
      }

      current_ = message_channel<T>::blocking_next(state_);
      if (!current_.has_value()) {
        state_.reset();
      }
    }

    std::shared_ptr<typename message_channel<T>::state> state_{};
    std::optional<T> current_{};
  };

  message_channel() : message_channel(current_runtime(), 0) {}

  explicit message_channel(std::size_t capacity)
      : message_channel(current_runtime(), capacity) {}

  explicit message_channel(uv_runtime& runtime, std::size_t capacity = 0)
      : state_(std::make_shared<state>(&runtime, capacity)) {
    state_->runtime->call([state = state_](uv_loop_t* loop) {
      state->async = std::make_unique<uv_async_t>();
      state->async_context_handle = std::make_unique<typename state::async_context>();
      int rc = uv_async_init(loop, state->async.get(), &state::on_async);
      if (rc < 0) {
        state->async.reset();
        state->async_context_handle.reset();
        throw_uv_error(rc, "uv_async_init");
      }

      state->async_context_handle->owner = state.get();
      state->async->data = state->async_context_handle.get();
      state->handle_initialized = true;
    });
  }

  ~message_channel() {
    close_noexcept();
  }

  message_channel(const message_channel&) = delete;
  message_channel& operator=(const message_channel&) = delete;

  message_channel(message_channel&& other) noexcept
      : state_(std::move(other.state_)) {}

  auto operator=(message_channel&& other) noexcept -> message_channel& {
    if (this == &other) {
      return *this;
    }

    close_noexcept();
    state_ = std::move(other.state_);
    return *this;
  }

  void send(const T& value) {
    blocking_send(value);
  }

  void send(T&& value) {
    blocking_send(std::move(value));
  }

  auto try_send(const T& value) -> bool {
    return try_send_impl(value);
  }

  auto try_send(T&& value) -> bool {
    return try_send_impl(std::move(value));
  }

  template <typename... Args>
  void emplace(Args&&... args) {
    blocking_send(T(std::forward<Args>(args)...));
  }

  template <typename... Args>
  auto async_emplace(Args&&... args) -> coro::task<void> {
    co_await async_send(T(std::forward<Args>(args)...));
  }

  auto async_send(const T& value) -> coro::task<void> {
    co_await async_send_impl(value);
  }

  auto async_send(T&& value) -> coro::task<void> {
    co_await async_send_impl(std::move(value));
  }

  auto recv() -> coro::task<T> {
    co_return co_await recv_task(require_state());
  }

  auto next() -> async_message {
    return async_message{require_state(), {}, std::nullopt, {}};
  }

  template <typename Rep, typename Period>
  auto next_for(std::chrono::duration<Rep, Period> timeout) -> async_message {
    return async_message{
        require_state(),
        {},
        std::chrono::duration_cast<std::chrono::milliseconds>(timeout),
        {}};
  }

  template <typename Rep, typename Period>
  auto next_until(std::stop_token stop_token,
                  std::chrono::duration<Rep, Period> poll_interval =
                      std::chrono::milliseconds(50)) -> async_message {
    return async_message{
        require_state(),
        {},
        std::chrono::duration_cast<std::chrono::milliseconds>(poll_interval),
        stop_token};
  }

  template <typename Rep, typename Period>
  auto recv_for(std::chrono::duration<Rep, Period> timeout)
      -> coro::task<std::optional<T>> {
    co_return co_await recv_for_task(
        require_state(),
        std::chrono::duration_cast<std::chrono::milliseconds>(timeout));
  }

  auto try_recv() -> std::optional<T> {
    auto state = require_state();
    std::shared_ptr<sender_waiter> sender;
    bool wake_loop = false;
    bool notify_blocked_senders = false;
    std::optional<T> value;
    std::vector<std::shared_ptr<timer_state>> timers_to_cancel;

    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (!state->messages.empty()) {
        value.emplace(std::move(state->messages.front()));
        state->messages.pop_front();
        pump_locked(*state, wake_loop, notify_blocked_senders, timers_to_cancel);
      } else {
        sender = pop_sender_waiter_locked(*state);
        if (sender != nullptr) {
          value.emplace(std::move(*sender->value));
          sender->value.reset();
          complete_sender_locked(*state, sender);
          wake_loop = true;
          notify_blocked_senders = true;
        }
      }
    }

    finalize_after_unlock(state, wake_loop, notify_blocked_senders, timers_to_cancel);
    return value;
  }

  void close() {
    auto state = require_state();
    bool should_schedule_close = false;
    std::vector<std::shared_ptr<timer_state>> timers_to_cancel;

    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (!state->closed) {
        state->closed = true;
      }

      while (auto receiver = pop_receiver_waiter_locked(*state)) {
        complete_receiver_exception_locked(
            *state, receiver, make_closed_exception(), timers_to_cancel);
      }

      while (auto sender = pop_sender_waiter_locked(*state)) {
        complete_sender_exception_locked(*state, sender, make_closed_exception());
      }

      if (!state->handle_closing) {
        state->handle_closing = true;
        should_schedule_close = true;
      }
    }

    state->send_cv.notify_all();
    cancel_timers(timers_to_cancel);

    if (should_schedule_close) {
      state->runtime->post([state]() {
        state->flush_ready_waiters();
        if (state->async != nullptr &&
            !uv_is_closing(as_uv_handle(state->async.get()))) {
          state->async_context_handle->keep_alive_on_close = state;
          uv_close(as_uv_handle(state->async.get()), [](uv_handle_t* handle) {
            auto* ctx = static_cast<typename state::async_context*>(handle->data);
            ctx->keep_alive_on_close.reset();
          });
        }
      });
    }
  }

  auto is_closed() const noexcept -> bool {
    if (!state_) {
      return true;
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->closed;
  }

  auto capacity() const noexcept -> std::size_t {
    if (!state_) {
      return 0;
    }

    return state_->capacity;
  }

  auto size() const -> std::size_t {
    auto state = require_state();
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->messages.size();
  }

  auto empty() const -> bool {
    return size() == 0;
  }

  auto messages() -> coro::generator<async_message> {
    auto channel_state = require_state();
    auto stop_requested = std::make_shared<bool>(false);

    while (!*stop_requested) {
      co_yield async_message{channel_state, stop_requested, std::nullopt, {}};
    }
  }

  template <typename Rep, typename Period>
  auto messages_for(std::chrono::duration<Rep, Period> timeout)
      -> coro::generator<async_message> {
    auto channel_state = require_state();
    auto stop_requested = std::make_shared<bool>(false);
    const auto timeout_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(timeout);

    while (!*stop_requested) {
      co_yield async_message{channel_state, stop_requested, timeout_ms, {}};
    }
  }

  template <typename Rep, typename Period>
  auto messages_until(std::stop_token stop_token,
                      std::chrono::duration<Rep, Period> poll_interval =
                          std::chrono::milliseconds(50))
      -> coro::generator<async_message> {
    auto channel_state = require_state();
    auto stop_requested = std::make_shared<bool>(false);
    auto poll_ms = std::chrono::duration_cast<std::chrono::milliseconds>(poll_interval);
    if (poll_ms.count() <= 0) {
      poll_ms = std::chrono::milliseconds(1);
    }

    while (!*stop_requested && !stop_token.stop_requested()) {
      co_yield async_message{channel_state, stop_requested, poll_ms, stop_token};
    }
  }

  auto begin() -> blocking_iterator {
    return blocking_iterator(require_state());
  }

  auto end() const noexcept -> blocking_sentinel {
    return {};
  }

 private:
  struct receiver_waiter {
    std::coroutine_handle<> continuation{};
    std::optional<T> value{};
    std::exception_ptr exception{};
    bool active = true;
    bool completed = false;
    bool timed_out = false;
    std::shared_ptr<timer_state> timer{};
  };

  struct sender_waiter {
    std::coroutine_handle<> continuation{};
    std::optional<T> value{};
    std::exception_ptr exception{};
    bool active = true;
    bool completed = false;
  };

  class closed_error : public std::runtime_error {
   public:
    closed_error() : std::runtime_error("message_channel is closed") {}
  };

  struct state {
    explicit state(uv_runtime* runtime_in, std::size_t capacity_in)
        : runtime(runtime_in), capacity(capacity_in) {}

    struct async_context {
      state* owner = nullptr;
      std::shared_ptr<state> keep_alive_on_close{};
    };

    static void on_async(uv_async_t* handle) {
      auto* ctx = static_cast<async_context*>(handle->data);
      ctx->owner->flush_ready_waiters();
    }

    void flush_ready_waiters() {
      std::deque<std::shared_ptr<receiver_waiter>> ready_receivers_local;
      std::deque<std::shared_ptr<sender_waiter>> ready_senders_local;
      {
        std::lock_guard<std::mutex> lock(mutex);
        ready_receivers_local.swap(ready_receivers);
        ready_senders_local.swap(ready_senders);
      }

      while (!ready_receivers_local.empty()) {
        auto waiter = std::move(ready_receivers_local.front());
        ready_receivers_local.pop_front();
        waiter->continuation.resume();
      }

      while (!ready_senders_local.empty()) {
        auto waiter = std::move(ready_senders_local.front());
        ready_senders_local.pop_front();
        waiter->continuation.resume();
      }
    }

    uv_runtime* runtime;
    std::size_t capacity = 0;
    std::unique_ptr<uv_async_t> async{};
    std::unique_ptr<async_context> async_context_handle{};
    std::mutex mutex;
    std::condition_variable send_cv;
    std::deque<T> messages;
    std::deque<std::shared_ptr<receiver_waiter>> receiver_waiters;
    std::deque<std::shared_ptr<sender_waiter>> sender_waiters;
    std::deque<std::shared_ptr<receiver_waiter>> ready_receivers;
    std::deque<std::shared_ptr<sender_waiter>> ready_senders;
    bool closed = false;
    bool handle_initialized = false;
    bool handle_closing = false;
  };

  struct timer_state : std::enable_shared_from_this<timer_state> {
    std::weak_ptr<receiver_waiter> waiter;
    std::shared_ptr<state> channel_state{};
    std::unique_ptr<uv_timer_t> timer{};
    std::shared_ptr<timer_state> keep_alive_on_close{};
    bool canceled = false;
    bool closing = false;

    void start(std::chrono::milliseconds timeout) {
      if (canceled || channel_state == nullptr) {
        return;
      }

      auto receiver = waiter.lock();
      if (receiver == nullptr || !receiver->active || receiver->completed) {
        return;
      }

      timer = std::make_unique<uv_timer_t>();
      int rc = uv_timer_init(channel_state->runtime->loop(), timer.get());
      if (rc < 0) {
        fail(make_uv_exception(rc, "uv_timer_init"));
        return;
      }

      timer->data = this;
      rc = uv_timer_start(
          timer.get(),
          [](uv_timer_t* handle) {
            static_cast<timer_state*>(handle->data)->fire();
          },
          static_cast<uint64_t>(timeout.count()), 0);
      if (rc < 0) {
        fail(make_uv_exception(rc, "uv_timer_start"));
        close_handle();
      }
    }

    void cancel() {
      canceled = true;
      close_handle();
    }

    void fire() {
      auto keep_alive = this->shared_from_this();
      if (channel_state == nullptr) {
        close_handle();
        return;
      }

      auto receiver = waiter.lock();
      {
        std::lock_guard<std::mutex> lock(channel_state->mutex);
        if (receiver != nullptr && receiver->active && !receiver->completed) {
          receiver->active = false;
          receiver->completed = true;
          receiver->timed_out = true;
          receiver->timer.reset();
          channel_state->ready_receivers.push_back(receiver);
        }
      }

      channel_state->flush_ready_waiters();
      close_handle();
    }

    void fail(std::exception_ptr exception) {
      auto keep_alive = this->shared_from_this();
      if (channel_state == nullptr) {
        return;
      }

      auto receiver = waiter.lock();
      {
        std::lock_guard<std::mutex> lock(channel_state->mutex);
        if (receiver != nullptr && receiver->active && !receiver->completed) {
          receiver->active = false;
          receiver->completed = true;
          receiver->exception = std::move(exception);
          receiver->timer.reset();
          channel_state->ready_receivers.push_back(receiver);
        }
      }

      channel_state->flush_ready_waiters();
    }

    void close_handle() {
      if (timer == nullptr || closing) {
        return;
      }

      closing = true;
      keep_alive_on_close = this->shared_from_this();
      uv_timer_stop(timer.get());
      uv_close(as_uv_handle(timer.get()), [](uv_handle_t* handle) {
        auto* self = static_cast<timer_state*>(handle->data);
        self->timer.reset();
        self->keep_alive_on_close.reset();
      });
    }
  };

  template <typename U>
  void blocking_send(U&& value) {
    auto state = require_state();
    bool wake_loop = false;
    std::vector<std::shared_ptr<timer_state>> timers_to_cancel;

    std::unique_lock<std::mutex> lock(state->mutex);
    if (state->closed || state->handle_closing) {
      throw std::runtime_error("message_channel is closed");
    }
    if (!state->handle_initialized || state->async == nullptr) {
      throw std::runtime_error("message_channel is not initialized");
    }

    const bool loop_thread = state->runtime->is_loop_thread();
    while (!can_send_now_locked(*state)) {
      if (loop_thread) {
        throw std::runtime_error(
            "message_channel::send would block on the runtime thread; use async_send");
      }

      state->send_cv.wait(lock, [&]() {
        return state->closed || state->handle_closing || can_send_now_locked(*state);
      });

      if (state->closed || state->handle_closing) {
        throw std::runtime_error("message_channel is closed");
      }
    }

    place_send_locked(
        *state, std::forward<U>(value), wake_loop, timers_to_cancel);
    lock.unlock();

    finalize_after_unlock(state, wake_loop, false, timers_to_cancel);
  }

  template <typename U>
  auto try_send_impl(U&& value) -> bool {
    auto state = require_state();
    bool wake_loop = false;
    std::vector<std::shared_ptr<timer_state>> timers_to_cancel;

    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->closed || state->handle_closing) {
        throw std::runtime_error("message_channel is closed");
      }
      if (!state->handle_initialized || state->async == nullptr) {
        throw std::runtime_error("message_channel is not initialized");
      }
      if (!can_send_now_locked(*state)) {
        return false;
      }

      place_send_locked(
          *state, std::forward<U>(value), wake_loop, timers_to_cancel);
    }

    finalize_after_unlock(state, wake_loop, false, timers_to_cancel);
    return true;
  }

  template <typename U>
  auto async_send_impl(U&& value) -> coro::task<void> {
    auto channel_state = require_state();
    auto sender = std::make_shared<sender_waiter>();
    sender->value.emplace(std::forward<U>(value));

    struct awaiter {
      std::shared_ptr<typename message_channel<T>::state> shared_state;
      std::shared_ptr<typename message_channel<T>::sender_waiter> waiter;
      bool wake_loop = false;
      std::vector<std::shared_ptr<typename message_channel<T>::timer_state>>
          timers_to_cancel;

      bool await_ready() {
        std::lock_guard<std::mutex> lock(shared_state->mutex);
        if (shared_state->closed || shared_state->handle_closing) {
          waiter->exception = message_channel<T>::make_closed_exception();
          waiter->completed = true;
          return true;
        }
        if (!shared_state->handle_initialized || shared_state->async == nullptr) {
          waiter->exception = std::make_exception_ptr(
              std::runtime_error("message_channel is not initialized"));
          waiter->completed = true;
          return true;
        }
        if (!message_channel<T>::can_send_now_locked(*shared_state)) {
          return false;
        }

        message_channel<T>::place_send_locked(
            *shared_state, std::move(*waiter->value), wake_loop, timers_to_cancel);
        waiter->value.reset();
        waiter->completed = true;
        return true;
      }

      bool await_suspend(std::coroutine_handle<> h) {
        waiter->continuation = h;

        {
          std::lock_guard<std::mutex> lock(shared_state->mutex);
          if (shared_state->closed || shared_state->handle_closing) {
            waiter->exception = message_channel<T>::make_closed_exception();
            waiter->completed = true;
            return false;
          }
          if (!shared_state->handle_initialized || shared_state->async == nullptr) {
            waiter->exception = std::make_exception_ptr(
                std::runtime_error("message_channel is not initialized"));
            waiter->completed = true;
            return false;
          }
          if (message_channel<T>::can_send_now_locked(*shared_state)) {
            message_channel<T>::place_send_locked(
                *shared_state, std::move(*waiter->value), wake_loop,
                timers_to_cancel);
            waiter->value.reset();
            waiter->completed = true;
            return false;
          }

          shared_state->sender_waiters.push_back(waiter);
        }

        return true;
      }

      void await_resume() {
        message_channel<T>::finalize_after_unlock(
            shared_state, wake_loop, false, timers_to_cancel);
        if (waiter->exception != nullptr) {
          std::rethrow_exception(waiter->exception);
        }
      }
    };

    co_await awaiter{
        std::move(channel_state),
        std::move(sender),
        false,
        {}};
  }

  static auto recv_for_task(const std::shared_ptr<state>& channel_state,
                            std::chrono::milliseconds timeout)
      -> coro::task<std::optional<T>> {
    struct awaiter {
      std::shared_ptr<typename message_channel<T>::state> shared_state;
      std::shared_ptr<typename message_channel<T>::receiver_waiter> waiter =
          std::make_shared<typename message_channel<T>::receiver_waiter>();
      std::chrono::milliseconds timeout;
      bool wake_loop = false;
      bool notify_blocked_senders = false;
      std::vector<std::shared_ptr<typename message_channel<T>::timer_state>>
          timers_to_cancel;

      bool await_ready() {
        std::lock_guard<std::mutex> lock(shared_state->mutex);
        if (message_channel<T>::try_recv_locked(
                *shared_state, waiter, wake_loop, notify_blocked_senders,
                timers_to_cancel)) {
          return true;
        }

        if (timeout.count() <= 0) {
          waiter->completed = true;
          waiter->timed_out = true;
          return true;
        }

        return false;
      }

      bool await_suspend(std::coroutine_handle<> h) {
        waiter->continuation = h;

        {
          std::lock_guard<std::mutex> lock(shared_state->mutex);
          if (message_channel<T>::try_recv_locked(
                  *shared_state, waiter, wake_loop, notify_blocked_senders,
                  timers_to_cancel)) {
            return false;
          }

          if (timeout.count() <= 0) {
            waiter->completed = true;
            waiter->timed_out = true;
            return false;
          }

          auto timer = std::make_shared<typename message_channel<T>::timer_state>();
          timer->waiter = waiter;
          timer->channel_state = shared_state;
          waiter->timer = timer;
          shared_state->receiver_waiters.push_back(waiter);
        }

        try {
          shared_state->runtime->post([timer = waiter->timer, timeout = timeout]() {
            timer->start(timeout);
          });
        } catch (...) {
          std::lock_guard<std::mutex> lock(shared_state->mutex);
          if (waiter->active && !waiter->completed) {
            waiter->active = false;
            waiter->completed = true;
            waiter->exception = std::current_exception();
            waiter->timer.reset();
            shared_state->ready_receivers.push_back(waiter);
            wake_loop = true;
          }
          return false;
        }

        return true;
      }

      auto await_resume() -> std::optional<T> {
        message_channel<T>::finalize_after_unlock(
            shared_state, wake_loop, notify_blocked_senders, timers_to_cancel);

        if (waiter->exception != nullptr) {
          std::rethrow_exception(waiter->exception);
        }
        if (waiter->timed_out) {
          return std::nullopt;
        }
        return std::move(waiter->value);
      }
    };

    co_return co_await awaiter{
        std::move(channel_state),
        std::make_shared<receiver_waiter>(),
        timeout,
        false,
        false,
        {}}; 
  }

  static auto recv_optional_task(const std::shared_ptr<state>& channel_state)
      -> coro::task<std::optional<T>> {
    try {
      co_return co_await recv_task(channel_state);
    } catch (const closed_error&) {
      co_return std::nullopt;
    }
  }

  static auto recv_until_stop_task(const std::shared_ptr<state>& channel_state,
                                   std::stop_token stop_token,
                                   std::chrono::milliseconds poll_interval)
      -> coro::task<std::optional<T>> {
    if (poll_interval.count() <= 0) {
      poll_interval = std::chrono::milliseconds(1);
    }

    while (!stop_token.stop_requested()) {
      try {
        auto result = co_await recv_for_task(channel_state, poll_interval);
        if (result.has_value()) {
          co_return result;
        }
      } catch (const closed_error&) {
        co_return std::nullopt;
      }
    }

    co_return std::nullopt;
  }

  static auto make_closed_exception() -> std::exception_ptr {
    return std::make_exception_ptr(closed_error{});
  }

  static auto make_uv_exception(int code, const char* op) -> std::exception_ptr {
    return std::make_exception_ptr(
        uv_error(code, std::string(op) + " failed: " + uv_strerror(code)));
  }

  template <typename Waiter>
  static void prune_waiters(std::deque<std::shared_ptr<Waiter>>& waiters) {
    while (!waiters.empty() && !waiters.front()->active) {
      waiters.pop_front();
    }
  }

  static auto pop_receiver_waiter_locked(state& state)
      -> std::shared_ptr<receiver_waiter> {
    prune_waiters(state.receiver_waiters);
    if (state.receiver_waiters.empty()) {
      return nullptr;
    }

    auto waiter = std::move(state.receiver_waiters.front());
    state.receiver_waiters.pop_front();
    return waiter;
  }

  static auto pop_sender_waiter_locked(state& state)
      -> std::shared_ptr<sender_waiter> {
    prune_waiters(state.sender_waiters);
    if (state.sender_waiters.empty()) {
      return nullptr;
    }

    auto waiter = std::move(state.sender_waiters.front());
    state.sender_waiters.pop_front();
    return waiter;
  }

  static auto has_waiting_receivers_locked(state& state) -> bool {
    prune_waiters(state.receiver_waiters);
    return !state.receiver_waiters.empty();
  }

  static auto has_waiting_senders_locked(state& state) -> bool {
    prune_waiters(state.sender_waiters);
    return !state.sender_waiters.empty();
  }

  static auto has_buffer_capacity_locked(const state& state) -> bool {
    return state.capacity == 0 || state.messages.size() < state.capacity;
  }

  static auto can_send_now_locked(state& state) -> bool {
    if (state.closed || state.handle_closing) {
      return false;
    }

    if (has_waiting_senders_locked(state)) {
      return false;
    }

    return has_waiting_receivers_locked(state) || has_buffer_capacity_locked(state);
  }

  template <typename U>
  static void place_send_locked(
      state& state, U&& value, bool& wake_loop,
      std::vector<std::shared_ptr<timer_state>>& timers_to_cancel) {
    if (auto receiver = pop_receiver_waiter_locked(state)) {
      complete_receiver_value_locked(
          state, receiver, std::forward<U>(value), timers_to_cancel);
      wake_loop = true;
      return;
    }

    state.messages.emplace_back(std::forward<U>(value));
  }

  template <typename U>
  static void complete_receiver_value_locked(
      state& state, const std::shared_ptr<receiver_waiter>& waiter, U&& value,
      std::vector<std::shared_ptr<timer_state>>& timers_to_cancel) {
    waiter->active = false;
    waiter->completed = true;
    waiter->timed_out = false;
    waiter->value.emplace(std::forward<U>(value));
    if (waiter->timer != nullptr) {
      timers_to_cancel.push_back(waiter->timer);
      waiter->timer.reset();
    }
    if (waiter->continuation != nullptr) {
      state.ready_receivers.push_back(waiter);
    }
  }

  static void complete_receiver_exception_locked(
      state& state, const std::shared_ptr<receiver_waiter>& waiter,
      std::exception_ptr exception,
      std::vector<std::shared_ptr<timer_state>>& timers_to_cancel) {
    waiter->active = false;
    waiter->completed = true;
    waiter->exception = std::move(exception);
    if (waiter->timer != nullptr) {
      timers_to_cancel.push_back(waiter->timer);
      waiter->timer.reset();
    }
    if (waiter->continuation != nullptr) {
      state.ready_receivers.push_back(waiter);
    }
  }

  static void complete_sender_locked(
      state& state, const std::shared_ptr<sender_waiter>& waiter) {
    waiter->active = false;
    waiter->completed = true;
    if (waiter->continuation != nullptr) {
      state.ready_senders.push_back(waiter);
    }
  }

  static void complete_sender_exception_locked(
      state& state, const std::shared_ptr<sender_waiter>& waiter,
      std::exception_ptr exception) {
    waiter->active = false;
    waiter->completed = true;
    waiter->exception = std::move(exception);
    if (waiter->continuation != nullptr) {
      state.ready_senders.push_back(waiter);
    }
  }

  static void pump_locked(
      state& state, bool& wake_loop, bool& notify_blocked_senders,
      std::vector<std::shared_ptr<timer_state>>& timers_to_cancel) {
    while (true) {
      auto receiver = pop_receiver_waiter_locked(state);
      if (receiver == nullptr) {
        break;
      }

      if (!state.messages.empty()) {
        auto value = std::move(state.messages.front());
        state.messages.pop_front();
        complete_receiver_value_locked(
            state, receiver, std::move(value), timers_to_cancel);
        wake_loop = true;
        notify_blocked_senders = true;
        continue;
      }

      auto sender = pop_sender_waiter_locked(state);
      if (sender != nullptr) {
        complete_receiver_value_locked(
            state, receiver, std::move(*sender->value), timers_to_cancel);
        sender->value.reset();
        complete_sender_locked(state, sender);
        wake_loop = true;
        continue;
      }

      state.receiver_waiters.push_front(std::move(receiver));
      break;
    }

    while (has_buffer_capacity_locked(state)) {
      auto sender = pop_sender_waiter_locked(state);
      if (sender == nullptr) {
        break;
      }

      state.messages.emplace_back(std::move(*sender->value));
      sender->value.reset();
      complete_sender_locked(state, sender);
      wake_loop = true;
    }
  }

  static auto try_recv_locked(
      state& state, const std::shared_ptr<receiver_waiter>& waiter,
      bool& wake_loop, bool& notify_blocked_senders,
      std::vector<std::shared_ptr<timer_state>>& timers_to_cancel) -> bool {
    if (!state.messages.empty()) {
      auto value = std::move(state.messages.front());
      state.messages.pop_front();
      complete_receiver_value_locked(
          state, waiter, std::move(value), timers_to_cancel);
      pump_locked(state, wake_loop, notify_blocked_senders, timers_to_cancel);
      notify_blocked_senders = true;
      return true;
    }

    if (auto sender = pop_sender_waiter_locked(state)) {
      complete_receiver_value_locked(
          state, waiter, std::move(*sender->value), timers_to_cancel);
      sender->value.reset();
      complete_sender_locked(state, sender);
      wake_loop = true;
      return true;
    }

    if (state.closed || state.handle_closing) {
      complete_receiver_exception_locked(
          state, waiter, make_closed_exception(), timers_to_cancel);
      return true;
    }

    return false;
  }

  static void cancel_timers(
      const std::vector<std::shared_ptr<timer_state>>& timers_to_cancel) {
    for (const auto& timer : timers_to_cancel) {
      if (timer == nullptr || timer->channel_state == nullptr) {
        continue;
      }

      try {
        timer->channel_state->runtime->post([timer]() {
          timer->cancel();
        });
      } catch (...) {
        // Best-effort cleanup during shutdown.
      }
    }
  }

  static void notify_ready_waiters(const std::shared_ptr<state>& state) {
    if (state == nullptr) {
      return;
    }

    if (state->runtime->is_loop_thread()) {
      state->flush_ready_waiters();
      return;
    }

    if (state->async == nullptr || uv_is_closing(as_uv_handle(state->async.get()))) {
      throw std::runtime_error("message_channel async handle is closing");
    }

    int rc = uv_async_send(state->async.get());
    if (rc < 0) {
      throw_uv_error(rc, "uv_async_send");
    }
  }

  static void finalize_after_unlock(
      const std::shared_ptr<state>& state, bool wake_loop,
      bool notify_blocked_senders,
      const std::vector<std::shared_ptr<timer_state>>& timers_to_cancel) {
    if (notify_blocked_senders && state != nullptr) {
      state->send_cv.notify_all();
    }

    cancel_timers(timers_to_cancel);

    if (wake_loop) {
      notify_ready_waiters(state);
    }
  }

  static auto blocking_next(const std::shared_ptr<state>& state) -> std::optional<T> {
    if (state == nullptr) {
      return std::nullopt;
    }
    if (state->runtime->is_loop_thread()) {
      throw std::runtime_error(
          "message_channel blocking iteration cannot run on the runtime thread");
    }

    std::shared_ptr<sender_waiter> sender;
    bool wake_loop = false;
    bool notify_blocked_senders = false;
    std::vector<std::shared_ptr<timer_state>> timers_to_cancel;
    std::optional<T> value;

    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (!state->messages.empty()) {
        value.emplace(std::move(state->messages.front()));
        state->messages.pop_front();
        pump_locked(*state, wake_loop, notify_blocked_senders, timers_to_cancel);
      } else {
        sender = pop_sender_waiter_locked(*state);
        if (sender != nullptr) {
          value.emplace(std::move(*sender->value));
          sender->value.reset();
          complete_sender_locked(*state, sender);
          wake_loop = true;
          notify_blocked_senders = true;
        } else if (state->closed || state->handle_closing) {
          return std::nullopt;
        }
      }
    }

    finalize_after_unlock(state, wake_loop, notify_blocked_senders, timers_to_cancel);
    if (value.has_value()) {
      return value;
    }

    try {
      return coro::sync_wait(recv_task(state));
    } catch (const closed_error&) {
      return std::nullopt;
    }
  }

  static auto recv_task(const std::shared_ptr<state>& channel_state) -> coro::task<T> {
    struct awaiter {
      std::shared_ptr<typename message_channel<T>::state> shared_state;
      std::shared_ptr<typename message_channel<T>::receiver_waiter> waiter =
          std::make_shared<typename message_channel<T>::receiver_waiter>();
      bool wake_loop = false;
      bool notify_blocked_senders = false;
      std::vector<std::shared_ptr<typename message_channel<T>::timer_state>>
          timers_to_cancel;

      bool await_ready() {
        std::lock_guard<std::mutex> lock(shared_state->mutex);
        return message_channel<T>::try_recv_locked(
            *shared_state, waiter, wake_loop, notify_blocked_senders, timers_to_cancel);
      }

      bool await_suspend(std::coroutine_handle<> h) {
        waiter->continuation = h;

        {
          std::lock_guard<std::mutex> lock(shared_state->mutex);
          if (message_channel<T>::try_recv_locked(
                  *shared_state, waiter, wake_loop, notify_blocked_senders,
                  timers_to_cancel)) {
            return false;
          }

          shared_state->receiver_waiters.push_back(waiter);
        }

        return true;
      }

      auto await_resume() -> T {
        message_channel<T>::finalize_after_unlock(
            shared_state, wake_loop, notify_blocked_senders, timers_to_cancel);

        if (waiter->exception != nullptr) {
          std::rethrow_exception(waiter->exception);
        }

        return std::move(*waiter->value);
      }
    };

    co_return co_await awaiter{
        std::move(channel_state),
        std::make_shared<receiver_waiter>(),
        false,
        false,
        {}};
  }

  static auto as_uv_handle(uv_async_t* handle) -> uv_handle_t* {
    return reinterpret_cast<uv_handle_t*>(handle);
  }

  static auto as_uv_handle(uv_timer_t* handle) -> uv_handle_t* {
    return reinterpret_cast<uv_handle_t*>(handle);
  }

  auto require_state() const -> std::shared_ptr<state> {
    if (!state_) {
      throw std::runtime_error("message_channel has no state");
    }
    return state_;
  }

  void close_noexcept() noexcept {
    if (!state_) {
      return;
    }

    try {
      close();
    } catch (...) {
      // Destructors must not throw.
    }
  }

  std::shared_ptr<state> state_;
};

namespace detail {

template <typename Fn, typename Value>
auto co_foreach_invoke(Fn& fn, Value&& value) -> coro::task<void> {
  using result_t = std::invoke_result_t<Fn&, Value>;

  if constexpr (coro::concepts::awaitable<result_t>) {
    co_await std::invoke(fn, std::forward<Value>(value));
  } else {
    std::invoke(fn, std::forward<Value>(value));
  }
}

}  // namespace detail

template <typename T>
struct selected_message {
  std::size_t index = 0;
  std::optional<T> message{};
};

template <typename Range, typename Fn>
auto co_foreach(Range&& range, Fn&& fn) -> coro::task<void> {
  auto callback = std::forward<Fn>(fn);

  for (auto next_message : range) {
    auto message = co_await next_message;
    if (!message.has_value()) {
      break;
    }

    co_await detail::co_foreach_invoke(callback, std::move(*message));
  }
}

template <typename Range, typename Fn>
auto co_foreach(Range&& range, std::size_t max_concurrency, Fn&& fn)
    -> coro::task<void> {
  if (max_concurrency == 0) {
    throw std::runtime_error("co_foreach max_concurrency must be greater than 0");
  }

  auto callback = std::forward<Fn>(fn);
  std::vector<coro::task<void>> batch;
  batch.reserve(max_concurrency);

  for (auto next_message : range) {
    auto message = co_await next_message;
    if (!message.has_value()) {
      break;
    }

    batch.emplace_back(detail::co_foreach_invoke(callback, std::move(*message)));
    if (batch.size() >= max_concurrency) {
      co_await coro::when_all(std::move(batch));
      batch.clear();
      batch.reserve(max_concurrency);
    }
  }

  if (!batch.empty()) {
    co_await coro::when_all(std::move(batch));
  }
}

template <typename T>
auto select(message_channel<T>& channel) -> coro::task<selected_message<T>> {
  co_return selected_message<T>{0, co_await channel.next()};
}

template <typename T, typename... Ts, std::size_t... Indices>
auto select_impl(std::index_sequence<Indices...>,
                 message_channel<T>& first,
                 message_channel<Ts>&... rest)
    -> coro::task<std::variant<selected_message<T>, selected_message<Ts>...>> {
  auto channels = std::forward_as_tuple(std::ref(first), std::ref(rest)...);

  auto wrap = []<typename U>(std::size_t index,
                             message_channel<U>& channel)
      -> coro::task<selected_message<U>> {
    co_return selected_message<U>{index, co_await channel.next()};
  };

  co_return co_await coro::when_any(
      wrap(Indices, std::get<Indices>(channels).get())...);
}

template <typename T, typename... Ts>
auto select(message_channel<T>& first, message_channel<Ts>&... rest)
    -> coro::task<std::variant<selected_message<T>, selected_message<Ts>...>> {
  co_return co_await select_impl(
      std::index_sequence_for<T, Ts...>{}, first, rest...);
}

}  // namespace luvcoro
