#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <coro/coro.hpp>

#include "luvcoro/message.hpp"
#include "luvcoro/runtime.hpp"

namespace {

coro::task<void> send_message(luvcoro::message_channel<std::string>& channel,
                              std::string message) {
  co_await channel.async_send(std::move(message));
}

coro::task<void> fail_delayed_task() {
  throw std::runtime_error("timer boom");
  co_return;
}

coro::task<void> run_spawn_timer() {
  using namespace std::chrono_literals;

  luvcoro::message_channel<std::string> channel;

  const bool started_after =
      luvcoro::spawn_after(25ms, send_message(channel, "after"));

  const bool started_at =
      luvcoro::spawn_at(std::chrono::steady_clock::now() + 50ms,
                        send_message(channel, "at"));

  const bool started_logged = luvcoro::spawn_logged_after(
      "delayed failure",
      10ms,
      fail_delayed_task(),
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
      });

  if (!started_after || !started_at || !started_logged) {
    throw std::runtime_error("failed to schedule timed detached coroutines");
  }

  std::vector<std::string> messages;
  messages.reserve(3);
  messages.push_back(co_await channel.recv());
  messages.push_back(co_await channel.recv());
  messages.push_back(co_await channel.recv());

  const std::vector<std::string> expected = {
      "after",
      "at",
      "delayed failure: timer boom",
  };

  std::sort(messages.begin(), messages.end());
  if (messages != expected) {
    throw std::runtime_error("timed spawn produced unexpected messages");
  }

  std::cout << "spawn timer ok" << std::endl;
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_spawn_timer());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
