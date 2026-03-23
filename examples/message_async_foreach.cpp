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
  co_await luvcoro::co_foreach(
      channel.messages(),
      [](std::string message) -> coro::task<void> {
        std::cout << "async foreach: " << message << std::endl;
        co_return;
      });
}

coro::task<void> run_message_async_foreach() {
  luvcoro::message_channel<std::string> channel;
  co_await coro::when_all(produce_messages(channel), consume_messages(channel));
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_message_async_foreach());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
