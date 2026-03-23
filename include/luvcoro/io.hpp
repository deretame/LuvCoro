#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <stdexcept>
#include <stop_token>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <coro/coro.hpp>

#include "luvcoro/buffer.hpp"
#include "luvcoro/control.hpp"
#include "luvcoro/endpoint.hpp"
#include "luvcoro/error.hpp"
#include "luvcoro/fs.hpp"
#include "luvcoro/tcp.hpp"
#include "luvcoro/udp.hpp"

namespace luvcoro::io {

struct until_any_result {
  std::string data;
  std::string delimiter;
  std::size_t delimiter_index = static_cast<std::size_t>(-1);
  bool matched = false;
};

template <typename Stream>
concept stream_like = requires(Stream& stream, std::size_t size, std::string_view data) {
  { stream.read_some(size) } -> std::same_as<coro::task<std::string>>;
  { stream.read_exact(size) } -> std::same_as<coro::task<std::string>>;
  { stream.write(data) } -> std::same_as<coro::task<std::size_t>>;
};

template <typename Stream>
concept buffered_stream_like =
    requires(Stream& stream, std::span<char> read_buffer, std::span<const char> write_buffer) {
      { stream.read_some_into(read_buffer) } -> std::same_as<coro::task<std::size_t>>;
      { stream.read_exact_into(read_buffer) } -> std::same_as<coro::task<std::size_t>>;
      { stream.write_some(write_buffer) } -> std::same_as<coro::task<std::size_t>>;
    };

template <typename Socket>
concept datagram_socket_like =
    requires(Socket& socket, const ip_endpoint& endpoint, std::size_t size, std::string_view data) {
      { socket.send_to(endpoint, data) } -> std::same_as<coro::task<std::size_t>>;
      { socket.recv_from(size) } -> std::same_as<coro::task<udp_datagram>>;
    };

template <typename Socket>
concept buffered_datagram_socket_like =
    requires(Socket& socket, const ip_endpoint& endpoint, std::span<char> read_buffer, std::span<const char> write_buffer) {
      { socket.send_some_to(endpoint, write_buffer) } -> std::same_as<coro::task<std::size_t>>;
      { socket.recv_from_into(read_buffer) } -> std::same_as<coro::task<udp_receive_result>>;
    };

template <stream_like Stream>
class stream_ref {
 public:
  explicit stream_ref(Stream& stream) noexcept : stream_(&stream) {}

  auto read_some(std::size_t max_bytes,
                 std::stop_token stop_token = {}) const -> coro::task<std::string> {
    return stream_->read_some(max_bytes, stop_token);
  }

  auto read_exact(std::size_t total_bytes,
                  std::stop_token stop_token = {}) const -> coro::task<std::string> {
    return stream_->read_exact(total_bytes, stop_token);
  }

  auto write(std::string_view data,
             std::stop_token stop_token = {}) const -> coro::task<std::size_t> {
    return stream_->write(data, stop_token);
  }

 private:
  Stream* stream_ = nullptr;
};

template <buffered_stream_like Stream>
class buffered_stream_ref {
 public:
  explicit buffered_stream_ref(Stream& stream) noexcept : stream_(&stream) {}

  auto read_some_into(std::span<char> buffer,
                      std::stop_token stop_token = {}) const -> coro::task<std::size_t> {
    return stream_->read_some_into(buffer, stop_token);
  }

  auto read_exact_into(std::span<char> buffer,
                       std::stop_token stop_token = {}) const -> coro::task<std::size_t> {
    return stream_->read_exact_into(buffer, stop_token);
  }

  auto write_some(std::span<const char> buffer,
                  std::stop_token stop_token = {}) const -> coro::task<std::size_t> {
    return stream_->write_some(buffer, stop_token);
  }

 private:
  Stream* stream_ = nullptr;
};

template <datagram_socket_like Socket>
class datagram_socket_ref {
 public:
  explicit datagram_socket_ref(Socket& socket) noexcept : socket_(&socket) {}

  auto send_to(const ip_endpoint& endpoint,
               std::string_view data,
               std::stop_token stop_token = {}) const
      -> coro::task<std::size_t> {
    return socket_->send_to(endpoint, data, stop_token);
  }

  auto receive(std::size_t max_bytes = 65536,
               std::stop_token stop_token = {}) const -> coro::task<udp_datagram> {
    return socket_->recv_from(max_bytes, stop_token);
  }

 private:
  Socket* socket_ = nullptr;
};

template <buffered_datagram_socket_like Socket>
class buffered_datagram_socket_ref {
 public:
  explicit buffered_datagram_socket_ref(Socket& socket) noexcept : socket_(&socket) {}

