#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <coro/coro.hpp>
#include <uv.h>

#include "luvcoro/message.hpp"
#include "luvcoro/runtime.hpp"

namespace luvcoro {

class file {
 public:
  file();
  explicit file(uv_runtime& runtime);
  ~file();

  file(const file&) = delete;
  file& operator=(const file&) = delete;

  file(file&& other) noexcept;
  file& operator=(file&& other) noexcept;

  coro::task<void> open(const std::string& path,
                        int flags = UV_FS_O_RDONLY,
                        int mode = 0,
                        std::stop_token stop_token = {});
  coro::task<void> open(uv_file fd, std::stop_token stop_token = {});
  coro::task<std::string> read(std::size_t max_bytes, std::stop_token stop_token = {});
  coro::task<std::string> read_some(std::size_t max_bytes, std::stop_token stop_token = {});
  coro::task<std::size_t> read_some_into(std::span<char> buffer, std::stop_token stop_token = {});
  coro::task<std::string> read_exact(std::size_t total_bytes, std::stop_token stop_token = {});
  coro::task<std::size_t> read_exact_into(std::span<char> buffer, std::stop_token stop_token = {});
  coro::task<std::size_t> write(std::string_view data, std::stop_token stop_token = {});
  coro::task<std::size_t> write_some(std::span<const char> data, std::stop_token stop_token = {});
  coro::task<std::string> read_at(std::size_t max_bytes,
                                  std::int64_t offset,
                                  std::stop_token stop_token = {});
  coro::task<std::size_t> read_at_into(std::span<char> buffer,
                                       std::int64_t offset,
                                       std::stop_token stop_token = {});
  coro::task<std::size_t> write_at(std::string_view data,
                                   std::int64_t offset,
                                   std::stop_token stop_token = {});
  coro::task<std::size_t> write_some_at(std::span<const char> data,
                                        std::int64_t offset,
                                        std::stop_token stop_token = {});
  coro::task<std::string> read_all(std::stop_token stop_token = {});
  coro::task<void> truncate(std::int64_t size, std::stop_token stop_token = {});
  coro::task<void> close(std::stop_token stop_token = {});

  bool is_open() const noexcept { return fd_ >= 0; }
  auto tell() const noexcept -> std::int64_t { return offset_; }
  void seek(std::int64_t offset) noexcept { offset_ = offset; }
  auto native_handle() const noexcept -> uv_file { return fd_; }
  auto runtime() const noexcept -> uv_runtime* { return runtime_; }

 private:
  void close_sync_noexcept();

  uv_runtime* runtime_;
  uv_file fd_ = -1;
  std::int64_t offset_ = 0;
};

namespace fs {

enum class copy_flags : unsigned int {
  none = 0,
  exclusive = UV_FS_COPYFILE_EXCL,
  reflink = UV_FS_COPYFILE_FICLONE,
  reflink_force = UV_FS_COPYFILE_FICLONE_FORCE,
};

inline auto operator|(copy_flags lhs, copy_flags rhs) noexcept -> copy_flags {
  return static_cast<copy_flags>(
      static_cast<unsigned int>(lhs) | static_cast<unsigned int>(rhs));
}

inline auto operator&=(copy_flags& lhs, copy_flags rhs) noexcept -> copy_flags& {
  lhs = lhs | rhs;
  return lhs;
}

enum class symlink_flags : unsigned int {
  none = 0,
  directory = UV_FS_SYMLINK_DIR,
  junction = UV_FS_SYMLINK_JUNCTION,
};

inline auto operator|(symlink_flags lhs, symlink_flags rhs) noexcept -> symlink_flags {
  return static_cast<symlink_flags>(
      static_cast<unsigned int>(lhs) | static_cast<unsigned int>(rhs));
}

inline auto operator&=(symlink_flags& lhs, symlink_flags rhs) noexcept -> symlink_flags& {
  lhs = lhs | rhs;
  return lhs;
}

struct file_status {
  uv_stat_t stat{};

  auto size() const noexcept -> std::uint64_t {
    return static_cast<std::uint64_t>(stat.st_size);
  }

  auto mode() const noexcept -> int { return stat.st_mode; }
  auto is_directory() const noexcept -> bool { return S_ISDIR(stat.st_mode) != 0; }
  auto is_regular_file() const noexcept -> bool { return S_ISREG(stat.st_mode) != 0; }
  auto is_character_device() const noexcept -> bool { return S_ISCHR(stat.st_mode) != 0; }
  auto is_block_device() const noexcept -> bool { return S_ISBLK(stat.st_mode) != 0; }
  auto is_fifo() const noexcept -> bool { return S_ISFIFO(stat.st_mode) != 0; }

  auto is_socket() const noexcept -> bool {
#ifdef S_ISSOCK
    return S_ISSOCK(stat.st_mode) != 0;
#else
    return false;
#endif
  }

