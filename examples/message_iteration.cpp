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

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::message_channel<std::string> channel(runtime);

    luvcoro::run_with_runtime(runtime, produce_messages(channel));

    for (const auto& message : channel) {
      std::cout << "iterated: " << message << std::endl;
    }

    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
