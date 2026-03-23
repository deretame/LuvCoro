#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include <coro/coro.hpp>

#include "luvcoro/runtime.hpp"
#include "luvcoro/task_scope.hpp"

namespace {

coro::task<void> long_running_worker(std::stop_token stop_token,
                                     std::atomic<int>& cancelled_workers) {
  try {
    while (true) {
      co_await luvcoro::sleep_for(std::chrono::milliseconds(100), stop_token);
    }
  } catch (const luvcoro::operation_cancelled&) {
    ++cancelled_workers;
  }
}

coro::task<void> failing_worker(std::stop_token stop_token) {
  co_await luvcoro::sleep_for(std::chrono::milliseconds(30), stop_token);
  throw std::runtime_error("structured worker failed");
}

coro::task<void> run_structured_scope() {
  std::atomic<int> cancelled_workers = 0;
  luvcoro::task_scope scope;

  scope.spawn([&](std::stop_token stop_token) -> coro::task<void> {
    co_await long_running_worker(stop_token, cancelled_workers);
  });
  scope.spawn([&](std::stop_token stop_token) -> coro::task<void> {
    co_await long_running_worker(stop_token, cancelled_workers);
  });
  scope.spawn([](std::stop_token stop_token) -> coro::task<void> {
    co_await failing_worker(stop_token);
  });

  bool failed = false;
  try {
    co_await scope.join();
  } catch (const std::runtime_error& ex) {
    failed = std::string_view(ex.what()) == "structured worker failed";
  }

  if (!failed) {
    throw std::runtime_error("task_scope did not surface child failure");
  }

  if (cancelled_workers.load() != 2) {
    throw std::runtime_error("task_scope did not cancel sibling workers");
  }

  std::cout << "structured scope cancelled siblings: "
            << cancelled_workers.load() << std::endl;
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_structured_scope());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
