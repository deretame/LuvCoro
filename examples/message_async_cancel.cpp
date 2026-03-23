#include <chrono>
#include <iostream>
#include <stop_token>
#include <string>

#include <coro/coro.hpp>

#include "luvcoro/message.hpp"
#include "luvcoro/runtime.hpp"

namespace {

coro::task<void> produce_messages(luvcoro::message_channel<std::string>& channel) {
  co_await channel.async_send("alpha");
  co_await luvcoro::sleep_for(std::chrono::milliseconds(10));
  co_await channel.async_send("beta");
  co_await luvcoro::sleep_for(std::chrono::milliseconds(50));
  channel.close();
}

coro::task<void> request_stop(std::stop_source& stop_source) {
  co_await luvcoro::sleep_for(std::chrono::milliseconds(25));
  stop_source.request_stop();
}

coro::task<void> consume_messages(luvcoro::message_channel<std::string>& channel,
                                  std::stop_source& stop_source) {
  int count = 0;

  for (auto next_message :
       channel.messages_until(stop_source.get_token(), std::chrono::milliseconds(10))) {
    auto message = co_await next_message;
    if (!message.has_value()) {
      break;
    }

    ++count;
    std::cout << "cancel-aware async iterated: " << *message << std::endl;
  }

  if (count != 2) {
    throw std::runtime_error("messages_until consumed unexpected number of messages");
  }
}

coro::task<void> run_message_async_cancel() {
  luvcoro::message_channel<std::string> channel;
  std::stop_source stop_source;

  co_await coro::when_all(
      produce_messages(channel),
      consume_messages(channel, stop_source),
      request_stop(stop_source));
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_message_async_cancel());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
