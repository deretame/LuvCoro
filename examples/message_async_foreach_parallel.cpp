#include <chrono>
#include <iostream>
#include <thread>

#include <coro/coro.hpp>

#include "luvcoro/message.hpp"
#include "luvcoro/runtime.hpp"

namespace {

coro::task<void> produce_messages(luvcoro::message_channel<int>& channel) {
  for (int i = 1; i <= 4; ++i) {
    co_await channel.async_send(i);
  }
  channel.close();
}

coro::task<void> consume_messages(luvcoro::message_channel<int>& channel) {
  const auto started = std::chrono::steady_clock::now();

  co_await luvcoro::co_foreach(
      channel.messages(), 4,
      [](int value) -> coro::task<void> {
        co_await luvcoro::blocking([value]() {
          std::this_thread::sleep_for(std::chrono::milliseconds(40));
          std::cout << "parallel foreach item: " << value << std::endl;
        });
      });

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);

  std::cout << "parallel foreach elapsed: " << elapsed.count() << "ms" << std::endl;
  if (elapsed.count() > 120) {
    throw std::runtime_error("parallel co_foreach took unexpectedly long");
  }
}

coro::task<void> run_message_async_foreach_parallel() {
  luvcoro::message_channel<int> channel;
  co_await coro::when_all(produce_messages(channel), consume_messages(channel));
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime_options options;
    options.thread_pool_size = 4;

    luvcoro::uv_runtime runtime(options);
    luvcoro::run_with_runtime(runtime, run_message_async_foreach_parallel());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
