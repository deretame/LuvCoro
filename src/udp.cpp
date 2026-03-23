#include "luvcoro/udp.hpp"

#include <algorithm>
#include <atomic>
#include <coroutine>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "luvcoro/error.hpp"

namespace luvcoro {
namespace {

int parse_ip_endpoint(const ip_endpoint& endpoint, sockaddr_storage& out) {
  sockaddr_in addr4{};
  int rc = uv_ip4_addr(endpoint.address.c_str(), endpoint.port, &addr4);
  if (rc == 0) {
    std::memcpy(&out, &addr4, sizeof(addr4));
    return 0;
  }

  sockaddr_in6 addr6{};
  rc = uv_ip6_addr(endpoint.address.c_str(), endpoint.port, &addr6);
  if (rc == 0) {
    std::memcpy(&out, &addr6, sizeof(addr6));
    return 0;
  }

  return rc;
}

udp_receive_result make_receive_result(std::size_t nread, const sockaddr_storage& addr) {
  udp_receive_result out{};
  out.size = nread;
  char ipbuf[INET6_ADDRSTRLEN] = {};

  if (addr.ss_family == AF_INET) {
    const auto* a = reinterpret_cast<const sockaddr_in*>(&addr);
    int rc = uv_ip4_name(a, ipbuf, sizeof(ipbuf));
    if (rc < 0) {
      throw_uv_error(rc, "uv_ip4_name");
    }
    out.ip = ipbuf;
    out.port = ntohs(a->sin_port);
    out.ipv6 = false;
    out.peer = ip_endpoint{out.ip, out.port, false};
    return out;
  }

  if (addr.ss_family == AF_INET6) {
    const auto* a = reinterpret_cast<const sockaddr_in6*>(&addr);
    int rc = uv_ip6_name(a, ipbuf, sizeof(ipbuf));
    if (rc < 0) {
      throw_uv_error(rc, "uv_ip6_name");
    }
    out.ip = ipbuf;
    out.port = ntohs(a->sin6_port);
    out.ipv6 = true;
    out.peer = ip_endpoint{out.ip, out.port, true};
    return out;
  }

  throw std::runtime_error("unsupported udp address family");
}

udp_datagram make_datagram(const char* buffer, const udp_receive_result& info) {
  udp_datagram out{};
  out.data = std::string(buffer, info.size);
  out.peer = info.peer;
  out.ip = info.ip;
  out.port = info.port;
  out.ipv6 = info.ipv6;
  return out;
}

void ensure_udp_handle(uv_udp_t* handle) {
  if (handle == nullptr) {
    throw std::runtime_error("udp socket handle is null");
  }
}

template <typename Fn>
using stop_callback_t = std::stop_callback<std::decay_t<Fn>>;

}  // namespace

udp_socket::udp_socket() : udp_socket(current_runtime()) {}

udp_socket::udp_socket(uv_runtime& runtime) : runtime_(&runtime) {
  handle_ = runtime_->call([](uv_loop_t* loop) {
    auto* handle = new uv_udp_t{};
    int rc = uv_udp_init(loop, handle);
    if (rc < 0) {
      delete handle;
      throw_uv_error(rc, "uv_udp_init");
    }
    return handle;
  });
}

udp_socket::~udp_socket() { close(); }

udp_socket::udp_socket(udp_socket&& other) noexcept
    : runtime_(other.runtime_), handle_(other.handle_), bound_(other.bound_) {
  other.runtime_ = nullptr;
  other.handle_ = nullptr;
  other.bound_ = false;
}

udp_socket& udp_socket::operator=(udp_socket&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  close();

  runtime_ = other.runtime_;
  handle_ = other.handle_;
  bound_ = other.bound_;

  other.runtime_ = nullptr;
  other.handle_ = nullptr;
  other.bound_ = false;

  return *this;
}

void udp_socket::close() {
  if (runtime_ == nullptr || handle_ == nullptr) {
    return;
  }

  uv_udp_t* handle = handle_;
  handle_ = nullptr;
  bound_ = false;

  try {
    runtime_->post([handle]() {
      uv_udp_recv_stop(handle);
      if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(handle))) {
        uv_close(
            reinterpret_cast<uv_handle_t*>(handle),
            [](uv_handle_t* h) { delete reinterpret_cast<uv_udp_t*>(h); });
      }
    });
  } catch (...) {
    // Destructors must not throw.
  }
}

