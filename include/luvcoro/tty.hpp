#pragma once

#include <cstddef>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

#include <coro/coro.hpp>
#include <uv.h>

#include "luvcoro/runtime.hpp"

namespace luvcoro {

enum class tty_mode {
  normal = UV_TTY_MODE_NORMAL,
  raw = UV_TTY_MODE_RAW,
  io = UV_TTY_MODE_IO,
  raw_vt = UV_TTY_MODE_RAW_VT,
};

struct tty_winsize {
  int width = 0;
  int height = 0;
};

auto guess_handle(uv_file fd) noexcept -> uv_handle_type;
auto is_tty(uv_file fd) noexcept -> bool;

class tty_stream {
 public:
  tty_stream();
  explicit tty_stream(uv_runtime& runtime);
  ~tty_stream();

  tty_stream(const tty_stream&) = delete;
  tty_stream& operator=(const tty_stream&) = delete;

  tty_stream(tty_stream&& other) noexcept;
  tty_stream& operator=(tty_stream&& other) noexcept;

  coro::task<void> open(uv_file fd, bool readable, std::stop_token stop_token = {});
  coro::task<void> open_stdin(std::stop_token stop_token = {});
  coro::task<void> open_stdout(std::stop_token stop_token = {});
  coro::task<void> open_stderr(std::stop_token stop_token = {});

  coro::task<std::size_t> write(std::string_view data, std::stop_token stop_token = {});
  coro::task<std::size_t> write_some(std::span<const char> data, std::stop_token stop_token = {});
  coro::task<std::string> read_some(std::size_t max_bytes = 4096, std::stop_token stop_token = {});
  coro::task<std::size_t> read_some_into(std::span<char> buffer, std::stop_token stop_token = {});
  coro::task<std::string> read_exact(std::size_t total_bytes, std::stop_token stop_token = {});
  coro::task<std::size_t> read_exact_into(std::span<char> buffer, std::stop_token stop_token = {});

  coro::task<void> set_mode(tty_mode mode, std::stop_token stop_token = {});
  coro::task<void> close(std::stop_token stop_token = {});

  auto winsize() const -> tty_winsize;
  auto fd() const noexcept -> uv_file { return fd_; }
  auto readable() const noexcept -> bool { return readable_; }
  auto writable() const noexcept -> bool { return is_open() && !readable_; }
  auto is_open() const noexcept -> bool { return handle_ != nullptr; }

 private:
  void close_sync_noexcept();

  uv_runtime* runtime_ = nullptr;
  uv_tty_t* handle_ = nullptr;
  uv_file fd_ = -1;
  bool readable_ = false;
};

coro::task<void> reset_tty_mode(uv_runtime& runtime, std::stop_token stop_token = {});
inline coro::task<void> reset_tty_mode(std::stop_token stop_token = {}) {
  co_await reset_tty_mode(current_runtime(), stop_token);
}

}  // namespace luvcoro