  auto is_symbolic_link() const noexcept -> bool {
#ifdef S_ISLNK
    return S_ISLNK(stat.st_mode) != 0;
#else
    return false;
#endif
  }
};

struct directory_entry {
  std::string name{};
  uv_dirent_type_t type = UV_DIRENT_UNKNOWN;

  auto is_file() const noexcept -> bool { return type == UV_DIRENT_FILE; }
  auto is_directory() const noexcept -> bool { return type == UV_DIRENT_DIR; }
  auto is_link() const noexcept -> bool { return type == UV_DIRENT_LINK; }
};

enum class watch_flags : unsigned int {
  none = 0,
  watch_entry = UV_FS_EVENT_WATCH_ENTRY,
  stat = UV_FS_EVENT_STAT,
  recursive = UV_FS_EVENT_RECURSIVE,
};

inline auto operator|(watch_flags lhs, watch_flags rhs) noexcept -> watch_flags {
  return static_cast<watch_flags>(
      static_cast<unsigned int>(lhs) | static_cast<unsigned int>(rhs));
}

inline auto operator&(watch_flags lhs, watch_flags rhs) noexcept -> watch_flags {
  return static_cast<watch_flags>(
      static_cast<unsigned int>(lhs) & static_cast<unsigned int>(rhs));
}

inline auto operator|=(watch_flags& lhs, watch_flags rhs) noexcept -> watch_flags& {
  lhs = lhs | rhs;
  return lhs;
}

struct event_notification {
  std::string watch_path{};
  std::string filename{};
  unsigned int events = 0;
  int status = 0;

  auto ok() const noexcept -> bool { return status >= 0; }
  auto renamed() const noexcept -> bool { return (events & UV_RENAME) != 0; }
  auto changed() const noexcept -> bool { return (events & UV_CHANGE) != 0; }
};

struct poll_notification {
  std::string watch_path{};
  uv_stat_t previous{};
  uv_stat_t current{};
  int status = 0;

  auto ok() const noexcept -> bool { return status >= 0; }
  auto size_changed() const noexcept -> bool { return previous.st_size != current.st_size; }
  auto modified_time_changed() const noexcept -> bool {
    return previous.st_mtim.tv_sec != current.st_mtim.tv_sec ||
           previous.st_mtim.tv_nsec != current.st_mtim.tv_nsec;
  }
};

class event_watcher {
 public:
  using async_event = typename message_channel<event_notification>::async_message;

  event_watcher();
  explicit event_watcher(uv_runtime& runtime);
  ~event_watcher();

  event_watcher(const event_watcher&) = delete;
  event_watcher& operator=(const event_watcher&) = delete;
  event_watcher(event_watcher&&) = delete;
  event_watcher& operator=(event_watcher&&) = delete;

  coro::task<void> start(const std::string& path,
                         watch_flags flags = watch_flags::none,
                         std::stop_token stop_token = {});
  coro::task<void> stop(std::stop_token stop_token = {});

  auto next() -> coro::task<event_notification>;

  template <typename Rep, typename Period>
  auto next_for(std::chrono::duration<Rep, Period> timeout)
      -> coro::task<std::optional<event_notification>> {
    co_return co_await events_.recv_for(timeout);
  }

  auto next_async() -> async_event { return events_.next(); }

  template <typename Rep, typename Period>
  auto next_async_for(std::chrono::duration<Rep, Period> timeout) -> async_event {
    return events_.next_for(timeout);
  }

  template <typename Rep, typename Period>
  auto next_async_until(std::stop_token stop_token,
                        std::chrono::duration<Rep, Period> poll_interval =
                            std::chrono::milliseconds(50)) -> async_event {
    return events_.next_until(stop_token, poll_interval);
  }

  auto events() -> coro::generator<async_event> { return events_.messages(); }

  template <typename Rep, typename Period>
  auto events_for(std::chrono::duration<Rep, Period> timeout)
      -> coro::generator<async_event> {
    return events_.messages_for(timeout);
  }

  template <typename Rep, typename Period>
  auto events_until(std::stop_token stop_token,
                    std::chrono::duration<Rep, Period> poll_interval =
                        std::chrono::milliseconds(50))
      -> coro::generator<async_event> {
    return events_.messages_until(stop_token, poll_interval);
  }

  auto path() const -> std::string { return path_; }
  auto active() const noexcept -> bool { return active_; }

  void close();

 private:
  struct state;

  uv_runtime* runtime_ = nullptr;
  message_channel<event_notification> events_;
  std::shared_ptr<state> state_{};
  std::string path_{};
  bool active_ = false;
};

class poll_watcher {
 public:
  using async_event = typename message_channel<poll_notification>::async_message;

  poll_watcher();
  explicit poll_watcher(uv_runtime& runtime);
  ~poll_watcher();

  poll_watcher(const poll_watcher&) = delete;
  poll_watcher& operator=(const poll_watcher&) = delete;
  poll_watcher(poll_watcher&&) = delete;
  poll_watcher& operator=(poll_watcher&&) = delete;