coro::task<void> udp_socket::bind(const std::string& ip,
                                  int port,
                                  unsigned int flags,
                                  std::stop_token stop_token) {
  co_await bind(ip_endpoint{ip, port}, flags, stop_token);
}

coro::task<void> udp_socket::bind(const ip_endpoint& endpoint,
                                  unsigned int flags,
                                  std::stop_token stop_token) {
  ensure_udp_handle(handle_);
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  runtime_->call([this, endpoint, flags](uv_loop_t*) {
    sockaddr_storage addr{};
    int rc = parse_ip_endpoint(endpoint, addr);
    if (rc < 0) {
      throw_uv_error(rc, "uv_ip*_addr");
    }

    rc = uv_udp_bind(handle_, reinterpret_cast<const sockaddr*>(&addr), flags);
    if (rc < 0) {
      throw_uv_error(rc, "uv_udp_bind");
    }

    bound_ = true;
  });

  co_return;
}

coro::task<void> udp_socket::set_broadcast(bool enabled, std::stop_token stop_token) {
  ensure_udp_handle(handle_);
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  runtime_->call([this, enabled](uv_loop_t*) {
    int rc = uv_udp_set_broadcast(handle_, enabled ? 1 : 0);
    if (rc < 0) {
      throw_uv_error(rc, "uv_udp_set_broadcast");
    }
  });

  co_return;
}

coro::task<void> udp_socket::set_multicast_loop(bool enabled, std::stop_token stop_token) {
  ensure_udp_handle(handle_);
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  runtime_->call([this, enabled](uv_loop_t*) {
    int rc = uv_udp_set_multicast_loop(handle_, enabled ? 1 : 0);
    if (rc < 0) {
      throw_uv_error(rc, "uv_udp_set_multicast_loop");
    }
  });

  co_return;
}

coro::task<void> udp_socket::set_multicast_ttl(int ttl, std::stop_token stop_token) {
  ensure_udp_handle(handle_);
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  runtime_->call([this, ttl](uv_loop_t*) {
    int rc = uv_udp_set_multicast_ttl(handle_, ttl);
    if (rc < 0) {
      throw_uv_error(rc, "uv_udp_set_multicast_ttl");
    }
  });

  co_return;
}

coro::task<void> udp_socket::set_multicast_interface(const std::string& interface_address,
                                                     std::stop_token stop_token) {
  ensure_udp_handle(handle_);
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  runtime_->call([this, interface_address](uv_loop_t*) {
    const char* iface = interface_address.empty() ? nullptr : interface_address.c_str();
    int rc = uv_udp_set_multicast_interface(handle_, iface);
    if (rc < 0) {
      throw_uv_error(rc, "uv_udp_set_multicast_interface");
    }
  });

  co_return;
}

coro::task<void> udp_socket::join_multicast_group(const std::string& multicast_address,
                                                  const std::string& interface_address,
                                                  std::stop_token stop_token) {
  ensure_udp_handle(handle_);
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  runtime_->call([this, multicast_address, interface_address](uv_loop_t*) {
    const char* iface = interface_address.empty() ? nullptr : interface_address.c_str();
    int rc = uv_udp_set_membership(handle_,
                                   multicast_address.c_str(),
                                   iface,
                                   UV_JOIN_GROUP);
    if (rc < 0) {
      throw_uv_error(rc, "uv_udp_set_membership");
    }
  });

  co_return;
}

