#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

#include <coro/coro.hpp>

#include "luvcoro/message.hpp"
#include "luvcoro/runtime.hpp"

namespace {

coro::task<void> background_sender_task(
    luvcoro::message_channel<std::string>& channel) {
  co_await luvcoro::sleep_for(std::chrono::milliseconds(20));
  co_await channel.async_send("detached hello");
  channel.close();
}

void launch_background_sender(luvcoro::message_channel<std::string>& channel) {
  const bool started = luvcoro::spawn(background_sender_task(channel));

  if (!started) {
    throw std::runtime_error("spawn failed to schedule detached coroutine");
  }
}

coro::task<void> run_spawn_detached() {
  luvcoro::message_channel<std::string> channel;

  launch_background_sender(channel);

  auto message = co_await channel.recv();
  if (message != "detached hello") {
    throw std::runtime_error("spawn detached produced unexpected message");
  }

  std::cout << "spawn detached ok: " << message << std::endl;
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_spawn_detached());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
