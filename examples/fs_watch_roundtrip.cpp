#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include <coro/coro.hpp>

#include "luvcoro/fs.hpp"
#include "luvcoro/runtime.hpp"

using namespace std::chrono_literals;

namespace {

auto watch_root() -> std::filesystem::path {
  return std::filesystem::temp_directory_path() / "luvcoro_fs_watch_roundtrip";
}

coro::task<void> trigger_changes(const std::string& watch_dir,
                                 const std::string& watch_file) {
  co_await luvcoro::sleep_for(100ms);
  co_await luvcoro::fs::append_file(watch_file, " world");
  co_await luvcoro::fs::write_file(
      (std::filesystem::path(watch_dir) / "created.txt").string(),
      "created");
}

coro::task<void> wait_for_event(luvcoro::fs::event_watcher& watcher) {
  auto notification = co_await watcher.next_for(3s);
  if (!notification.has_value()) {
    throw std::runtime_error("fs event watcher timed out");
  }
  if (!notification->ok()) {
    throw luvcoro::uv_error(notification->status, "fs event watcher callback failed");
  }
  if (!notification->renamed() && !notification->changed()) {
    throw std::runtime_error("fs event watcher reported no recognized event flags");
  }
}

coro::task<void> wait_for_poll(luvcoro::fs::poll_watcher& watcher) {
  while (true) {
    auto notification = co_await watcher.next_for(3s);
    if (!notification.has_value()) {
      throw std::runtime_error("fs poll watcher timed out");
    }
    if (!notification->ok()) {
      throw luvcoro::uv_error(notification->status, "fs poll watcher callback failed");
    }
    if (notification->size_changed() || notification->modified_time_changed()) {
      co_return;
    }
  }
}

coro::task<void> run_fs_watch_roundtrip() {
  const auto root = watch_root();
  const auto watched_file = root / "watched.txt";

  if (std::filesystem::exists(root)) {
    co_await luvcoro::fs::remove_all(root.string());
  }
  co_await luvcoro::fs::create_directories(root.string());
  co_await luvcoro::fs::write_file(watched_file.string(), "hello");

  luvcoro::fs::event_watcher event_watcher;
  co_await event_watcher.start(root.string());

  luvcoro::fs::poll_watcher poll_watcher;
  co_await poll_watcher.start(watched_file.string(), 50ms);

  if (!event_watcher.active() || !poll_watcher.active()) {
    throw std::runtime_error("fs watchers did not start");
  }

  co_await coro::when_all(
      trigger_changes(root.string(), watched_file.string()),
      wait_for_event(event_watcher),
      wait_for_poll(poll_watcher));

  co_await event_watcher.stop();
  co_await poll_watcher.stop();

  event_watcher.close();
  poll_watcher.close();

  co_await luvcoro::fs::remove_all(root.string());
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_fs_watch_roundtrip());
    std::cout << "fs watch roundtrip ok" << std::endl;
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
