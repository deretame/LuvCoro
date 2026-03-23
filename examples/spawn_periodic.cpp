#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <coro/coro.hpp>

#include "luvcoro/runtime.hpp"

namespace {

coro::task<void> wait_until_ready(std::function<bool()> predicate,
                                  std::chrono::milliseconds timeout,
                                  std::string_view label) {
  using namespace std::chrono_literals;

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      throw std::runtime_error(std::string(label) + " timed out");
    }
    co_await luvcoro::sleep_for(5ms);
  }
}

coro::task<void> increment_counter(std::shared_ptr<std::atomic<int>> counter,
                                   std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    co_return;
  }

  counter->fetch_add(1, std::memory_order_relaxed);
}

coro::task<void> record_tick_after(
    std::shared_ptr<std::vector<std::chrono::steady_clock::time_point>> completions,
    std::chrono::milliseconds work_time,
    std::stop_token stop_token) {
  co_await luvcoro::sleep_for(work_time, stop_token);
  if (stop_token.stop_requested()) {
    co_return;
  }

  completions->push_back(std::chrono::steady_clock::now());
}

coro::task<void> run_spawn_periodic() {
  using namespace std::chrono_literals;

  {
    auto counter = std::make_shared<std::atomic<int>>(0);

    auto ticker = luvcoro::spawn_every(
        20ms,
        [counter](std::stop_token stop_token) {
          return increment_counter(counter, stop_token);
        });

    if (!ticker) {
      throw std::runtime_error("spawn_every failed to schedule periodic coroutine");
    }

    co_await wait_until_ready(
        [counter]() {
          return counter->load(std::memory_order_relaxed) >= 2;
        },
        250ms,
        "spawn_every");

    ticker.cancel();
    const auto cancelled_at = counter->load(std::memory_order_relaxed);

    co_await luvcoro::sleep_for(50ms);
    if (counter->load(std::memory_order_relaxed) != cancelled_at) {
      throw std::runtime_error("spawn_every advanced after cancellation");
    }
  }

  {
    auto counter = std::make_shared<std::atomic<int>>(0);
    const auto started_at = std::chrono::steady_clock::now();

    auto ticker = luvcoro::spawn_every_immediate(
        100ms,
        [counter](std::stop_token stop_token) {
          return increment_counter(counter, stop_token);
        });

    if (!ticker) {
      throw std::runtime_error(
          "spawn_every_immediate failed to schedule periodic coroutine");
    }

    co_await wait_until_ready(
        [counter]() {
          return counter->load(std::memory_order_relaxed) >= 1;
        },
        100ms,
        "spawn_every_immediate");
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at);
    if (elapsed.count() >= 60) {
      throw std::runtime_error("spawn_every_immediate did not fire immediately");
    }

    ticker.cancel();
    const auto cancelled_at = counter->load(std::memory_order_relaxed);
    co_await luvcoro::sleep_for(120ms);
    if (counter->load(std::memory_order_relaxed) != cancelled_at) {
      throw std::runtime_error("spawn_every_immediate advanced after cancellation");
    }
  }

  {
    auto completions =
        std::make_shared<std::vector<std::chrono::steady_clock::time_point>>();

    auto ticker = luvcoro::spawn_every_fixed_rate(
        80ms,
        [completions](std::stop_token stop_token) {
          return record_tick_after(completions, 40ms, stop_token);
        });

    if (!ticker) {
      throw std::runtime_error(
          "spawn_every_fixed_rate failed to schedule periodic coroutine");
    }

    co_await wait_until_ready(
        [completions]() {
          return completions->size() >= 2;
        },
        400ms,
        "spawn_every_fixed_rate");

    ticker.cancel();

    const auto between_ticks = std::chrono::duration_cast<std::chrono::milliseconds>(
        (*completions)[1] - (*completions)[0]);
    if (between_ticks.count() >= 105) {
      throw std::runtime_error("spawn_every_fixed_rate drifted like fixed-delay");
    }
  }

  {
    auto error_message = std::make_shared<std::optional<std::string>>();

    auto ticker = luvcoro::spawn_logged_every_immediate(
        "logged periodic",
        20ms,
        []() -> coro::task<void> {
          throw std::runtime_error("periodic boom");
          co_return;
        },
        [error_message](std::string_view name, std::exception_ptr exception) noexcept {
          try {
            if (exception != nullptr) {
              std::rethrow_exception(exception);
            }
            *error_message = std::string(name) + ": unknown exception";
          } catch (const std::exception& ex) {
            *error_message = std::string(name) + ": " + ex.what();
          } catch (...) {
            *error_message = std::string(name) + ": non-standard exception";
          }
        });

    if (!ticker) {
      throw std::runtime_error(
          "spawn_logged_every_immediate failed to schedule periodic coroutine");
    }

    co_await wait_until_ready(
        [error_message]() {
          return error_message->has_value();
        },
        100ms,
        "spawn_logged_every_immediate");
    if (*error_message != "logged periodic: periodic boom") {
      throw std::runtime_error(
          "spawn_logged_every_immediate produced unexpected log message");
    }
  }

  std::cout << "spawn periodic modes ok" << std::endl;
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_spawn_periodic());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
