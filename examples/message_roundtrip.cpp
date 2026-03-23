#include <iostream>
#include <stdexcept>
#include <string>

#include <coro/coro.hpp>

#include "luvcoro/message.hpp"
#include "luvcoro/runtime.hpp"

namespace {

coro::task<void> sender_task(luvcoro::message_channel<std::string>& channel) {
  co_await luvcoro::blocking([&channel]() {
    channel.send("hello from libuv worker");
  });
}

coro::task<void> receiver_task(luvcoro::message_channel<std::string>& channel) {
  auto message = co_await channel.recv();
  if (message != "hello from libuv worker") {
    throw std::runtime_error("message payload mismatch");
  }
}

coro::task<void> run_message_roundtrip() {
  luvcoro::message_channel<std::string> channel;
  co_await coro::when_all(receiver_task(channel), sender_task(channel));
  channel.close();
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime_options options;
    options.thread_pool_size = 4;

    luvcoro::uv_runtime runtime(options);
    luvcoro::run_with_runtime(runtime, run_message_roundtrip());
    std::cout << "message roundtrip ok" << std::endl;
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
