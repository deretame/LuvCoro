#include <chrono>
#include <iostream>
#include <string>

#include <coro/coro.hpp>

#include "luvcoro/message.hpp"
#include "luvcoro/runtime.hpp"

namespace {

coro::task<void> produce_messages(luvcoro::message_channel<std::string>& channel) {
  co_await luvcoro::blocking([&channel]() {
    channel.send("first");
    channel.send("second");
  });
}

coro::task<void> consume_messages(luvcoro::message_channel<std::string>& channel) {
  int count = 0;

  for (auto next_message : channel.messages_for(std::chrono::milliseconds(40))) {
    auto message = co_await next_message;
    if (!message.has_value()) {
      break;
    }

    ++count;
    std::cout << "timed async iterated: " << *message << std::endl;
  }

  if (count != 2) {
    throw std::runtime_error("messages_for timed iteration count mismatch");
  }

  channel.close();
}

coro::task<void> run_message_async_timeout_iteration() {
  luvcoro::message_channel<std::string> channel;
  co_await coro::when_all(produce_messages(channel), consume_messages(channel));
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_message_async_timeout_iteration());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
