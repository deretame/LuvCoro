#include <chrono>
#include <iostream>
#include <stdexcept>

#include <coro/coro.hpp>

#include "luvcoro/runtime.hpp"
#include "luvcoro/sync.hpp"

using namespace std::chrono_literals;

namespace {

struct shared_state {
  luvcoro::async_mutex mutex;
  luvcoro::async_condition_variable ready_cv;
  luvcoro::async_semaphore done{0};
  bool ready = false;
  int value = 0;
};

coro::task<void> produce(shared_state& state) {
  co_await luvcoro::sleep_for(50ms);
  auto lock = co_await state.mutex.scoped_lock();
  state.value = 42;
  state.ready = true;
  lock.unlock();
  state.ready_cv.notify_one();
}

coro::task<void> consume(shared_state& state) {
  auto lock = co_await state.mutex.scoped_lock();
  co_await state.ready_cv.wait(
      state.mutex,
      [&state]() { return state.ready; });

  if (state.value != 42) {
    throw std::runtime_error("unexpected shared state value");
  }

  lock.unlock();
  state.done.release();
}

coro::task<void> run_sync_roundtrip() {
  shared_state state{};
  co_await coro::when_all(
      produce(state),
      consume(state),
      [&state]() -> coro::task<void> {
        co_await state.done.acquire();
      }());
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_sync_roundtrip());
    std::cout << "sync roundtrip ok" << std::endl;
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
