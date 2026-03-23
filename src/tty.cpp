#include "luvcoro/tty.hpp"

#include <cerrno>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "luvcoro/error.hpp"
#include "stream_ops.hpp"

namespace luvcoro {

namespace {

#ifdef _WIN32
auto duplicate_file_descriptor(uv_file fd) -> uv_file {
  const int dup_fd = _dup(fd);
  if (dup_fd < 0) {
    throw_uv_error(uv_translate_sys_error(errno), "_dup");
  }
  return dup_fd;
}

void close_file_descriptor_noexcept(uv_file fd) noexcept {
  if (fd >= 0) {
    _close(fd);
  }
}
#else
auto duplicate_file_descriptor(uv_file fd) -> uv_file {
  const int dup_fd = dup(fd);
  if (dup_fd < 0) {
    throw_uv_error(uv_translate_sys_error(errno), "dup");
  }
  return dup_fd;
}

void close_file_descriptor_noexcept(uv_file fd) noexcept {
  if (fd >= 0) {
    close(fd);
  }
}
#endif

}  // namespace

auto guess_handle(uv_file fd) noexcept -> uv_handle_type {
  return uv_guess_handle(fd);
}

auto is_tty(uv_file fd) noexcept -> bool {
  return guess_handle(fd) == UV_TTY;
}

tty_stream::tty_stream() : tty_stream(current_runtime()) {}

tty_stream::tty_stream(uv_runtime& runtime) : runtime_(&runtime) {}

tty_stream::~tty_stream() { close_sync_noexcept(); }

tty_stream::tty_stream(tty_stream&& other) noexcept
    : runtime_(other.runtime_),
      handle_(other.handle_),
      fd_(other.fd_),
      readable_(other.readable_) {
  other.runtime_ = nullptr;
  other.handle_ = nullptr;
  other.fd_ = -1;
  other.readable_ = false;
}

tty_stream& tty_stream::operator=(tty_stream&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  close_sync_noexcept();

  runtime_ = other.runtime_;
  handle_ = other.handle_;
  fd_ = other.fd_;
  readable_ = other.readable_;

  other.runtime_ = nullptr;
  other.handle_ = nullptr;
  other.fd_ = -1;
  other.readable_ = false;
  return *this;
}

void tty_stream::close_sync_noexcept() {
  if (runtime_ == nullptr || handle_ == nullptr) {
    return;
  }

  uv_tty_t* handle = handle_;
  handle_ = nullptr;
  fd_ = -1;
  readable_ = false;

  try {
    runtime_->call([handle](uv_loop_t*) {
      if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(handle))) {
        uv_close(
            reinterpret_cast<uv_handle_t*>(handle),
            [](uv_handle_t* raw_handle) { delete reinterpret_cast<uv_tty_t*>(raw_handle); });
      }
    });
  } catch (...) {
  }
}

coro::task<void> tty_stream::open(uv_file fd, bool readable, std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }
  if (!is_tty(fd)) {
    throw std::runtime_error("requested file descriptor is not a tty");
  }

  if (is_open()) {
    co_await close(stop_token);
  }

  const uv_file dup_fd = duplicate_file_descriptor(fd);
  try {
    handle_ = runtime_->call([dup_fd, readable](uv_loop_t* loop) {
      auto* handle = new uv_tty_t{};
      const int rc = uv_tty_init(loop, handle, dup_fd, readable ? 1 : 0);
      if (rc < 0) {
        delete handle;
        throw_uv_error(rc, "uv_tty_init");
      }
      return handle;
    });
  } catch (...) {
    close_file_descriptor_noexcept(dup_fd);
    throw;
  }

  fd_ = dup_fd;
  readable_ = readable;
}

coro::task<void> tty_stream::open_stdin(std::stop_token stop_token) {
  co_await open(0, true, stop_token);
}

coro::task<void> tty_stream::open_stdout(std::stop_token stop_token) {
  co_await open(1, false, stop_token);
}

coro::task<void> tty_stream::open_stderr(std::stop_token stop_token) {
  co_await open(2, false, stop_token);
}

