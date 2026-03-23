#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

#include <coro/coro.hpp>
#include <uv.h>

#include "luvcoro/control.hpp"
#include "luvcoro/runtime.hpp"

namespace luvcoro {

class process;

class pipe_client {
 public:
  pipe_client();
  explicit pipe_client(uv_runtime& runtime, bool ipc = false);
  ~pipe_client();

  pipe_client(const pipe_client&) = delete;
  pipe_client& operator=(const pipe_client&) = delete;

  pipe_client(pipe_client&& other) noexcept;
  pipe_client& operator=(pipe_client&& other) noexcept;

  coro::task<void> connect(const std::string& name, std::stop_token stop_token = {});
  coro::task<void> open(uv_file fd, bool readable, std::stop_token stop_token = {});
  coro::task<std::size_t> write(std::string_view data, std::stop_token stop_token = {});
  coro::task<std::size_t> write_some(std::span<const char> data, std::stop_token stop_token = {});
  coro::task<std::string> read_some(std::size_t max_bytes = 4096, std::stop_token stop_token = {});
  coro::task<std::size_t> read_some_into(std::span<char> buffer, std::stop_token stop_token = {});
  coro::task<std::string> read_exact(std::size_t total_bytes, std::stop_token stop_token = {});
  coro::task<std::size_t> read_exact_into(std::span<char> buffer, std::stop_token stop_token = {});
  coro::task<void> shutdown(std::stop_token stop_token = {});

  auto local_name() const -> std::string;
  auto peer_name() const -> std::string;

  bool is_connected() const noexcept { return connected_; }
  void close();

 private:
  friend class pipe_server;
  friend class process;
  pipe_client(uv_runtime& runtime, uv_pipe_t* handle, bool connected, bool ipc) noexcept;

  uv_runtime* runtime_ = nullptr;
  uv_pipe_t* handle_ = nullptr;
  bool connected_ = false;
  bool ipc_ = false;
};

class pipe_server {
 public:
  pipe_server();
  explicit pipe_server(uv_runtime& runtime, bool ipc = false);
  ~pipe_server();

  pipe_server(const pipe_server&) = delete;
  pipe_server& operator=(const pipe_server&) = delete;
  pipe_server(pipe_server&&) = delete;
  pipe_server& operator=(pipe_server&&) = delete;

  coro::task<void> bind(const std::string& name, std::stop_token stop_token = {});
  coro::task<void> set_pending_instances(int count, std::stop_token stop_token = {});
  coro::task<void> listen(int backlog = 128, std::stop_token stop_token = {});
  coro::task<pipe_client> accept(std::stop_token stop_token = {});

  auto local_name() const -> std::string;
  void close();

 private:
  static void on_connection(uv_stream_t* server, int status);
  void on_connection_impl(int status);

  uv_runtime* runtime_ = nullptr;
  uv_pipe_t* handle_ = nullptr;
  bool bound_ = false;
  bool listening_ = false;
  bool ipc_ = false;

  std::deque<uv_pipe_t*> pending_clients_{};
  std::coroutine_handle<> accept_waiter_{};
  int accept_status_ = 0;
};

template <typename Rep, typename Period>
inline auto connect_for(pipe_client& client,
                        const std::string& name,
                        std::chrono::duration<Rep, Period> timeout,
                        std::stop_token stop_token = {}) -> coro::task<void> {
  co_await with_timeout(
      stop_token,
      timeout,
      [&client, name](std::stop_token inner_stop_token) {
        return client.connect(name, inner_stop_token);
      });
}

template <typename Rep, typename Period>
inline auto accept_for(pipe_server& server,
                       std::chrono::duration<Rep, Period> timeout,
                       std::stop_token stop_token = {}) -> coro::task<pipe_client> {
  co_return co_await with_timeout(
      stop_token,
      timeout,
      [&server](std::stop_token inner_stop_token) {
        return server.accept(inner_stop_token);
      });
}

}  // namespace luvcoro
