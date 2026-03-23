#pragma once

#include <chrono>
#include <concepts>
#include <exception>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <vector>

#include <coro/concepts/awaitable.hpp>
#include <coro/coro.hpp>

#include "luvcoro/error.hpp"
#include "luvcoro/runtime.hpp"

namespace luvcoro {

inline void throw_if_stop_requested(std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }
}

namespace detail {

template <typename Factory>
using controlled_awaitable_t =
    decltype(std::declval<std::decay_t<Factory>&>()(std::declval<std::stop_token>()));

template <typename Factory>
using controlled_result_t =
    std::remove_cvref_t<
        typename coro::concepts::awaitable_traits<controlled_awaitable_t<Factory>>::awaiter_return_type>;

template <typename T>
struct operation_outcome {
  enum class kind {
    value,
    cancelled,
    timeout,
    exception,
  };

  kind status = kind::value;
  std::optional<T> value{};
  std::exception_ptr exception{};
};

template <>
struct operation_outcome<void> {
  enum class kind {
    value,
    cancelled,
    timeout,
    exception,
  };

  kind status = kind::value;
  std::exception_ptr exception{};
};

template <typename T>
auto make_cancelled_outcome() -> operation_outcome<T> {
  operation_outcome<T> outcome{};
  outcome.status = operation_outcome<T>::kind::cancelled;
  return outcome;
}

template <typename T>
auto make_timeout_outcome() -> operation_outcome<T> {
  operation_outcome<T> outcome{};
  outcome.status = operation_outcome<T>::kind::timeout;
  return outcome;
}

template <typename T>
auto make_exception_outcome(std::exception_ptr exception) -> operation_outcome<T> {
  operation_outcome<T> outcome{};
  outcome.status = operation_outcome<T>::kind::exception;
  outcome.exception = exception;
  return outcome;
}

template <typename T>
auto make_value_outcome(T value) -> operation_outcome<T> {
  operation_outcome<T> outcome{};
  outcome.status = operation_outcome<T>::kind::value;
  outcome.value.emplace(std::move(value));
  return outcome;
}

inline auto make_value_outcome() -> operation_outcome<void> {
  operation_outcome<void> outcome{};
  outcome.status = operation_outcome<void>::kind::value;
  return outcome;
}

template <typename T, typename Factory>
auto wrap_operation(Factory&& factory, std::stop_token stop_token)
    -> coro::task<operation_outcome<T>> {
  try {
    if constexpr (std::is_void_v<T>) {
      co_await std::forward<Factory>(factory)(stop_token);
      co_return make_value_outcome();
    } else {
      co_return make_value_outcome<T>(
          co_await std::forward<Factory>(factory)(stop_token));
    }
  } catch (const operation_timeout&) {
    co_return make_timeout_outcome<T>();
  } catch (const operation_cancelled&) {
    co_return make_cancelled_outcome<T>();
  } catch (...) {
    co_return make_exception_outcome<T>(std::current_exception());
  }
}

template <typename T>
auto timeout_outcome_task(uv_runtime& runtime,
                          std::chrono::milliseconds timeout,
                          std::stop_token cancel_token)
    -> coro::task<operation_outcome<T>> {
  try {
    co_await sleep_for(runtime, timeout, cancel_token);
    co_return make_timeout_outcome<T>();
  } catch (const operation_cancelled&) {
    co_return make_cancelled_outcome<T>();
  }
}

template <typename T>
auto cancellation_outcome_task(uv_runtime& runtime,
                               std::stop_token stop_token,
                               std::stop_token cancel_token)
    -> coro::task<operation_outcome<T>> {
  auto result = co_await wait_for_stop(runtime, stop_token, cancel_token);
  if (result == stop_wait_result::requested) {
    co_return make_cancelled_outcome<T>();
  }

  co_return make_cancelled_outcome<T>();
}

template <typename T>
auto resolve_operation_outcome(operation_outcome<T> outcome) -> T {
  switch (outcome.status) {
    case operation_outcome<T>::kind::value:
      return std::move(*outcome.value);
    case operation_outcome<T>::kind::cancelled:
      throw operation_cancelled();
    case operation_outcome<T>::kind::timeout:
      throw operation_timeout();
    case operation_outcome<T>::kind::exception:
      std::rethrow_exception(outcome.exception);
  }

  throw std::runtime_error("unexpected operation outcome");
}

inline void resolve_operation_outcome(operation_outcome<void> outcome) {
  switch (outcome.status) {
    case operation_outcome<void>::kind::value:
      return;
    case operation_outcome<void>::kind::cancelled:
      throw operation_cancelled();
    case operation_outcome<void>::kind::timeout:
      throw operation_timeout();
    case operation_outcome<void>::kind::exception:
      std::rethrow_exception(outcome.exception);
  }

  throw std::runtime_error("unexpected operation outcome");
}

template <typename Factory>
auto run_controlled(uv_runtime& runtime,
                    std::stop_token stop_token,
                    std::optional<std::chrono::milliseconds> timeout,
                    Factory&& factory)
    -> coro::task<controlled_result_t<Factory>> {
  using result_t = controlled_result_t<Factory>;

  static_assert(!std::is_reference_v<result_t>,
                "luvcoro timeout/cancellation helpers do not support reference return types");

  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  if (timeout.has_value() && timeout->count() <= 0) {
    throw operation_timeout();
  }

  std::stop_source cancel_source;
  std::vector<coro::task<operation_outcome<result_t>>> contenders;
  contenders.emplace_back(
      wrap_operation<result_t>(std::forward<Factory>(factory), cancel_source.get_token()));

  if (timeout.has_value()) {
    contenders.emplace_back(
        timeout_outcome_task<result_t>(runtime, *timeout, cancel_source.get_token()));
  }

  if (stop_token.stop_possible()) {
    contenders.emplace_back(
        cancellation_outcome_task<result_t>(runtime, stop_token, cancel_source.get_token()));
  }

  auto outcome = co_await coro::when_any(cancel_source, std::move(contenders));
  if constexpr (std::is_void_v<result_t>) {
    resolve_operation_outcome(std::move(outcome));
    co_return;
  } else {
    co_return resolve_operation_outcome(std::move(outcome));
  }
}

}  // namespace detail

