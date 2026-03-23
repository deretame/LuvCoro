#include "luvcoro/dns.hpp"

#include <atomic>
#include <coroutine>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <vector>

#include "luvcoro/error.hpp"

namespace luvcoro::dns {
namespace {

template <typename Fn>
using stop_callback_t = std::stop_callback<std::decay_t<Fn>>;

auto sockaddr_to_endpoint(const sockaddr* addr) -> ip_endpoint {
  char ipbuf[INET6_ADDRSTRLEN] = {};

  if (addr->sa_family == AF_INET) {
    const auto* addr4 = reinterpret_cast<const sockaddr_in*>(addr);
    int rc = uv_ip4_name(addr4, ipbuf, sizeof(ipbuf));
    if (rc < 0) {
      throw_uv_error(rc, "uv_ip4_name");
    }

    return ip_endpoint{ipbuf, ntohs(addr4->sin_port), false};
  }

  if (addr->sa_family == AF_INET6) {
    const auto* addr6 = reinterpret_cast<const sockaddr_in6*>(addr);
    int rc = uv_ip6_name(addr6, ipbuf, sizeof(ipbuf));
    if (rc < 0) {
      throw_uv_error(rc, "uv_ip6_name");
    }

    return ip_endpoint{ipbuf, ntohs(addr6->sin6_port), true};
  }

  throw std::runtime_error("unsupported address family");
}

auto endpoint_to_sockaddr(const ip_endpoint& endpoint, sockaddr_storage& storage) -> const sockaddr* {
  sockaddr_in addr4{};
  int rc = uv_ip4_addr(endpoint.address.c_str(), endpoint.port, &addr4);
  if (rc == 0) {
    std::memcpy(&storage, &addr4, sizeof(addr4));
    return reinterpret_cast<const sockaddr*>(&storage);
  }

  sockaddr_in6 addr6{};
  rc = uv_ip6_addr(endpoint.address.c_str(), endpoint.port, &addr6);
  if (rc == 0) {
    std::memcpy(&storage, &addr6, sizeof(addr6));
    return reinterpret_cast<const sockaddr*>(&storage);
  }

  throw_uv_error(rc, "uv_ip*_addr");
}

}  // namespace

coro::task<std::vector<ip_endpoint>> resolve(uv_runtime& runtime,
                                             const std::string& host,
                                             const std::string& service,
                                             int family,
                                             int socktype,
                                             int flags,
                                             std::stop_token stop_token) {
  struct state {
    uv_runtime* runtime = nullptr;
    std::string host;
    std::string service;
    int family;
    int socktype;
    int flags;
    uv_getaddrinfo_t req{};
    std::coroutine_handle<> continuation{};
    std::shared_ptr<state> keep_alive{};
    std::optional<stop_callback_t<std::function<void()>>> stop_callback{};
    std::atomic<bool> completed = false;
    int status = 0;
    std::vector<ip_endpoint> results;
    bool cancel_requested = false;
    bool started = false;

    void complete() {
      if (completed.exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      auto h = continuation;
      continuation = {};
      if (h) {
        h.resume();
      }
      stop_callback.reset();
      keep_alive.reset();
    }
  };

  struct awaiter {
    std::shared_ptr<state> shared_state;
    std::stop_token stop_token;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) {
      if (stop_token.stop_requested()) {
        shared_state->cancel_requested = true;
        shared_state->status = UV_ECANCELED;
        return false;
      }

      shared_state->continuation = h;
      shared_state->keep_alive = shared_state;
      shared_state->req.data = shared_state.get();

      if (stop_token.stop_possible()) {
        shared_state->stop_callback.emplace(stop_token, std::function<void()>(
            [op_state = shared_state]() {
              op_state->runtime->post([op_state]() {
                if (op_state->completed.load(std::memory_order_acquire)) {
                  return;
                }
                op_state->cancel_requested = true;
                if (!op_state->started) {
                  op_state->status = UV_ECANCELED;
                  op_state->complete();
                  return;
                }
                const int rc =
                    uv_cancel(reinterpret_cast<uv_req_t*>(&op_state->req));
                if (rc < 0 && rc != UV_EBUSY && rc != UV_ENOSYS) {
                  op_state->status = UV_ECANCELED;
                  op_state->complete();
                }
              });
            }));
      }

      shared_state->runtime->post([op_state = shared_state]() {
        if (op_state->completed.load(std::memory_order_acquire)) {
          return;
        }
        addrinfo hints{};
        hints.ai_family = op_state->family;
        hints.ai_socktype = op_state->socktype;
        hints.ai_flags = op_state->flags;

        const char* node = op_state->host.empty() ? nullptr : op_state->host.c_str();
        const char* service_name =
            op_state->service.empty() ? nullptr : op_state->service.c_str();

        op_state->started = true;
        int rc = uv_getaddrinfo(
            op_state->runtime->loop(),
            &op_state->req,
            [](uv_getaddrinfo_t* req, int status, addrinfo* res) {
              auto* self = static_cast<state*>(req->data);
              self->status = status;

              if (status == 0) {
                for (auto* current = res; current != nullptr; current = current->ai_next) {
                  self->results.push_back(sockaddr_to_endpoint(current->ai_addr));
                }
              }

              if (res != nullptr) {
                uv_freeaddrinfo(res);
              }

              self->complete();
            },
            node,
            service_name,
            &hints);

        if (rc < 0) {
          op_state->status = rc;
          op_state->complete();
        }
      });

      return true;
    }

    auto await_resume() -> std::vector<ip_endpoint> {
      if (shared_state->status == UV_ECANCELED ||
          (shared_state->cancel_requested && shared_state->status == 0)) {
        throw operation_cancelled();
      }
      if (shared_state->status < 0) {
        throw_uv_error(shared_state->status, "uv_getaddrinfo");
      }
      return std::move(shared_state->results);
    }
  };

  auto shared_state = std::make_shared<state>();
  shared_state->runtime = &runtime;
  shared_state->host = host;
  shared_state->service = service;
  shared_state->family = family;
  shared_state->socktype = socktype;
  shared_state->flags = flags;
  co_return co_await awaiter{std::move(shared_state), stop_token};
}

coro::task<name_info> reverse_lookup(uv_runtime& runtime,
                                     const ip_endpoint& endpoint,
                                     int flags,
                                     std::stop_token stop_token) {
  struct state {
    uv_runtime* runtime = nullptr;
    ip_endpoint endpoint;
    int flags;
    uv_getnameinfo_t req{};
    sockaddr_storage storage{};
    std::coroutine_handle<> continuation{};
    std::shared_ptr<state> keep_alive{};
    std::optional<stop_callback_t<std::function<void()>>> stop_callback{};
    std::atomic<bool> completed = false;
    int status = 0;
    name_info result{};
    bool cancel_requested = false;
    bool started = false;

    void complete() {
      if (completed.exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      auto h = continuation;
      continuation = {};
      if (h) {
        h.resume();
      }
      stop_callback.reset();
      keep_alive.reset();
    }
  };

  struct awaiter {
    std::shared_ptr<state> shared_state;
    std::stop_token stop_token;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) {
      if (stop_token.stop_requested()) {
        shared_state->cancel_requested = true;
        shared_state->status = UV_ECANCELED;
        return false;
      }

      shared_state->continuation = h;
      shared_state->keep_alive = shared_state;
      shared_state->req.data = shared_state.get();

      if (stop_token.stop_possible()) {
        shared_state->stop_callback.emplace(stop_token, std::function<void()>(
            [op_state = shared_state]() {
              op_state->runtime->post([op_state]() {
                if (op_state->completed.load(std::memory_order_acquire)) {
                  return;
                }
                op_state->cancel_requested = true;
                if (!op_state->started) {
                  op_state->status = UV_ECANCELED;
                  op_state->complete();
                  return;
                }
                const int rc =
                    uv_cancel(reinterpret_cast<uv_req_t*>(&op_state->req));
                if (rc < 0 && rc != UV_EBUSY && rc != UV_ENOSYS) {
                  op_state->status = UV_ECANCELED;
                  op_state->complete();
                }
              });
            }));
      }

      shared_state->runtime->post([op_state = shared_state]() {
        if (op_state->completed.load(std::memory_order_acquire)) {
          return;
        }

        const sockaddr* addr = endpoint_to_sockaddr(op_state->endpoint, op_state->storage);
        op_state->started = true;
        int rc = uv_getnameinfo(
            op_state->runtime->loop(),
            &op_state->req,
            [](uv_getnameinfo_t* req, int status, const char* hostname, const char* service) {
              auto* self = static_cast<state*>(req->data);
              self->status = status;

              if (status == 0) {
                self->result.host = hostname != nullptr ? hostname : "";
                self->result.service = service != nullptr ? service : "";
              }

              self->complete();
            },
            addr,
            op_state->flags);

        if (rc < 0) {
          op_state->status = rc;
          op_state->complete();
        }
      });

      return true;
    }

    auto await_resume() -> name_info {
      if (shared_state->status == UV_ECANCELED ||
          (shared_state->cancel_requested && shared_state->status == 0)) {
        throw operation_cancelled();
      }
      if (shared_state->status < 0) {
        throw_uv_error(shared_state->status, "uv_getnameinfo");
      }
      return std::move(shared_state->result);
    }
  };

  auto shared_state = std::make_shared<state>();
  shared_state->runtime = &runtime;
  shared_state->endpoint = endpoint;
  shared_state->flags = flags;
  co_return co_await awaiter{std::move(shared_state), stop_token};
}

}  // namespace luvcoro::dns
