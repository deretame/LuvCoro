#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

#include <coro/coro.hpp>

#include "luvcoro/runtime.hpp"

namespace {

coro::task<void> run_blocking_roundtrip() {
  auto answer = co_await luvcoro::blocking([]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return 42;
  });

  if (answer != 42) {
    throw std::runtime_error("blocking task returned unexpected result");
  }

  bool flag = false;
  co_await luvcoro::blocking([&flag]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    flag = true;
  });

  if (!flag) {
    throw std::runtime_error("blocking void task did not complete");
  }
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime_options options;
    options.thread_pool_size = 8;

    luvcoro::uv_runtime runtime(options);
    luvcoro::run_with_runtime(runtime, run_blocking_roundtrip());
    std::cout << "blocking roundtrip ok" << std::endl;
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
