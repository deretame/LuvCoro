#include <chrono>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <thread>

#include <coro/coro.hpp>

#include "luvcoro/control.hpp"
#include "luvcoro/runtime.hpp"

namespace {

coro::task<void> run_cancel_timeout() {
  using namespace std::chrono_literals;

  bool timeout_triggered = false;
  try {
    co_await luvcoro::with_timeout(
        25ms,
        [](std::stop_token stop_token) -> coro::task<void> {
          while (true) {
            co_await luvcoro::sleep_for(100ms, stop_token);
          }
        });
  } catch (const luvcoro::operation_timeout&) {
    timeout_triggered = true;
  }

  if (!timeout_triggered) {
    throw std::runtime_error("with_timeout did not time out");
  }

  std::stop_source stop_source;
  bool cancelled = false;
  std::jthread stopper([&stop_source]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stop_source.request_stop();
  });
  try {
    co_await luvcoro::with_stop_token(
        stop_source.get_token(),
        [](std::stop_token stop_token) -> coro::task<void> {
          while (true) {
            co_await luvcoro::sleep_for(100ms, stop_token);
          }
        });
  } catch (const luvcoro::operation_cancelled&) {
    cancelled = true;
  }

  if (!cancelled) {
    throw std::runtime_error("with_stop_token did not cancel");
  }

  bool deadline_triggered = false;
  try {
    co_await luvcoro::with_deadline(
        std::chrono::steady_clock::now() + 20ms,
        [](std::stop_token stop_token) -> coro::task<void> {
          co_await luvcoro::sleep_for(std::chrono::seconds(1), stop_token);
        });
  } catch (const luvcoro::operation_timeout&) {
    deadline_triggered = true;
  }

  if (!deadline_triggered) {
    throw std::runtime_error("with_deadline did not time out");
  }

  bool blocking_timeout_triggered = false;
  try {
    co_await luvcoro::with_timeout(
        25ms,
        [](std::stop_token stop_token) -> coro::task<void> {
          co_await luvcoro::blocking(stop_token, [](std::stop_token inner_stop_token) {
            while (!inner_stop_token.stop_requested()) {
              std::this_thread::sleep_for(5ms);
            }
            throw luvcoro::operation_cancelled();
          });
        });
  } catch (const luvcoro::operation_timeout&) {
    blocking_timeout_triggered = true;
  }

  if (!blocking_timeout_triggered) {
    throw std::runtime_error("with_timeout did not stop blocking work");
  }

  std::stop_source blocking_stop_source;
  bool blocking_cancelled = false;
  std::jthread blocking_stopper([&blocking_stop_source]() {
    std::this_thread::sleep_for(20ms);
    blocking_stop_source.request_stop();
  });
  try {
    co_await luvcoro::with_stop_token(
        blocking_stop_source.get_token(),
        [](std::stop_token stop_token) -> coro::task<void> {
          co_await luvcoro::blocking(stop_token, [](std::stop_token inner_stop_token) {
            while (!inner_stop_token.stop_requested()) {
              std::this_thread::sleep_for(5ms);
            }
            throw luvcoro::operation_cancelled();
          });
        });
  } catch (const luvcoro::operation_cancelled&) {
    blocking_cancelled = true;
  }

  if (!blocking_cancelled) {
    throw std::runtime_error("with_stop_token did not stop blocking work");
  }

  bool blocking_deadline_triggered = false;
  try {
    co_await luvcoro::with_deadline(
        std::chrono::steady_clock::now() + 20ms,
        [](std::stop_token stop_token) -> coro::task<void> {
          co_await luvcoro::blocking(stop_token, [](std::stop_token inner_stop_token) {
            while (!inner_stop_token.stop_requested()) {
              std::this_thread::sleep_for(5ms);
            }
            throw luvcoro::operation_cancelled();
          });
        });
  } catch (const luvcoro::operation_timeout&) {
    blocking_deadline_triggered = true;
  }

  if (!blocking_deadline_triggered) {
    throw std::runtime_error("with_deadline did not stop blocking work");
  }

  std::cout << "cancel and timeout controls ok" << std::endl;
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_cancel_timeout());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
