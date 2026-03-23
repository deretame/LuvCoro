#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

#include <coro/coro.hpp>
#include <uv.h>

#include "luvcoro/fs.hpp"
#include "luvcoro/pipe.hpp"
#include "luvcoro/runtime.hpp"
#include "luvcoro/tty.hpp"

namespace luvcoro {

enum class stdio_kind {
  none,
  tty,
  pipe,
  file,
};

class stdio_stream {
 public:
  stdio_stream();
  explicit stdio_stream(uv_runtime& runtime);
  ~stdio_stream() = default;

  stdio_stream(const stdio_stream&) = delete;
  stdio_stream& operator=(const stdio_stream&) = delete;

  stdio_stream(stdio_stream&& other) noexcept;
  stdio_stream& operator=(stdio_stream&& other) noexcept;

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
  coro::task<void> close(std::stop_token stop_token = {});

  auto kind() const noexcept -> stdio_kind { return kind_; }
  auto guessed_handle() const noexcept -> uv_handle_type { return guessed_handle_; }
  auto readable() const noexcept -> bool { return readable_; }
  auto writable() const noexcept -> bool { return is_open() && !readable_; }
  auto is_open() const noexcept -> bool { return kind_ != stdio_kind::none; }
  auto tty() noexcept -> tty_stream* { return tty_ ? &*tty_ : nullptr; }
  auto pipe() noexcept -> pipe_client* { return pipe_ ? &*pipe_ : nullptr; }
  auto file_handle() noexcept -> file* { return file_ ? &*file_ : nullptr; }

 private:
  void reset_state() noexcept;

  uv_runtime* runtime_ = nullptr;
  stdio_kind kind_ = stdio_kind::none;
  uv_handle_type guessed_handle_ = UV_UNKNOWN_HANDLE;
  bool readable_ = false;
  std::optional<tty_stream> tty_{};
  std::optional<pipe_client> pipe_{};
  std::optional<file> file_{};
};

auto to_string(stdio_kind kind) noexcept -> const char*;

}  // namespace luvcoro