coro::task<void> udp_socket::leave_multicast_group(const std::string& multicast_address,
                                                   const std::string& interface_address,
                                                   std::stop_token stop_token) {
  ensure_udp_handle(handle_);
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  runtime_->call([this, multicast_address, interface_address](uv_loop_t*) {
    const char* iface = interface_address.empty() ? nullptr : interface_address.c_str();
    int rc = uv_udp_set_membership(handle_,
                                   multicast_address.c_str(),
                                   iface,
                                   UV_LEAVE_GROUP);
    if (rc < 0) {
      throw_uv_error(rc, "uv_udp_set_membership");
    }
  });

  co_return;
}

coro::task<std::size_t> udp_socket::send_to(const std::string& ip,
                                            int port,
                                            std::string_view data,
                                            std::stop_token stop_token) {
  co_return co_await send_to(ip_endpoint{ip, port}, data, stop_token);
}

coro::task<std::size_t> udp_socket::send_to(const ip_endpoint& endpoint,
                                            std::string_view data,
                                            std::stop_token stop_token) {
  ensure_udp_handle(handle_);

  if (data.empty()) {
    co_return 0;
  }

  struct state {
    udp_socket* socket = nullptr;
    ip_endpoint endpoint;
    std::string data;
    sockaddr_storage addr{};
    uv_udp_send_t req{};
    std::coroutine_handle<> continuation{};
    std::shared_ptr<state> keep_alive{};
    std::optional<stop_callback_t<std::function<void()>>> stop_callback{};
    std::atomic<bool> completed = false;
    int status = 0;
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

  struct send_awaiter {
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
              op_state->socket->runtime_->post([op_state]() {
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

      shared_state->socket->runtime_->post([op_state = shared_state]() {
        if (op_state->completed.load(std::memory_order_acquire)) {
          return;
        }

        int rc = parse_ip_endpoint(op_state->endpoint, op_state->addr);
        if (rc < 0) {
          op_state->status = rc;
          op_state->complete();
          return;
        }

        auto buf = uv_buf_init(op_state->data.data(),
                               static_cast<unsigned int>(op_state->data.size()));
        op_state->started = true;
        rc = uv_udp_send(
            &op_state->req,
            op_state->socket->handle_,
            &buf,
            1,
            reinterpret_cast<const sockaddr*>(&op_state->addr),
            [](uv_udp_send_t* req, int send_status) {
              auto* self = static_cast<state*>(req->data);
              self->status = send_status;
              self->complete();
            });

        if (rc < 0) {
          op_state->status = rc;
          op_state->complete();
        }
      });

      return true;
    }

    std::size_t await_resume() const {
      if (shared_state->status == UV_ECANCELED ||
          (shared_state->cancel_requested && shared_state->status == 0)) {
        throw operation_cancelled();
      }
      if (shared_state->status < 0) {
        throw_uv_error(shared_state->status, "uv_udp_send");
      }
      return shared_state->data.size();
    }
  };

  auto shared_state = std::make_shared<state>();
  shared_state->socket = this;
  shared_state->endpoint = endpoint;
  shared_state->data = std::string(data);
  co_return co_await send_awaiter{std::move(shared_state), stop_token};
}

coro::task<std::size_t> udp_socket::send_some_to(const ip_endpoint& endpoint,
                                                 std::span<const char> data,
                                                 std::stop_token stop_token) {
  ensure_udp_handle(handle_);

  if (data.empty()) {
    co_return 0;
  }

  co_return co_await send_to(
      endpoint, std::string_view(data.data(), data.size()), stop_token);
}

coro::task<udp_datagram> udp_socket::recv_from(std::size_t max_bytes,
                                               std::stop_token stop_token) {
  ensure_udp_handle(handle_);
  if (!bound_) {
    throw std::runtime_error("udp socket must be bound before recv_from");
  }
  if (max_bytes == 0) {
    throw std::runtime_error("recv_from max_bytes must be > 0");
  }

  std::vector<char> buffer(max_bytes);
  auto result = co_await recv_from_into(
      std::span<char>(buffer.data(), buffer.size()), stop_token);
  co_return make_datagram(buffer.data(), result);
}

coro::task<udp_receive_result> udp_socket::recv_from_into(std::span<char> buffer,
                                                          std::stop_token stop_token) {
  ensure_udp_handle(handle_);
  if (!bound_) {
    throw std::runtime_error("udp socket must be bound before recv_from");
  }
  if (buffer.empty()) {
    throw std::runtime_error("recv_from_into buffer must not be empty");
  }

  struct state {
    udp_socket* socket = nullptr;
    std::span<char> buffer;
    sockaddr_storage addr{};
    std::coroutine_handle<> continuation{};
    std::shared_ptr<state> keep_alive{};
    std::optional<stop_callback_t<std::function<void()>>> stop_callback{};
    std::atomic<bool> completed = false;
    int status = 0;
    std::size_t nread = 0;
    bool have_addr = false;
    bool cancel_requested = false;

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

  struct recv_awaiter {
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

      if (stop_token.stop_possible()) {
        shared_state->stop_callback.emplace(stop_token, std::function<void()>(
            [op_state = shared_state]() {
              op_state->socket->runtime_->post([op_state]() {
                if (op_state->completed.load(std::memory_order_acquire)) {
                  return;
                }
                op_state->cancel_requested = true;
                if (op_state->socket->handle_->data == op_state.get()) {
                  uv_udp_recv_stop(op_state->socket->handle_);
                  op_state->socket->handle_->data = nullptr;
                }
                op_state->status = UV_ECANCELED;
                op_state->complete();
              });
            }));
      }

      shared_state->socket->runtime_->post([op_state = shared_state]() {
        if (op_state->completed.load(std::memory_order_acquire)) {
          return;
        }

        if (op_state->socket->handle_->data != nullptr) {
          op_state->status = UV_EBUSY;
          op_state->complete();
          return;
        }

        op_state->socket->handle_->data = op_state.get();

        int rc = uv_udp_recv_start(
            op_state->socket->handle_,
            [](uv_handle_t* handle, size_t, uv_buf_t* buf) {
              auto* self = static_cast<state*>(handle->data);
              buf->base = self->buffer.data();
              buf->len = static_cast<unsigned int>(self->buffer.size());
            },
            [](uv_udp_t* handle, ssize_t nread, const uv_buf_t*, const sockaddr* addr, unsigned) {
              if (nread == 0) {
                return;
              }

              auto* self = static_cast<state*>(handle->data);
              if (self == nullptr) {
                return;
              }

              if (nread < 0) {
                self->status = static_cast<int>(nread);
                self->nread = 0;
              } else {
                self->status = 0;
                self->nread = static_cast<std::size_t>(nread);
                if (addr != nullptr) {
                  std::memcpy(&self->addr, addr, sizeof(sockaddr_storage));
                  self->have_addr = true;
                }
              }

              uv_udp_recv_stop(handle);
              handle->data = nullptr;
              self->complete();
            });

        if (rc < 0) {
          op_state->socket->handle_->data = nullptr;
          op_state->status = rc;
          op_state->complete();
        }
      });

      return true;
    }

    udp_receive_result await_resume() const {
      if (shared_state->status == UV_ECANCELED ||
          (shared_state->cancel_requested && shared_state->status == 0)) {
        throw operation_cancelled();
      }
      if (shared_state->status < 0) {
        throw_uv_error(shared_state->status, "uv_udp_recv_start");
      }
      if (!shared_state->have_addr) {
        throw std::runtime_error("udp recv_from missing peer address");
      }
      return make_receive_result(shared_state->nread, shared_state->addr);
    }
  };

  auto shared_state = std::make_shared<state>();
  shared_state->socket = this;
  shared_state->buffer = buffer;
  co_return co_await recv_awaiter{std::move(shared_state), stop_token};
}

}  // namespace luvcoro
