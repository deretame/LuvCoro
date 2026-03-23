#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <thread>

#include <coro/coro.hpp>

#include "luvcoro/runtime.hpp"

using namespace std::chrono_literals;

namespace {

coro::task<void> verify_queued_cancellation() {
  if (!luvcoro::spawn([]() -> coro::task<void> {
        co_await luvcoro::blocking([]() {
          std::this_thread::sleep_for(150ms);
        });
      })) {
    throw std::runtime_error("failed to occupy blocking worker");
  }

  co_await luvcoro::sleep_for(20ms);

  std::stop_source stop_source;
  std::jthread canceller([&stop_source]() {
    std::this_thread::sleep_for(30ms);
    stop_source.request_stop();
  });

  bool cancelled = false;
  try {
    co_await luvcoro::blocking(stop_source.get_token(), []() {
      std::this_thread::sleep_for(20ms);
    });
  } catch (const luvcoro::operation_cancelled&) {
    cancelled = true;
  }

  if (!cancelled) {
    throw std::runtime_error("queued blocking operation was not cancelled");
  }

  co_await luvcoro::sleep_for(180ms);
}

coro::task<void> verify_cooperative_cancellation() {
  std::stop_source stop_source;
  std::atomic<bool> started = false;

  std::jthread canceller([&stop_source, &started]() {
    while (!started.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(40ms);
    stop_source.request_stop();
  });

  try {
    co_await luvcoro::blocking(stop_source.get_token(),
                               [&started](std::stop_token stop_token) {
                                 started.store(true, std::memory_order_relaxed);
                                 while (!stop_token.stop_requested()) {
                                   std::this_thread::sleep_for(10ms);
                                 }
                                 throw luvcoro::operation_cancelled();
                               });
  } catch (const luvcoro::operation_cancelled&) {
    if (!started.load(std::memory_order_relaxed)) {
      throw std::runtime_error("cooperative blocking task never started");
    }
    co_return;
  }

  throw std::runtime_error("cooperative blocking operation was not cancelled");
}

coro::task<void> run_blocking_cancel() {
  co_await verify_queued_cancellation();
  co_await verify_cooperative_cancellation();
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime_options options;
    options.thread_pool_size = 1;

    luvcoro::uv_runtime runtime(options);
    luvcoro::run_with_runtime(runtime, run_blocking_cancel());
    std::cout << "blocking cancellation ok" << std::endl;
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
