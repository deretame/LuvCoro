#include <chrono>
#include <iostream>
#include <string>
#include <variant>

#include <coro/coro.hpp>

#include "luvcoro/message.hpp"
#include "luvcoro/runtime.hpp"

namespace {

coro::task<void> produce_numbers(luvcoro::message_channel<int>& channel) {
  co_await luvcoro::sleep_for(std::chrono::milliseconds(30));
  co_await channel.async_send(42);
  channel.close();
}

coro::task<void> produce_words(luvcoro::message_channel<std::string>& channel) {
  co_await luvcoro::sleep_for(std::chrono::milliseconds(10));
  co_await channel.async_send("fast");
  channel.close();
}

coro::task<void> consume_first(luvcoro::message_channel<int>& numbers,
                               luvcoro::message_channel<std::string>& words) {
  auto result = co_await luvcoro::select(numbers, words);

  std::visit(
      [](const auto& selected) {
        using selected_t = std::decay_t<decltype(selected)>;

        if constexpr (std::is_same_v<selected_t, luvcoro::selected_message<int>>) {
          if (!selected.message.has_value() || *selected.message != 42) {
            throw std::runtime_error("select returned unexpected int payload");
          }
          std::cout << "select winner: channel[" << selected.index
                    << "] int=" << *selected.message << std::endl;
        } else {
          if (!selected.message.has_value() || *selected.message != "fast") {
            throw std::runtime_error("select returned unexpected string payload");
          }
          std::cout << "select winner: channel[" << selected.index
                    << "] string=" << *selected.message << std::endl;
        }
      },
      result);
}

coro::task<void> run_message_select() {
  luvcoro::message_channel<int> numbers;
  luvcoro::message_channel<std::string> words;

  co_await coro::when_all(
      consume_first(numbers, words),
      produce_numbers(numbers),
      produce_words(words));
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_message_select());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
