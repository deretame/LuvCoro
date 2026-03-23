#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <span>
#include <stop_token>
#include <string_view>
#include <string>

#include <coro/coro.hpp>
#include <uv.h>

#include "luvcoro/control.hpp"
#include "luvcoro/endpoint.hpp"
#include "luvcoro/runtime.hpp"

namespace luvcoro {

class tcp_client {
 public:
  tcp_client();
  explicit tcp_client(uv_runtime& runtime);
  ~tcp_client();

  tcp_client(const tcp_client&) = delete;
  tcp_client& operator=(const tcp_client&) = delete;

  tcp_client(tcp_client&& other) noexcept;
  tcp_client& operator=(tcp_client&& other) noexcept;

  coro::task<void> connect(const std::string& ip, int port, std::stop_token stop_token = {});
  coro::task<void> connect(const ip_endpoint& endpoint, std::stop_token stop_token = {});
  coro::task<std::size_t> write(std::string_view data, std::stop_token stop_token = {});
  coro::task<std::size_t> write_some(std::span<const char> data, std::stop_token stop_token = {});
  coro::task<std::string> read_some(std::size_t max_bytes = 4096, std::stop_token stop_token = {});
  coro::task<std::size_t> read_some_into(std::span<char> buffer, std::stop_token stop_token = {});
  coro::task<std::string> read_exact(std::size_t total_bytes, std::stop_token stop_token = {});
  coro::task<std::size_t> read_exact_into(std::span<char> buffer, std::stop_token stop_token = {});
  coro::task<void> shutdown(std::stop_token stop_token = {});
  coro::task<void> set_nodelay(bool enabled = true, std::stop_token stop_token = {});
  coro::task<void> set_keepalive(bool enabled = true,
                                 unsigned int delay = 60,
                                 std::stop_token stop_token = {});
  auto local_endpoint() const -> ip_endpoint;
  auto remote_endpoint() const -> ip_endpoint;
  bool is_connected() const noexcept { return connected_; }

  void close();

 private:
  friend class tcp_server;
  tcp_client(uv_runtime& runtime, uv_tcp_t* handle, bool connected) noexcept;

  uv_runtime* runtime_;
  uv_tcp_t* handle_ = nullptr;
  bool connected_ = false;
};

class tcp_server {
 public:
  tcp_server();
  explicit tcp_server(uv_runtime& runtime);
  ~tcp_server();

  tcp_server(const tcp_server&) = delete;
  tcp_server& operator=(const tcp_server&) = delete;
  tcp_server(tcp_server&&) = delete;
  tcp_server& operator=(tcp_server&&) = delete;

  coro::task<void> bind(const std::string& ip,
                        int port,
                        unsigned int flags = 0,
                        std::stop_token stop_token = {});
  coro::task<void> bind(const ip_endpoint& endpoint,
                        unsigned int flags = 0,
                        std::stop_token stop_token = {});
  coro::task<void> set_simultaneous_accepts(bool enabled = true,
                                            std::stop_token stop_token = {});
  coro::task<void> listen(int backlog = 128, std::stop_token stop_token = {});
  coro::task<tcp_client> accept(std::stop_token stop_token = {});
  auto local_endpoint() const -> ip_endpoint;

  void close();

 private:
  static void on_connection(uv_stream_t* server, int status);
  void on_connection_impl(int status);

  uv_runtime* runtime_;
  uv_tcp_t* handle_ = nullptr;
  bool bound_ = false;
  bool listening_ = false;

  std::deque<uv_tcp_t*> pending_clients_;
  std::coroutine_handle<> accept_waiter_{};
  int accept_status_ = 0;
};

template <typename Rep, typename Period>
inline auto connect_for(tcp_client& client,
                        const ip_endpoint& endpoint,
                        std::chrono::duration<Rep, Period> timeout,
                        std::stop_token stop_token = {}) -> coro::task<void> {
  co_await with_timeout(
      stop_token,
      timeout,
      [&client, endpoint](std::stop_token inner_stop_token) {
        return client.connect(endpoint, inner_stop_token);
      });
}

template <typename Rep, typename Period>
inline auto connect_for(tcp_client& client,
                        const std::string& ip,
                        int port,
                        std::chrono::duration<Rep, Period> timeout,
                        std::stop_token stop_token = {}) -> coro::task<void> {
  co_await connect_for(client, ip_endpoint{ip, port}, timeout, stop_token);
}

template <typename Rep, typename Period>
inline auto accept_for(tcp_server& server,
                       std::chrono::duration<Rep, Period> timeout,
                       std::stop_token stop_token = {}) -> coro::task<tcp_client> {
  co_return co_await with_timeout(
      stop_token,
      timeout,
      [&server](std::stop_token inner_stop_token) {
        return server.accept(inner_stop_token);
      });
}

}  // namespace luvcoro
