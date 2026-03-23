#include <iostream>
#include <string>

#include <coro/coro.hpp>

#include "luvcoro/message.hpp"
#include "luvcoro/runtime.hpp"

namespace {

coro::task<void> produce_messages(luvcoro::message_channel<std::string>& channel) {
  co_await luvcoro::blocking([&channel]() {
    channel.send("alpha");
    channel.send("beta");
    channel.send("gamma");
    channel.close();
  });
}

coro::task<void> consume_messages(luvcoro::message_channel<std::string>& channel) {
  for (auto next_message : channel.messages()) {
    auto message = co_await next_message;
    if (!message.has_value()) {
      break;
    }

    std::cout << "async iterated: " << *message << std::endl;
  }
}

coro::task<void> run_message_async_iteration() {
  luvcoro::message_channel<std::string> channel;
  co_await coro::when_all(produce_messages(channel), consume_messages(channel));
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_message_async_iteration());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
