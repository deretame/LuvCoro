#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

#include <coro/coro.hpp>

#include "luvcoro/message.hpp"
#include "luvcoro/runtime.hpp"

namespace {

coro::task<void> run_message_bounded() {
  luvcoro::message_channel<std::string> channel(1);

  co_await channel.async_send("first");
  if (channel.try_send("overflow")) {
    throw std::runtime_error("try_send should fail when the bounded queue is full");
  }

  co_await coro::when_all(
      [&]() -> coro::task<void> {
        co_await luvcoro::sleep_for(std::chrono::milliseconds(20));
        auto first = co_await channel.recv_for(std::chrono::milliseconds(200));
        if (!first.has_value() || *first != "first") {
          throw std::runtime_error("bounded channel failed to receive the first message");
        }
      }(),
      [&]() -> coro::task<void> {
        co_await channel.async_send("second");
      }());

  auto second = co_await channel.recv_for(std::chrono::milliseconds(200));
  if (!second.has_value() || *second != "second") {
    throw std::runtime_error("bounded channel failed to deliver the backpressured message");
  }

  auto timeout = co_await channel.recv_for(std::chrono::milliseconds(20));
  if (timeout.has_value()) {
    throw std::runtime_error("recv_for should time out on an empty channel");
  }

  channel.close();
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_message_bounded());
    std::cout << "message bounded ok" << std::endl;
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
