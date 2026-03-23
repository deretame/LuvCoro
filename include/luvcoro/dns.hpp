#pragma once

#include <stop_token>
#include <string>
#include <vector>

#include <coro/coro.hpp>
#include <uv.h>

#include "luvcoro/endpoint.hpp"
#include "luvcoro/runtime.hpp"

namespace luvcoro::dns {

struct name_info {
  std::string host;
  std::string service;
};

coro::task<std::vector<ip_endpoint>> resolve(uv_runtime& runtime,
                                             const std::string& host,
                                             const std::string& service = "",
                                             int family = AF_UNSPEC,
                                             int socktype = SOCK_STREAM,
                                             int flags = 0,
                                             std::stop_token stop_token = {});

inline coro::task<std::vector<ip_endpoint>> resolve(const std::string& host,
                                                    const std::string& service = "",
                                                    int family = AF_UNSPEC,
                                                    int socktype = SOCK_STREAM,
                                                    int flags = 0,
                                                    std::stop_token stop_token = {}) {
  co_return co_await resolve(
      current_runtime(), host, service, family, socktype, flags, stop_token);
}

inline coro::task<std::vector<ip_endpoint>> resolve(const std::string& host,
                                                    int port,
                                                    int family = AF_UNSPEC,
                                                    int socktype = SOCK_STREAM,
                                                    int flags = 0,
                                                    std::stop_token stop_token = {}) {
  co_return co_await resolve(current_runtime(),
                             host,
                             std::to_string(port),
                             family,
                             socktype,
                             flags,
                             stop_token);
}

inline coro::task<std::vector<ip_endpoint>> resolve(uv_runtime& runtime,
                                                    const std::string& host,
                                                    int port,
                                                    int family = AF_UNSPEC,
                                                    int socktype = SOCK_STREAM,
                                                    int flags = 0,
                                                    std::stop_token stop_token = {}) {
  co_return co_await resolve(
      runtime, host, std::to_string(port), family, socktype, flags, stop_token);
}

coro::task<name_info> reverse_lookup(uv_runtime& runtime,
                                     const ip_endpoint& endpoint,
                                     int flags = 0,
                                     std::stop_token stop_token = {});

inline coro::task<name_info> reverse_lookup(const ip_endpoint& endpoint,
                                            int flags = 0,
                                            std::stop_token stop_token = {}) {
  co_return co_await reverse_lookup(current_runtime(), endpoint, flags, stop_token);
}

}  // namespace luvcoro::dns
