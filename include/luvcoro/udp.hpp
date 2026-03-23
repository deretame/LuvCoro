#pragma once

#include <cstddef>
#include <span>
#include <stop_token>
#include <string_view>
#include <string>

#include <coro/coro.hpp>
#include <uv.h>

#include "luvcoro/endpoint.hpp"
#include "luvcoro/runtime.hpp"

namespace luvcoro {

struct udp_datagram {
  std::string data;
  ip_endpoint peer;
  std::string ip;
  int port = 0;
  bool ipv6 = false;
};

struct udp_receive_result {
  std::size_t size = 0;
  ip_endpoint peer;
  std::string ip;
  int port = 0;
  bool ipv6 = false;
};

class udp_socket {
 public:
  udp_socket();
  explicit udp_socket(uv_runtime& runtime);
  ~udp_socket();

  udp_socket(const udp_socket&) = delete;
  udp_socket& operator=(const udp_socket&) = delete;

  udp_socket(udp_socket&& other) noexcept;
  udp_socket& operator=(udp_socket&& other) noexcept;

  coro::task<void> bind(const std::string& ip,
                        int port,
                        unsigned int flags = 0,
                        std::stop_token stop_token = {});
  coro::task<void> bind(const ip_endpoint& endpoint,
                        unsigned int flags = 0,
                        std::stop_token stop_token = {});
  coro::task<void> set_broadcast(bool enabled = true, std::stop_token stop_token = {});
  coro::task<void> set_multicast_loop(bool enabled = true, std::stop_token stop_token = {});
  coro::task<void> set_multicast_ttl(int ttl, std::stop_token stop_token = {});
  coro::task<void> set_multicast_interface(const std::string& interface_address,
                                           std::stop_token stop_token = {});
  coro::task<void> join_multicast_group(const std::string& multicast_address,
                                        const std::string& interface_address = "",
                                        std::stop_token stop_token = {});
  coro::task<void> leave_multicast_group(const std::string& multicast_address,
                                         const std::string& interface_address = "",
                                         std::stop_token stop_token = {});
  coro::task<std::size_t> send_to(const std::string& ip,
                                  int port,
                                  std::string_view data,
                                  std::stop_token stop_token = {});
  coro::task<std::size_t> send_to(const ip_endpoint& endpoint,
                                  std::string_view data,
                                  std::stop_token stop_token = {});
  coro::task<std::size_t> send_some_to(const ip_endpoint& endpoint,
                                       std::span<const char> data,
                                       std::stop_token stop_token = {});
  coro::task<udp_datagram> recv_from(std::size_t max_bytes = 65536,
                                     std::stop_token stop_token = {});
  coro::task<udp_receive_result> recv_from_into(std::span<char> buffer,
                                                std::stop_token stop_token = {});

  void close();

 private:
  uv_runtime* runtime_;
  uv_udp_t* handle_ = nullptr;
  bool bound_ = false;
};

}  // namespace luvcoro
