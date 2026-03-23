#include <chrono>
#include <csignal>
#include <iostream>
#include <optional>
#include <stdexcept>

#include <coro/coro.hpp>

#include "luvcoro/runtime.hpp"
#include "luvcoro/signal.hpp"
#include <uv.h>

using namespace std::chrono_literals;

namespace {

constexpr int watched_signal =
#ifdef _WIN32
    SIGBREAK;
#else
    SIGINT;
#endif

coro::task<void> raise_signal_later() {
  co_await luvcoro::sleep_for(100ms);
#ifdef _WIN32
  if (std::raise(SIGBREAK) != 0) {
    throw std::runtime_error("failed to raise SIGBREAK");
  }
#else
  if (std::raise(SIGINT) != 0) {
    throw std::runtime_error("failed to raise SIGINT");
  }
#endif
}

coro::task<void> wait_until_signal_registered(luvcoro::signal_set& signals) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (signals.contains(watched_signal)) {
      co_return;
    }
    co_await luvcoro::sleep_for(10ms);
  }

  throw std::runtime_error("signal watcher did not become active in time");
}

coro::task<void> run_signal_roundtrip() {
  luvcoro::signal_set signals;
  co_await signals.add(watched_signal);
  co_await wait_until_signal_registered(signals);

  auto wait_for_signal = [&signals]() -> coro::task<void> {
    auto notification = co_await signals.next_for(2s);
    if (!notification.has_value()) {
      throw std::runtime_error("signal wait timed out");
    }
    if (notification->signum != watched_signal) {
      throw std::runtime_error("unexpected signal received");
    }
  };

  co_await coro::when_all(raise_signal_later(), wait_for_signal());
  co_await signals.remove(watched_signal);
  signals.close();
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_signal_roundtrip());
    std::cout << "signal roundtrip ok" << std::endl;
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