template <typename Factory>
requires requires(std::decay_t<Factory>& fn, std::stop_token stop_token) {
  fn(stop_token);
}
auto with_stop_token(uv_runtime& runtime, std::stop_token stop_token, Factory&& factory)
    -> coro::task<detail::controlled_result_t<Factory>> {
  co_return co_await detail::run_controlled(
      runtime, stop_token, std::nullopt, std::forward<Factory>(factory));
}

template <typename Factory>
requires requires(std::decay_t<Factory>& fn, std::stop_token stop_token) {
  fn(stop_token);
}
auto with_stop_token(std::stop_token stop_token, Factory&& factory)
    -> coro::task<detail::controlled_result_t<Factory>> {
  co_return co_await with_stop_token(current_runtime(),
                                     stop_token,
                                     std::forward<Factory>(factory));
}

template <typename Rep, typename Period, typename Factory>
requires requires(std::decay_t<Factory>& fn, std::stop_token stop_token) {
  fn(stop_token);
}
auto with_timeout(uv_runtime& runtime,
                  std::chrono::duration<Rep, Period> timeout,
                  Factory&& factory) -> coro::task<detail::controlled_result_t<Factory>> {
  co_return co_await detail::run_controlled(
      runtime,
      {},
      std::chrono::duration_cast<std::chrono::milliseconds>(timeout),
      std::forward<Factory>(factory));
}

template <typename Rep, typename Period, typename Factory>
requires requires(std::decay_t<Factory>& fn, std::stop_token stop_token) {
  fn(stop_token);
}
auto with_timeout(std::chrono::duration<Rep, Period> timeout,
                  Factory&& factory) -> coro::task<detail::controlled_result_t<Factory>> {
  co_return co_await with_timeout(current_runtime(),
                                  timeout,
                                  std::forward<Factory>(factory));
}

template <typename Rep, typename Period, typename Factory>
requires requires(std::decay_t<Factory>& fn, std::stop_token stop_token) {
  fn(stop_token);
}
auto with_timeout(uv_runtime& runtime,
                  std::stop_token stop_token,
                  std::chrono::duration<Rep, Period> timeout,
                  Factory&& factory) -> coro::task<detail::controlled_result_t<Factory>> {
  co_return co_await detail::run_controlled(
      runtime,
      stop_token,
      std::chrono::duration_cast<std::chrono::milliseconds>(timeout),
      std::forward<Factory>(factory));
}

template <typename Rep, typename Period, typename Factory>
requires requires(std::decay_t<Factory>& fn, std::stop_token stop_token) {
  fn(stop_token);
}
auto with_timeout(std::stop_token stop_token,
                  std::chrono::duration<Rep, Period> timeout,
                  Factory&& factory) -> coro::task<detail::controlled_result_t<Factory>> {
  co_return co_await with_timeout(current_runtime(),
                                  stop_token,
                                  timeout,
                                  std::forward<Factory>(factory));
}

template <typename Clock, typename Duration, typename Factory>
requires requires(std::decay_t<Factory>& fn, std::stop_token stop_token) {
  fn(stop_token);
}
auto with_deadline(uv_runtime& runtime,
                   std::chrono::time_point<Clock, Duration> deadline,
                   Factory&& factory) -> coro::task<detail::controlled_result_t<Factory>> {
  const auto now = Clock::now();
  if (deadline <= now) {
    throw operation_timeout();
  }

  co_return co_await with_timeout(
      runtime,
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now),
      std::forward<Factory>(factory));
}

template <typename Clock, typename Duration, typename Factory>
requires requires(std::decay_t<Factory>& fn, std::stop_token stop_token) {
  fn(stop_token);
}
auto with_deadline(std::chrono::time_point<Clock, Duration> deadline,
                   Factory&& factory) -> coro::task<detail::controlled_result_t<Factory>> {
  co_return co_await with_deadline(current_runtime(),
                                   deadline,
                                   std::forward<Factory>(factory));
}

template <typename Clock, typename Duration, typename Factory>
requires requires(std::decay_t<Factory>& fn, std::stop_token stop_token) {
  fn(stop_token);
}
auto with_deadline(uv_runtime& runtime,
                   std::stop_token stop_token,
                   std::chrono::time_point<Clock, Duration> deadline,
                   Factory&& factory) -> coro::task<detail::controlled_result_t<Factory>> {
  const auto now = Clock::now();
  if (deadline <= now) {
    throw operation_timeout();
  }

  co_return co_await with_timeout(
      runtime,
      stop_token,
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now),
      std::forward<Factory>(factory));
}

template <typename Clock, typename Duration, typename Factory>
requires requires(std::decay_t<Factory>& fn, std::stop_token stop_token) {
  fn(stop_token);
}
auto with_deadline(std::stop_token stop_token,
                   std::chrono::time_point<Clock, Duration> deadline,
                   Factory&& factory) -> coro::task<detail::controlled_result_t<Factory>> {
  co_return co_await with_deadline(current_runtime(),
                                   stop_token,
                                   deadline,
                                   std::forward<Factory>(factory));
}

}  // namespace luvcoro
