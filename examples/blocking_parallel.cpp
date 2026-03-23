#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

#include <coro/coro.hpp>

#include "luvcoro/runtime.hpp"

namespace {

coro::task<void> blocking_job(int id, std::chrono::milliseconds delay) {
  co_await luvcoro::blocking([id, delay]() {
    std::this_thread::sleep_for(delay);
    std::cout << "blocking worker " << id << " finished" << std::endl;
  });
}

coro::task<void> run_parallel_blocking() {
  const auto started = std::chrono::steady_clock::now();

  co_await coro::when_all(
      blocking_job(1, std::chrono::milliseconds(80)),
      blocking_job(2, std::chrono::milliseconds(80)),
      blocking_job(3, std::chrono::milliseconds(80)),
      blocking_job(4, std::chrono::milliseconds(80)));

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);

  std::cout << "parallel blocking elapsed: " << elapsed.count() << "ms" << std::endl;
  if (elapsed.count() > 400) {
    throw std::runtime_error("parallel blocking took unexpectedly long");
  }
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime_options options;
    options.thread_pool_size = 4;

    luvcoro::uv_runtime runtime(options);
    luvcoro::run_with_runtime(runtime, run_parallel_blocking());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
