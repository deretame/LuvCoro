#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

#include <coro/coro.hpp>

#include "luvcoro/runtime.hpp"

namespace {

coro::task<void> run_timer_roundtrip() {
  using namespace std::chrono_literals;

  const auto before_sleep = std::chrono::steady_clock::now();
  co_await luvcoro::sleep_for(15ms);
  const auto after_sleep = std::chrono::steady_clock::now();
  const auto sleep_elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(after_sleep - before_sleep);
  if (sleep_elapsed.count() < 10) {
    throw std::runtime_error("sleep_for returned too early");
  }

  co_await luvcoro::sleep_until(std::chrono::steady_clock::now() + 10ms);

  luvcoro::steady_timer timer;
  timer.expires_after(10ms);
  co_await timer.wait();

  bool cancelled = false;
  timer.expires_after(200ms);
  std::jthread canceller([&timer]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    timer.cancel();
  });
  try {
    co_await timer.wait();
  } catch (const luvcoro::operation_cancelled&) {
    cancelled = true;
  }

  if (!cancelled) {
    throw std::runtime_error("steady_timer cancel did not interrupt wait");
  }

  std::cout << "timer roundtrip ok" << std::endl;
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_timer_roundtrip());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
