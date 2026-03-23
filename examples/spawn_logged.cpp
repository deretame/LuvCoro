#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

#include <coro/coro.hpp>

#include "luvcoro/message.hpp"
#include "luvcoro/runtime.hpp"

namespace {

void launch_logged_failure(luvcoro::message_channel<std::string>& channel) {
  const bool started = luvcoro::spawn_logged(
      "background failure",
      []() -> coro::task<void> {
        co_await luvcoro::sleep_for(std::chrono::milliseconds(20));
        throw std::runtime_error("boom");
      },
      [&channel](std::string_view name, std::exception_ptr exception) noexcept {
        try {
          if (exception != nullptr) {
            std::rethrow_exception(exception);
          }
          channel.send(std::string(name) + ": unknown exception");
        } catch (const std::exception& ex) {
          channel.send(std::string(name) + ": " + ex.what());
        } catch (...) {
          channel.send(std::string(name) + ": non-standard exception");
        }
        channel.close();
      });

  if (!started) {
    throw std::runtime_error("spawn_logged failed to schedule detached coroutine");
  }
}

coro::task<void> run_spawn_logged() {
  luvcoro::message_channel<std::string> channel;

  launch_logged_failure(channel);

  auto message = co_await channel.recv();
  if (message != "background failure: boom") {
    throw std::runtime_error("spawn_logged produced unexpected log message");
  }

  std::cout << "spawn logged ok: " << message << std::endl;
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_spawn_logged());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