  auto send_some_to(const ip_endpoint& endpoint,
                    std::span<const char> buffer,
                    std::stop_token stop_token = {}) const
      -> coro::task<std::size_t> {
    return socket_->send_some_to(endpoint, buffer, stop_token);
  }

  auto receive_into(std::span<char> buffer,
                    std::stop_token stop_token = {}) const -> coro::task<udp_receive_result> {
    return socket_->recv_from_into(buffer, stop_token);
  }

 private:
  Socket* socket_ = nullptr;
};

template <stream_like Stream>
class stream_buffer {
 public:
  struct delimiter_match {
    bool found = false;
    std::size_t start_offset = 0;
    std::size_t end_offset = 0;
    std::size_t delimiter_index = 0;
    std::string delimiter;
  };

  explicit stream_buffer(Stream& stream) noexcept : stream_(&stream) {}

  auto read_some(std::size_t max_bytes = 4096,
                 std::stop_token stop_token = {}) -> coro::task<std::string> {
    if (max_bytes == 0) {
      co_return std::string{};
    }

    if (!pending_.empty()) {
      const auto count = pending_.size() < max_bytes ? pending_.size() : max_bytes;
      auto out = pending_.substr(0, count);
      pending_.erase(0, count);
      co_return out;
    }

    co_return co_await stream_->read_some(max_bytes, stop_token);
  }

  auto read_exact(std::size_t total_bytes,
                  std::stop_token stop_token = {}) -> coro::task<std::string> {
    std::string out;
    out.reserve(total_bytes);

    while (out.size() < total_bytes) {
      if (stop_token.stop_requested()) {
        throw operation_cancelled();
      }
      if (!pending_.empty()) {
        const auto count =
            pending_.size() < (total_bytes - out.size()) ? pending_.size() : (total_bytes - out.size());
        out.append(pending_.data(), count);
        pending_.erase(0, count);
        continue;
      }

      auto chunk = co_await stream_->read_some(total_bytes - out.size(), stop_token);
      if (chunk.empty()) {
        break;
      }
      out += chunk;
    }

    co_return out;
  }

  auto read_until(std::string_view delimiter,
                  std::size_t chunk_size = 4096,
                  std::stop_token stop_token = {})
      -> coro::task<std::string> {
    if (delimiter.empty()) {
      throw std::invalid_argument("read_until delimiter must not be empty");
    }

    while (true) {
      auto match = find_any_delimiter(std::span<const std::string_view>(&delimiter, 1));
      if (match.found) {
        auto out = pending_.substr(0, match.end_offset);
        pending_.erase(0, match.end_offset);
        co_return out;
      }

      if (stop_token.stop_requested()) {
        throw operation_cancelled();
      }
      auto chunk = co_await stream_->read_some(chunk_size, stop_token);
      if (chunk.empty()) {
        auto out = std::move(pending_);
        pending_.clear();
        co_return out;
      }
      pending_ += chunk;
    }
  }

  auto read_until_any(std::span<const std::string_view> delimiters,
                      std::size_t chunk_size = 4096,
                      std::stop_token stop_token = {}) -> coro::task<std::string> {
    auto result = co_await read_until_any_result(delimiters, chunk_size, stop_token);
    co_return result.data;
  }

  auto read_until_any_result(std::span<const std::string_view> delimiters,
                             std::size_t chunk_size = 4096,
                             std::stop_token stop_token = {}) -> coro::task<until_any_result> {
    validate_delimiters(delimiters);

    while (true) {
      auto match = find_any_delimiter(delimiters);
      if (match.found) {
        until_any_result out{};
        out.data = pending_.substr(0, match.end_offset);
        out.delimiter = match.delimiter;
        out.delimiter_index = match.delimiter_index;
        out.matched = true;
        pending_.erase(0, match.end_offset);
        co_return out;
      }

      if (stop_token.stop_requested()) {
        throw operation_cancelled();
      }
      auto chunk = co_await stream_->read_some(chunk_size, stop_token);
      if (chunk.empty()) {
        until_any_result out{};
        out.data = std::move(pending_);
        pending_.clear();
        co_return out;
      }
      pending_ += chunk;
    }
  }

  auto read_exactly(std::size_t total_bytes,
                    std::stop_token stop_token = {}) -> coro::task<std::string> {
    auto out = co_await read_exact(total_bytes, stop_token);
    if (out.size() != total_bytes) {
      throw std::runtime_error("read_exactly reached eof before enough bytes were read");
    }
    co_return out;
  }