  template <typename Rep, typename Period>
  auto start(const std::string& path,
             std::chrono::duration<Rep, Period> interval,
             std::stop_token stop_token = {}) -> coro::task<void> {
    co_await start_impl(path,
                        std::chrono::duration_cast<std::chrono::milliseconds>(interval),
                        stop_token);
  }

  coro::task<void> stop(std::stop_token stop_token = {});

  auto next() -> coro::task<poll_notification>;

  template <typename Rep, typename Period>
  auto next_for(std::chrono::duration<Rep, Period> timeout)
      -> coro::task<std::optional<poll_notification>> {
    co_return co_await events_.recv_for(timeout);
  }

  auto next_async() -> async_event { return events_.next(); }

  template <typename Rep, typename Period>
  auto next_async_for(std::chrono::duration<Rep, Period> timeout) -> async_event {
    return events_.next_for(timeout);
  }

  template <typename Rep, typename Period>
  auto next_async_until(std::stop_token stop_token,
                        std::chrono::duration<Rep, Period> poll_interval =
                            std::chrono::milliseconds(50)) -> async_event {
    return events_.next_until(stop_token, poll_interval);
  }

  auto events() -> coro::generator<async_event> { return events_.messages(); }

  template <typename Rep, typename Period>
  auto events_for(std::chrono::duration<Rep, Period> timeout)
      -> coro::generator<async_event> {
    return events_.messages_for(timeout);
  }

  template <typename Rep, typename Period>
  auto events_until(std::stop_token stop_token,
                    std::chrono::duration<Rep, Period> poll_interval =
                        std::chrono::milliseconds(50))
      -> coro::generator<async_event> {
    return events_.messages_until(stop_token, poll_interval);
  }

  auto path() const -> std::string { return path_; }
  auto interval() const noexcept -> std::chrono::milliseconds { return interval_; }
  auto active() const noexcept -> bool { return active_; }

  void close();

 private:
  struct state;

  coro::task<void> start_impl(const std::string& path,
                              std::chrono::milliseconds interval,
                              std::stop_token stop_token);

  uv_runtime* runtime_ = nullptr;
  message_channel<poll_notification> events_;
  std::shared_ptr<state> state_{};
  std::string path_{};
  std::chrono::milliseconds interval_{0};
  bool active_ = false;
};

coro::task<file_status> stat(const std::string& path, std::stop_token stop_token = {});
coro::task<file_status> lstat(const std::string& path, std::stop_token stop_token = {});
coro::task<bool> exists(const std::string& path, std::stop_token stop_token = {});
coro::task<bool> is_directory(const std::string& path, std::stop_token stop_token = {});
coro::task<std::uint64_t> file_size(const std::string& path, std::stop_token stop_token = {});
coro::task<std::vector<directory_entry>> list_directory(const std::string& path,
                                                        std::stop_token stop_token = {});

coro::task<void> create_directory(const std::string& path,
                                  int mode = 0755,
                                  std::stop_token stop_token = {});
coro::task<void> create_directories(const std::string& path,
                                    int mode = 0755,
                                    std::stop_token stop_token = {});
coro::task<void> rename(const std::string& path,
                        const std::string& new_path,
                        std::stop_token stop_token = {});
coro::task<void> copy_file(const std::string& path,
                           const std::string& new_path,
                           copy_flags flags = copy_flags::none,
                           std::stop_token stop_token = {});
coro::task<void> create_hard_link(const std::string& path,
                                  const std::string& new_path,
                                  std::stop_token stop_token = {});
coro::task<void> create_symbolic_link(const std::string& path,
                                      const std::string& new_path,
                                      symlink_flags flags = symlink_flags::none,
                                      std::stop_token stop_token = {});
coro::task<std::string> read_link(const std::string& path, std::stop_token stop_token = {});
coro::task<std::string> real_path(const std::string& path, std::stop_token stop_token = {});
coro::task<void> chmod(const std::string& path,
                       int mode,
                       std::stop_token stop_token = {});
coro::task<void> remove_file(const std::string& path, std::stop_token stop_token = {});
coro::task<void> remove_directory(const std::string& path, std::stop_token stop_token = {});
coro::task<void> remove_all(const std::string& path, std::stop_token stop_token = {});

coro::task<std::string> read_file(const std::string& path, std::stop_token stop_token = {});
coro::task<void> write_file(
    const std::string& path,
    std::string_view data,
    int mode = 0644,
    std::stop_token stop_token = {});
coro::task<void> append_file(
    const std::string& path,
    std::string_view data,
    int mode = 0644,
    std::stop_token stop_token = {});
coro::task<std::size_t> sendfile(file& target,
                                 file& source,
                                 std::size_t max_bytes,
                                 std::stop_token stop_token = {});
coro::task<std::size_t> sendfile(file& target,
                                 file& source,
                                 std::int64_t source_offset,
                                 std::size_t max_bytes,
                                 std::stop_token stop_token = {});

}  // namespace fs

}  // namespace luvcoro
