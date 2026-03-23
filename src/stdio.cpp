#include "luvcoro/stdio.hpp"

#include <stdexcept>
#include <utility>

#include "luvcoro/error.hpp"

namespace luvcoro {

stdio_stream::stdio_stream() : stdio_stream(current_runtime()) {}

stdio_stream::stdio_stream(uv_runtime& runtime) : runtime_(&runtime) {}

stdio_stream::stdio_stream(stdio_stream&& other) noexcept
    : runtime_(other.runtime_),
      kind_(other.kind_),
      guessed_handle_(other.guessed_handle_),
      readable_(other.readable_),
      tty_(std::move(other.tty_)),
      pipe_(std::move(other.pipe_)),
      file_(std::move(other.file_)) {
  other.reset_state();
}

auto stdio_stream::operator=(stdio_stream&& other) noexcept -> stdio_stream& {
  if (this == &other) {
    return *this;
  }

  runtime_ = other.runtime_;
  kind_ = other.kind_;
  guessed_handle_ = other.guessed_handle_;
  readable_ = other.readable_;
  tty_ = std::move(other.tty_);
  pipe_ = std::move(other.pipe_);
  file_ = std::move(other.file_);

  other.reset_state();
  return *this;
}

void stdio_stream::reset_state() noexcept {
  kind_ = stdio_kind::none;
  guessed_handle_ = UV_UNKNOWN_HANDLE;
  readable_ = false;
}

coro::task<void> stdio_stream::open(uv_file fd,
                                    bool readable,
                                    std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  if (is_open()) {
    co_await close(stop_token);
  }

  const auto handle_type = guess_handle(fd);
  guessed_handle_ = handle_type;
  readable_ = readable;

  switch (handle_type) {
    case UV_TTY:
      tty_.emplace(*runtime_);
      co_await tty_->open(fd, readable, stop_token);
      kind_ = stdio_kind::tty;
      break;
    case UV_NAMED_PIPE:
      pipe_.emplace(*runtime_);
      co_await pipe_->open(fd, readable, stop_token);
      kind_ = stdio_kind::pipe;
      break;
    case UV_FILE:
      file_.emplace(*runtime_);
      co_await file_->open(fd, stop_token);
      kind_ = stdio_kind::file;
      break;
    default:
      reset_state();
      throw std::runtime_error("unsupported stdio handle type");
  }
}

coro::task<void> stdio_stream::open_stdin(std::stop_token stop_token) {
  co_await open(0, true, stop_token);
}

coro::task<void> stdio_stream::open_stdout(std::stop_token stop_token) {
  co_await open(1, false, stop_token);
}

coro::task<void> stdio_stream::open_stderr(std::stop_token stop_token) {
  co_await open(2, false, stop_token);
}

coro::task<std::size_t> stdio_stream::write(std::string_view data, std::stop_token stop_token) {
  if (!is_open()) {
    throw std::runtime_error("stdio stream is not open");
  }
  if (readable_) {
    throw std::runtime_error("stdio stream was opened as readable");
  }

  switch (kind_) {
    case stdio_kind::tty:
      co_return co_await tty_->write(data, stop_token);
    case stdio_kind::pipe:
      co_return co_await pipe_->write(data, stop_token);
    case stdio_kind::file:
      co_return co_await file_->write(data, stop_token);
    case stdio_kind::none:
      break;
  }

  throw std::runtime_error("stdio stream backend is not available");
}

coro::task<std::size_t> stdio_stream::write_some(std::span<const char> data,
                                                 std::stop_token stop_token) {
  if (!is_open()) {
    throw std::runtime_error("stdio stream is not open");
  }
  if (readable_) {
    throw std::runtime_error("stdio stream was opened as readable");
  }

  switch (kind_) {
    case stdio_kind::tty:
      co_return co_await tty_->write_some(data, stop_token);
    case stdio_kind::pipe:
      co_return co_await pipe_->write_some(data, stop_token);
    case stdio_kind::file:
      co_return co_await file_->write_some(data, stop_token);
    case stdio_kind::none:
      break;
  }

  throw std::runtime_error("stdio stream backend is not available");
}

coro::task<std::string> stdio_stream::read_some(std::size_t max_bytes,
                                                std::stop_token stop_token) {
  if (!is_open()) {
    throw std::runtime_error("stdio stream is not open");
  }
  if (!readable_) {
    throw std::runtime_error("stdio stream was opened as writable");
  }

  switch (kind_) {
    case stdio_kind::tty:
      co_return co_await tty_->read_some(max_bytes, stop_token);
    case stdio_kind::pipe:
      co_return co_await pipe_->read_some(max_bytes, stop_token);
    case stdio_kind::file:
      co_return co_await file_->read_some(max_bytes, stop_token);
    case stdio_kind::none:
      break;
  }

  throw std::runtime_error("stdio stream backend is not available");
}

coro::task<std::size_t> stdio_stream::read_some_into(std::span<char> buffer,
                                                     std::stop_token stop_token) {
  if (!is_open()) {
    throw std::runtime_error("stdio stream is not open");
  }
  if (!readable_) {
    throw std::runtime_error("stdio stream was opened as writable");
  }

  switch (kind_) {
    case stdio_kind::tty:
      co_return co_await tty_->read_some_into(buffer, stop_token);
    case stdio_kind::pipe:
      co_return co_await pipe_->read_some_into(buffer, stop_token);
    case stdio_kind::file:
      co_return co_await file_->read_some_into(buffer, stop_token);
    case stdio_kind::none:
      break;
  }

  throw std::runtime_error("stdio stream backend is not available");
}

coro::task<std::string> stdio_stream::read_exact(std::size_t total_bytes,
                                                 std::stop_token stop_token) {
  if (!is_open()) {
    throw std::runtime_error("stdio stream is not open");
  }
  if (!readable_) {
    throw std::runtime_error("stdio stream was opened as writable");
  }

  switch (kind_) {
    case stdio_kind::tty:
      co_return co_await tty_->read_exact(total_bytes, stop_token);
    case stdio_kind::pipe:
      co_return co_await pipe_->read_exact(total_bytes, stop_token);
    case stdio_kind::file:
      co_return co_await file_->read_exact(total_bytes, stop_token);
    case stdio_kind::none:
      break;
  }

  throw std::runtime_error("stdio stream backend is not available");
}

coro::task<std::size_t> stdio_stream::read_exact_into(std::span<char> buffer,
                                                      std::stop_token stop_token) {
  if (!is_open()) {
    throw std::runtime_error("stdio stream is not open");
  }
  if (!readable_) {
    throw std::runtime_error("stdio stream was opened as writable");
  }

  switch (kind_) {
    case stdio_kind::tty:
      co_return co_await tty_->read_exact_into(buffer, stop_token);
    case stdio_kind::pipe:
      co_return co_await pipe_->read_exact_into(buffer, stop_token);
    case stdio_kind::file:
      co_return co_await file_->read_exact_into(buffer, stop_token);
    case stdio_kind::none:
      break;
  }

  throw std::runtime_error("stdio stream backend is not available");
}

coro::task<void> stdio_stream::close(std::stop_token stop_token) {
  switch (kind_) {
    case stdio_kind::tty:
      if (tty_) {
        co_await tty_->close(stop_token);
        tty_.reset();
      }
      break;
    case stdio_kind::pipe:
      if (pipe_) {
        pipe_->close();
        pipe_.reset();
      }
      break;
    case stdio_kind::file:
      if (file_) {
        co_await file_->close(stop_token);
        file_.reset();
      }
      break;
    case stdio_kind::none:
      break;
  }

  reset_state();
}

auto to_string(stdio_kind kind) noexcept -> const char* {
  switch (kind) {
    case stdio_kind::none:
      return "none";
    case stdio_kind::tty:
      return "tty";
    case stdio_kind::pipe:
      return "pipe";
    case stdio_kind::file:
      return "file";
  }

  return "unknown";
}

}  // namespace luvcoro