  auto pending() const noexcept -> std::string_view {
    return pending_;
  }

 private:
  static void validate_delimiters(std::span<const std::string_view> delimiters) {
    if (delimiters.empty()) {
      throw std::invalid_argument("read_until_any requires at least one delimiter");
    }

    for (auto delimiter : delimiters) {
      if (delimiter.empty()) {
        throw std::invalid_argument("read_until_any delimiter must not be empty");
      }
    }
  }

  auto find_any_delimiter(std::span<const std::string_view> delimiters) const -> delimiter_match {
    delimiter_match best{};
    std::size_t best_start = std::string::npos;
    std::size_t best_length = 0;

    for (std::size_t index = 0; index < delimiters.size(); ++index) {
      const auto delimiter = delimiters[index];
      const auto pos = pending_.find(delimiter);
      if (pos == std::string::npos) {
        continue;
      }

      if (!best.found || pos < best_start ||
          (pos == best_start && delimiter.size() > best_length)) {
        best.found = true;
        best.start_offset = pos;
        best_start = pos;
        best_length = delimiter.size();
        best.end_offset = pos + delimiter.size();
        best.delimiter_index = index;
        best.delimiter = std::string(delimiter);
      }
    }

    return best;
  }

  Stream* stream_ = nullptr;
  std::string pending_;
};

template <stream_like Stream>
auto as_stream(Stream& stream) noexcept -> stream_ref<Stream> {
  return stream_ref<Stream>(stream);
}

template <buffered_stream_like Stream>
auto as_buffered_stream(Stream& stream) noexcept -> buffered_stream_ref<Stream> {
  return buffered_stream_ref<Stream>(stream);
}

template <datagram_socket_like Socket>
auto as_datagram_socket(Socket& socket) noexcept -> datagram_socket_ref<Socket> {
  return datagram_socket_ref<Socket>(socket);
}

template <buffered_datagram_socket_like Socket>
auto as_buffered_datagram_socket(Socket& socket) noexcept -> buffered_datagram_socket_ref<Socket> {
  return buffered_datagram_socket_ref<Socket>(socket);
}

template <stream_like Stream>
auto make_stream_buffer(Stream& stream) -> stream_buffer<Stream> {
  return stream_buffer<Stream>(stream);
}

namespace detail {

template <stream_like Stream>
auto write_owned(Stream& stream,
                 std::string data,
                 std::stop_token stop_token) -> coro::task<std::size_t>;

template <stream_like Stream>
auto read_until_owned(stream_buffer<Stream>& stream,
                      std::string delimiter,
                      std::size_t chunk_size,
                      std::stop_token stop_token) -> coro::task<std::string>;

template <stream_like Stream>
auto read_until_any_owned(stream_buffer<Stream>& stream,
                          std::vector<std::string> delimiters,
                          std::size_t chunk_size,
                          std::stop_token stop_token) -> coro::task<std::string>;

}  // namespace detail

template <stream_like Stream>
auto read_some(Stream& stream,
               std::size_t max_bytes = 4096,
               std::stop_token stop_token = {}) -> coro::task<std::string> {
  return stream.read_some(max_bytes, stop_token);
}

template <stream_like Stream, typename Rep, typename Period>
auto read_some_for(Stream& stream,
                   std::size_t max_bytes,
                   std::chrono::duration<Rep, Period> timeout,
                   std::stop_token stop_token = {}) -> coro::task<std::string> {
  co_return co_await with_timeout(
      stop_token,
      timeout,
      [&stream, max_bytes](std::stop_token inner_stop_token) {
        return stream.read_some(max_bytes, inner_stop_token);
      });
}

template <stream_like Stream>
auto read_some(const stream_ref<Stream>& stream,
               std::size_t max_bytes = 4096,
               std::stop_token stop_token = {})
    -> coro::task<std::string> {
  return stream.read_some(max_bytes, stop_token);
}

template <stream_like Stream>
auto read_some(stream_buffer<Stream>& stream,
               std::size_t max_bytes = 4096,
               std::stop_token stop_token = {})
    -> coro::task<std::string> {
  return stream.read_some(max_bytes, stop_token);
}

template <buffered_stream_like Stream>
auto read_some_into(Stream& stream,
                    std::span<char> buffer,
                    std::stop_token stop_token = {}) -> coro::task<std::size_t> {
  return stream.read_some_into(buffer, stop_token);
}

template <buffered_stream_like Stream>
auto read_some_into(Stream& stream,
                    mutable_buffer buffer,
                    std::stop_token stop_token = {}) -> coro::task<std::size_t> {
  return stream.read_some_into(buffer.as_span(), stop_token);
}

template <buffered_stream_like Stream>
auto read_some_into(const buffered_stream_ref<Stream>& stream, std::span<char> buffer)
    -> coro::task<std::size_t> {
  return stream.read_some_into(buffer);
}

template <buffered_stream_like Stream>
auto read_some_into(const buffered_stream_ref<Stream>& stream, mutable_buffer buffer)
    -> coro::task<std::size_t> {
  return stream.read_some_into(buffer.as_span());
}

template <stream_like Stream>
auto read_exact(Stream& stream,
                std::size_t total_bytes,
                std::stop_token stop_token = {}) -> coro::task<std::string> {
  return stream.read_exact(total_bytes, stop_token);
}

template <stream_like Stream, typename Rep, typename Period>
auto read_exact_for(Stream& stream,
                    std::size_t total_bytes,
                    std::chrono::duration<Rep, Period> timeout,
                    std::stop_token stop_token = {}) -> coro::task<std::string> {
  co_return co_await with_timeout(
      stop_token,
      timeout,
      [&stream, total_bytes](std::stop_token inner_stop_token) {
        return stream.read_exact(total_bytes, inner_stop_token);
      });
}

template <stream_like Stream>
auto read_exact(const stream_ref<Stream>& stream, std::size_t total_bytes)
    -> coro::task<std::string> {
  return stream.read_exact(total_bytes);
}

template <stream_like Stream>
auto read_exact(stream_buffer<Stream>& stream, std::size_t total_bytes)
    -> coro::task<std::string> {
  return stream.read_exact(total_bytes);
}

template <stream_like Stream>
auto read_exactly(Stream& stream,
                  std::size_t total_bytes,
                  std::stop_token stop_token = {}) -> coro::task<std::string> {
  auto out = co_await stream.read_exact(total_bytes, stop_token);
  if (out.size() != total_bytes) {
    throw std::runtime_error("read_exactly reached eof before enough bytes were read");
  }
  co_return out;
}

template <stream_like Stream>
auto read_exactly(const stream_ref<Stream>& stream, std::size_t total_bytes)
    -> coro::task<std::string> {
  auto out = co_await stream.read_exact(total_bytes);
  if (out.size() != total_bytes) {
    throw std::runtime_error("read_exactly reached eof before enough bytes were read");
  }
  co_return out;
}

template <stream_like Stream>
auto read_exactly(stream_buffer<Stream>& stream, std::size_t total_bytes)
    -> coro::task<std::string> {
  return stream.read_exactly(total_bytes);
}

template <buffered_stream_like Stream>
auto read_exactly_into(Stream& stream,
                       std::span<char> buffer,
                       std::stop_token stop_token = {}) -> coro::task<std::size_t> {
  auto nread = co_await stream.read_exact_into(buffer, stop_token);
  if (nread != buffer.size()) {
    throw std::runtime_error("read_exactly_into reached eof before enough bytes were read");
  }
  co_return nread;
}

template <buffered_stream_like Stream>
auto read_exactly_into(Stream& stream, mutable_buffer buffer) -> coro::task<std::size_t> {
  auto span = buffer.as_span();
  auto nread = co_await stream.read_exact_into(span);
  if (nread != span.size()) {
    throw std::runtime_error("read_exactly_into reached eof before enough bytes were read");
  }
  co_return nread;
}

template <buffered_stream_like Stream>
auto read_exactly_into(const buffered_stream_ref<Stream>& stream, std::span<char> buffer)
    -> coro::task<std::size_t> {
  auto nread = co_await stream.read_exact_into(buffer);
  if (nread != buffer.size()) {
    throw std::runtime_error("read_exactly_into reached eof before enough bytes were read");
  }
  co_return nread;
}

template <buffered_stream_like Stream>
auto read_exactly_into(const buffered_stream_ref<Stream>& stream, mutable_buffer buffer)
    -> coro::task<std::size_t> {
  auto span = buffer.as_span();
  auto nread = co_await stream.read_exact_into(span);
  if (nread != span.size()) {
    throw std::runtime_error("read_exactly_into reached eof before enough bytes were read");
  }
  co_return nread;
}

template <stream_like Stream>
auto read_exactly_into(stream_buffer<Stream>& stream, std::span<char> buffer)
    -> coro::task<std::size_t> {
  auto data = co_await stream.read_exactly(buffer.size());
  std::copy_n(data.data(), data.size(), buffer.data());
  co_return data.size();
}

template <stream_like Stream>
auto read_exactly_into(stream_buffer<Stream>& stream, mutable_buffer buffer)
    -> coro::task<std::size_t> {
  auto span = buffer.as_span();
  auto data = co_await stream.read_exactly(span.size());
  std::copy_n(data.data(), data.size(), span.data());
  co_return data.size();
}

template <buffered_stream_like Stream>
auto read_exact_into(Stream& stream, std::span<char> buffer) -> coro::task<std::size_t> {
  return stream.read_exact_into(buffer);
}

template <buffered_stream_like Stream>
auto read_exact_into(Stream& stream, mutable_buffer buffer) -> coro::task<std::size_t> {
  return stream.read_exact_into(buffer.as_span());
}

template <buffered_stream_like Stream>
auto read_exact_into(const buffered_stream_ref<Stream>& stream, std::span<char> buffer)
    -> coro::task<std::size_t> {
  return stream.read_exact_into(buffer);
}

template <buffered_stream_like Stream>
auto read_exact_into(const buffered_stream_ref<Stream>& stream, mutable_buffer buffer)
    -> coro::task<std::size_t> {
  return stream.read_exact_into(buffer.as_span());
}

template <stream_like Stream>
auto write(Stream& stream,
           std::string_view data,
           std::stop_token stop_token = {}) -> coro::task<std::size_t> {
  return stream.write(data, stop_token);
}

template <stream_like Stream, typename Rep, typename Period>
auto write_for(Stream& stream,
               std::string_view data,
               std::chrono::duration<Rep, Period> timeout,
               std::stop_token stop_token = {}) -> coro::task<std::size_t> {
  std::string owned(data);
  co_return co_await with_timeout(
      stop_token,
      timeout,
      [&stream, owned = std::move(owned)](std::stop_token inner_stop_token) mutable {
        return detail::write_owned(stream, std::move(owned), inner_stop_token);
      });
}

template <stream_like Stream>
auto write(const stream_ref<Stream>& stream, std::string_view data)
    -> coro::task<std::size_t> {
  return stream.write(data);
}

template <stream_like Stream>
auto write_all(Stream& stream,
               std::string_view data,
               std::stop_token stop_token = {}) -> coro::task<std::size_t> {
  std::string owned(data);
  std::size_t total = 0;

  while (total < owned.size()) {
    if (stop_token.stop_requested()) {
      throw operation_cancelled();
    }
    auto written =
        co_await stream.write(std::string_view(owned).substr(total), stop_token);
    if (written == 0) {
      throw std::runtime_error("write_all made no progress");
    }
    total += written;
  }

  co_return total;
}

template <stream_like Stream>
auto write_all(const stream_ref<Stream>& stream, std::string_view data)
    -> coro::task<std::size_t> {
  std::string owned(data);
  std::size_t total = 0;

  while (total < owned.size()) {
    auto written = co_await stream.write(std::string_view(owned).substr(total));
    if (written == 0) {
      throw std::runtime_error("write_all made no progress");
    }
    total += written;
  }

  co_return total;
}

template <buffered_stream_like Stream>
auto write_some(Stream& stream,
                std::span<const char> buffer,
                std::stop_token stop_token = {}) -> coro::task<std::size_t> {
  return stream.write_some(buffer, stop_token);
}

template <buffered_stream_like Stream>
auto write_some(Stream& stream, const_buffer buffer) -> coro::task<std::size_t> {
  return stream.write_some(buffer.as_span());
}

template <buffered_stream_like Stream>
auto write_some(const buffered_stream_ref<Stream>& stream, std::span<const char> buffer)
    -> coro::task<std::size_t> {
  return stream.write_some(buffer);
}

template <buffered_stream_like Stream>
auto write_some(const buffered_stream_ref<Stream>& stream, const_buffer buffer)
    -> coro::task<std::size_t> {
  return stream.write_some(buffer.as_span());
}

template <datagram_socket_like Socket>
auto send_to(Socket& socket,
             const ip_endpoint& endpoint,
             std::string_view data,
             std::stop_token stop_token = {})
    -> coro::task<std::size_t> {
  return socket.send_to(endpoint, data, stop_token);
}

template <datagram_socket_like Socket>
auto send_to(const datagram_socket_ref<Socket>& socket,
             const ip_endpoint& endpoint,
             std::string_view data) -> coro::task<std::size_t> {
  return socket.send_to(endpoint, data);
}

template <buffered_datagram_socket_like Socket>
auto send_some_to(Socket& socket,
                  const ip_endpoint& endpoint,
                  std::span<const char> buffer,
                  std::stop_token stop_token = {})
    -> coro::task<std::size_t> {
  return socket.send_some_to(endpoint, buffer, stop_token);
}

template <buffered_datagram_socket_like Socket>
auto send_some_to(Socket& socket, const ip_endpoint& endpoint, const_buffer buffer)
    -> coro::task<std::size_t> {
  return socket.send_some_to(endpoint, buffer.as_span());
}

template <buffered_datagram_socket_like Socket>
auto send_some_to(const buffered_datagram_socket_ref<Socket>& socket,
                  const ip_endpoint& endpoint,
                  std::span<const char> buffer) -> coro::task<std::size_t> {
  return socket.send_some_to(endpoint, buffer);
}

template <buffered_datagram_socket_like Socket>
auto send_some_to(const buffered_datagram_socket_ref<Socket>& socket,
                  const ip_endpoint& endpoint,
                  const_buffer buffer) -> coro::task<std::size_t> {
  return socket.send_some_to(endpoint, buffer.as_span());
}

template <datagram_socket_like Socket>
auto receive(Socket& socket,
             std::size_t max_bytes = 65536,
             std::stop_token stop_token = {}) -> coro::task<udp_datagram> {
  return socket.recv_from(max_bytes, stop_token);
}

template <datagram_socket_like Socket, typename Rep, typename Period>
auto receive_for(Socket& socket,
                 std::size_t max_bytes,
                 std::chrono::duration<Rep, Period> timeout,
                 std::stop_token stop_token = {}) -> coro::task<udp_datagram> {
  co_return co_await with_timeout(
      stop_token,
      timeout,
      [&socket, max_bytes](std::stop_token inner_stop_token) {
        return socket.recv_from(max_bytes, inner_stop_token);
      });
}

template <datagram_socket_like Socket>
auto receive(const datagram_socket_ref<Socket>& socket, std::size_t max_bytes = 65536)
    -> coro::task<udp_datagram> {
  return socket.receive(max_bytes);
}

template <buffered_datagram_socket_like Socket>
auto receive_into(Socket& socket,
                  std::span<char> buffer,
                  std::stop_token stop_token = {}) -> coro::task<udp_receive_result> {
  return socket.recv_from_into(buffer, stop_token);
}

template <buffered_datagram_socket_like Socket, typename Rep, typename Period>
auto receive_into_for(Socket& socket,
                      std::span<char> buffer,
                      std::chrono::duration<Rep, Period> timeout,
                      std::stop_token stop_token = {}) -> coro::task<udp_receive_result> {
  co_return co_await with_timeout(
      stop_token,
      timeout,
      [&socket, buffer](std::stop_token inner_stop_token) {
        return socket.recv_from_into(buffer, inner_stop_token);
      });
}

template <buffered_datagram_socket_like Socket>
auto receive_into(Socket& socket, mutable_buffer buffer) -> coro::task<udp_receive_result> {
  return socket.recv_from_into(buffer.as_span());
}

template <buffered_datagram_socket_like Socket>
auto receive_into(const buffered_datagram_socket_ref<Socket>& socket, std::span<char> buffer)
    -> coro::task<udp_receive_result> {
  return socket.receive_into(buffer);
}

template <buffered_datagram_socket_like Socket>
auto receive_into(const buffered_datagram_socket_ref<Socket>& socket, mutable_buffer buffer)
    -> coro::task<udp_receive_result> {
  return socket.receive_into(buffer.as_span());
}

template <stream_like Stream>
auto read_until(stream_buffer<Stream>& stream,
                std::string_view delimiter,
                std::size_t chunk_size = 4096,
                std::stop_token stop_token = {}) -> coro::task<std::string> {
  return stream.read_until(delimiter, chunk_size, stop_token);
}

template <stream_like Stream, typename Rep, typename Period>
auto read_until_for(stream_buffer<Stream>& stream,
                    std::string_view delimiter,
                    std::chrono::duration<Rep, Period> timeout,
                    std::size_t chunk_size = 4096,
                    std::stop_token stop_token = {}) -> coro::task<std::string> {
  std::string owned(delimiter);
  co_return co_await with_timeout(
      stop_token,
      timeout,
      [&stream, owned = std::move(owned), chunk_size](std::stop_token inner_stop_token) mutable {
        return detail::read_until_owned(
            stream, std::move(owned), chunk_size, inner_stop_token);
      });
}

template <stream_like Stream>
auto read_until_any(stream_buffer<Stream>& stream,
                    std::span<const std::string_view> delimiters,
                    std::size_t chunk_size = 4096,
                    std::stop_token stop_token = {}) -> coro::task<std::string> {
  return stream.read_until_any(delimiters, chunk_size, stop_token);
}

template <stream_like Stream, typename Rep, typename Period>
auto read_until_any_for(stream_buffer<Stream>& stream,
                        std::span<const std::string_view> delimiters,
                        std::chrono::duration<Rep, Period> timeout,
                        std::size_t chunk_size = 4096,
                        std::stop_token stop_token = {}) -> coro::task<std::string> {
  std::vector<std::string> owned_storage;
  owned_storage.reserve(delimiters.size());
  for (auto delimiter : delimiters) {
    owned_storage.emplace_back(delimiter);
  }

  co_return co_await with_timeout(
      stop_token,
      timeout,
      [&stream, owned_storage = std::move(owned_storage), chunk_size](std::stop_token inner_stop_token) mutable {
        return detail::read_until_any_owned(
            stream, std::move(owned_storage), chunk_size, inner_stop_token);
      });
}

template <stream_like Stream>
auto read_until_any_result(stream_buffer<Stream>& stream,
                           std::span<const std::string_view> delimiters,
                           std::size_t chunk_size = 4096,
                           std::stop_token stop_token = {}) -> coro::task<until_any_result> {
  return stream.read_until_any_result(delimiters, chunk_size, stop_token);
}

template <stream_like Stream, std::size_t N>
auto read_until_any(stream_buffer<Stream>& stream,
                    const std::string_view (&delimiters)[N],
                    std::size_t chunk_size = 4096) -> coro::task<std::string> {
  return stream.read_until_any(std::span<const std::string_view>(delimiters, N), chunk_size);
}

template <stream_like Stream, std::size_t N>
auto read_until_any_result(stream_buffer<Stream>& stream,
                           const std::string_view (&delimiters)[N],
                           std::size_t chunk_size = 4096) -> coro::task<until_any_result> {
  return stream.read_until_any_result(std::span<const std::string_view>(delimiters, N), chunk_size);
}

template <stream_like Stream, std::size_t N>
auto read_until_any(stream_buffer<Stream>& stream,
                    const std::array<std::string_view, N>& delimiters,
                    std::size_t chunk_size = 4096) -> coro::task<std::string> {
  return stream.read_until_any(std::span<const std::string_view>(delimiters.data(), delimiters.size()),
                               chunk_size);
}

template <stream_like Stream, std::size_t N>
auto read_until_any_result(stream_buffer<Stream>& stream,
                           const std::array<std::string_view, N>& delimiters,
                           std::size_t chunk_size = 4096) -> coro::task<until_any_result> {
  return stream.read_until_any_result(
      std::span<const std::string_view>(delimiters.data(), delimiters.size()),
      chunk_size);
}

template <stream_like Stream>
auto read_line(stream_buffer<Stream>& stream,
               bool keep_end_of_line = false,
               std::stop_token stop_token = {})
    -> coro::task<std::string> {
  auto line = co_await stream.read_until("\n", 4096, stop_token);
  if (!keep_end_of_line) {
    if (!line.empty() && line.back() == '\n') {
      line.pop_back();
    }
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
  }
  co_return line;
}

namespace detail {

template <stream_like Stream>
auto write_owned(Stream& stream,
                 std::string data,
                 std::stop_token stop_token) -> coro::task<std::size_t> {
  co_return co_await stream.write(data, stop_token);
}

template <stream_like Stream>
auto read_until_owned(stream_buffer<Stream>& stream,
                      std::string delimiter,
                      std::size_t chunk_size,
                      std::stop_token stop_token) -> coro::task<std::string> {
  co_return co_await stream.read_until(delimiter, chunk_size, stop_token);
}

template <stream_like Stream>
auto read_until_any_owned(stream_buffer<Stream>& stream,
                          std::vector<std::string> delimiters,
                          std::size_t chunk_size,
                          std::stop_token stop_token) -> coro::task<std::string> {
  std::vector<std::string_view> views;
  views.reserve(delimiters.size());
  for (const auto& delimiter : delimiters) {
    views.emplace_back(delimiter);
  }
  co_return co_await stream.read_until_any(
      std::span<const std::string_view>(views.data(), views.size()),
      chunk_size,
      stop_token);
}

template <buffered_stream_like Stream>
auto write_all_buffer(Stream& stream, std::span<const char> buffer)
    -> coro::task<std::size_t> {
  std::size_t total = 0;

  while (total < buffer.size()) {
    auto written = co_await stream.write_some(buffer.subspan(total));
    if (written == 0) {
      throw std::runtime_error("write_all_buffer made no progress");
    }
    total += written;
  }

  co_return total;
}

template <buffered_stream_like Stream>
auto write_all_buffer(const buffered_stream_ref<Stream>& stream, std::span<const char> buffer)
    -> coro::task<std::size_t> {
  std::size_t total = 0;

  while (total < buffer.size()) {
    auto written = co_await stream.write_some(buffer.subspan(total));
    if (written == 0) {
      throw std::runtime_error("write_all_buffer made no progress");
    }
    total += written;
  }

  co_return total;
}

}  // namespace detail

template <buffered_stream_like Source, buffered_stream_like Sink>
auto copy(Source& source, Sink& sink, std::size_t buffer_size = 64 * 1024)
    -> coro::task<std::size_t> {
  if (buffer_size == 0) {
    throw std::invalid_argument("copy buffer_size must be > 0");
  }

  std::string buffer(buffer_size, '\0');
  std::size_t total = 0;

  while (true) {
    auto nread = co_await source.read_some_into(std::span<char>(buffer.data(), buffer.size()));
    if (nread == 0) {
      break;
    }

    total += co_await detail::write_all_buffer(sink, std::span<const char>(buffer.data(), nread));
  }

  co_return total;
}

template <buffered_stream_like Source, buffered_stream_like Sink>
auto copy(const buffered_stream_ref<Source>& source,
          const buffered_stream_ref<Sink>& sink,
          std::size_t buffer_size = 64 * 1024) -> coro::task<std::size_t> {
  if (buffer_size == 0) {
    throw std::invalid_argument("copy buffer_size must be > 0");
  }

  std::string buffer(buffer_size, '\0');
  std::size_t total = 0;

  while (true) {
    auto nread = co_await source.read_some_into(std::span<char>(buffer.data(), buffer.size()));
    if (nread == 0) {
      break;
    }

    total += co_await detail::write_all_buffer(sink, std::span<const char>(buffer.data(), nread));
  }

  co_return total;
}

template <buffered_stream_like Source, buffered_stream_like Sink>
auto copy_n(Source& source, Sink& sink, std::size_t bytes_to_copy, std::size_t buffer_size = 64 * 1024)
    -> coro::task<std::size_t> {
  if (buffer_size == 0) {
    throw std::invalid_argument("copy_n buffer_size must be > 0");
  }

  std::string buffer(buffer_size, '\0');
  std::size_t total = 0;

  while (total < bytes_to_copy) {
    const auto wanted = (bytes_to_copy - total) < buffer.size() ? (bytes_to_copy - total) : buffer.size();
    auto nread = co_await source.read_some_into(std::span<char>(buffer.data(), wanted));
    if (nread == 0) {
      break;
    }

    total += co_await detail::write_all_buffer(
        sink, const_buffer(buffer.data(), nread).as_span());
  }

  co_return total;
}

template <buffered_stream_like Source, buffered_stream_like Sink>
auto copy_n(const buffered_stream_ref<Source>& source,
            const buffered_stream_ref<Sink>& sink,
            std::size_t bytes_to_copy,
            std::size_t buffer_size = 64 * 1024) -> coro::task<std::size_t> {
  if (buffer_size == 0) {
    throw std::invalid_argument("copy_n buffer_size must be > 0");
  }

  std::string buffer(buffer_size, '\0');
  std::size_t total = 0;

  while (total < bytes_to_copy) {
    const auto wanted = (bytes_to_copy - total) < buffer.size() ? (bytes_to_copy - total) : buffer.size();
    auto nread = co_await source.read_some_into(std::span<char>(buffer.data(), wanted));
    if (nread == 0) {
      break;
    }

    total += co_await detail::write_all_buffer(
        sink, const_buffer(buffer.data(), nread).as_span());
  }

  co_return total;
}

inline auto sendfile(file& target,
                     file& source,
                     std::size_t max_bytes,
                     std::stop_token stop_token = {}) -> coro::task<std::size_t> {
  co_return co_await fs::sendfile(target, source, max_bytes, stop_token);
}

inline auto sendfile(file& target,
                     file& source,
                     std::int64_t source_offset,
                     std::size_t max_bytes,
                     std::stop_token stop_token = {}) -> coro::task<std::size_t> {
  co_return co_await fs::sendfile(target, source, source_offset, max_bytes, stop_token);
}

}  // namespace luvcoro::io