coro::task<std::size_t> tty_stream::write(std::string_view data, std::stop_token stop_token) {
  if (!is_open()) {
    throw std::runtime_error("tty stream is not open");
  }
  if (readable_) {
    throw std::runtime_error("tty stream was opened as readable");
  }

  co_return co_await detail::stream_write(*runtime_, handle_, std::string(data), stop_token);
}

coro::task<std::size_t> tty_stream::write_some(std::span<const char> data,
                                               std::stop_token stop_token) {
  if (!is_open()) {
    throw std::runtime_error("tty stream is not open");
  }
  if (readable_) {
    throw std::runtime_error("tty stream was opened as readable");
  }

  co_return co_await detail::stream_write_some(*runtime_, handle_, data, stop_token);
}

coro::task<std::string> tty_stream::read_some(std::size_t max_bytes,
                                              std::stop_token stop_token) {
  if (!is_open()) {
    throw std::runtime_error("tty stream is not open");
  }
  if (!readable_) {
    throw std::runtime_error("tty stream was not opened as readable");
  }

  co_return co_await detail::stream_read_some(*runtime_, handle_, max_bytes, stop_token);
}

coro::task<std::size_t> tty_stream::read_some_into(std::span<char> buffer,
                                                   std::stop_token stop_token) {
  if (!is_open()) {
    throw std::runtime_error("tty stream is not open");
  }
  if (!readable_) {
    throw std::runtime_error("tty stream was not opened as readable");
  }

  co_return co_await detail::stream_read_some_into(*runtime_, handle_, buffer, stop_token);
}

coro::task<std::string> tty_stream::read_exact(std::size_t total_bytes,
                                               std::stop_token stop_token) {
  std::string out;
  out.reserve(total_bytes);

  while (out.size() < total_bytes) {
    if (stop_token.stop_requested()) {
      throw operation_cancelled();
    }
    auto chunk = co_await read_some(total_bytes - out.size(), stop_token);
    if (chunk.empty()) {
      break;
    }
    out += chunk;
  }

  co_return out;
}

coro::task<std::size_t> tty_stream::read_exact_into(std::span<char> buffer,
                                                    std::stop_token stop_token) {
  std::size_t total = 0;

  while (total < buffer.size()) {
    if (stop_token.stop_requested()) {
      throw operation_cancelled();
    }
    auto nread = co_await read_some_into(buffer.subspan(total), stop_token);
    if (nread == 0) {
      break;
    }
    total += nread;
  }

  co_return total;
}

coro::task<void> tty_stream::set_mode(tty_mode mode, std::stop_token stop_token) {
  if (!is_open()) {
    throw std::runtime_error("tty stream is not open");
  }
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  runtime_->call([this, mode](uv_loop_t*) {
    const int rc = uv_tty_set_mode(handle_, static_cast<uv_tty_mode_t>(mode));
    if (rc < 0) {
      throw_uv_error(rc, "uv_tty_set_mode");
    }
  });
  co_return;
}

coro::task<void> tty_stream::close(std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }
  if (!is_open()) {
    co_return;
  }

  uv_tty_t* handle = handle_;
  handle_ = nullptr;
  fd_ = -1;
  readable_ = false;

  runtime_->call([handle](uv_loop_t*) {
    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(handle))) {
      uv_close(
          reinterpret_cast<uv_handle_t*>(handle),
          [](uv_handle_t* raw_handle) { delete reinterpret_cast<uv_tty_t*>(raw_handle); });
    }
  });
  co_return;
}

auto tty_stream::winsize() const -> tty_winsize {
  if (runtime_ == nullptr || handle_ == nullptr) {
    throw std::runtime_error("tty stream is not open");
  }

  return runtime_->call([this](uv_loop_t*) {
    tty_winsize size{};
    const int rc = uv_tty_get_winsize(handle_, &size.width, &size.height);
    if (rc < 0) {
      throw_uv_error(rc, "uv_tty_get_winsize");
    }
    return size;
  });
}

coro::task<void> reset_tty_mode(uv_runtime& runtime, std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  runtime.call([](uv_loop_t*) {
    const int rc = uv_tty_reset_mode();
    if (rc < 0) {
      throw_uv_error(rc, "uv_tty_reset_mode");
    }
  });
  co_return;
}

}  // namespace luvcoro
