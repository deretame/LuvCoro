#include "luvcoro/tcp.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <span>
#include <stop_token>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "luvcoro/error.hpp"
#include "stream_ops.hpp"

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

auto sockaddr_to_endpoint(const sockaddr_storage& addr) -> ip_endpoint {
  char buffer[INET6_ADDRSTRLEN] = {};

  if (addr.ss_family == AF_INET) {
    const auto* addr4 = reinterpret_cast<const sockaddr_in*>(&addr);
    int rc = uv_ip4_name(addr4, buffer, sizeof(buffer));
    if (rc < 0) {
      throw_uv_error(rc, "uv_ip4_name");
    }
    return ip_endpoint{buffer, ntohs(addr4->sin_port)};
  }

  if (addr.ss_family == AF_INET6) {
    const auto* addr6 = reinterpret_cast<const sockaddr_in6*>(&addr);
    int rc = uv_ip6_name(addr6, buffer, sizeof(buffer));
    if (rc < 0) {
      throw_uv_error(rc, "uv_ip6_name");
    }
    return ip_endpoint{buffer, ntohs(addr6->sin6_port)};
  }

  throw std::runtime_error("unsupported socket address family");
}

}  // namespace

tcp_client::tcp_client() : tcp_client(current_runtime()) {}

tcp_client::tcp_client(uv_runtime& runtime) : runtime_(&runtime) {
  handle_ = runtime_->call([](uv_loop_t* loop) {
    auto* handle = new uv_tcp_t{};
    int rc = uv_tcp_init(loop, handle);
    if (rc < 0) {
      delete handle;
      throw_uv_error(rc, "uv_tcp_init");
    }
    return handle;
  });
}

tcp_client::tcp_client(uv_runtime& runtime, uv_tcp_t* handle, bool connected) noexcept
    : runtime_(&runtime), handle_(handle), connected_(connected) {}

tcp_client::~tcp_client() { close(); }

tcp_client::tcp_client(tcp_client&& other) noexcept
    : runtime_(other.runtime_), handle_(other.handle_), connected_(other.connected_) {
  other.runtime_ = nullptr;
  other.handle_ = nullptr;
  other.connected_ = false;
}

tcp_client& tcp_client::operator=(tcp_client&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  close();

  runtime_ = other.runtime_;
  handle_ = other.handle_;
  connected_ = other.connected_;

  other.runtime_ = nullptr;
  other.handle_ = nullptr;
  other.connected_ = false;

  return *this;
}

void tcp_client::close() {
  if (runtime_ == nullptr || handle_ == nullptr) {
    return;
  }

  uv_tcp_t* handle = handle_;
  handle_ = nullptr;
  connected_ = false;

  try {
    runtime_->post([handle]() {
      if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(handle))) {
        uv_close(
            reinterpret_cast<uv_handle_t*>(handle),
            [](uv_handle_t* h) { delete reinterpret_cast<uv_tcp_t*>(h); });
      }
    });
  } catch (...) {
    // Destructors must not throw.
  }
}

coro::task<void> tcp_client::connect(const std::string& ip,
                                     int port,
                                     std::stop_token stop_token) {
  co_await connect(ip_endpoint{ip, port}, stop_token);
}

coro::task<void> tcp_client::connect(const ip_endpoint& endpoint,
                                     std::stop_token stop_token) {
  if (handle_ == nullptr) {
    throw std::runtime_error("tcp handle is null");
  }
  if (connected_) {
    throw std::runtime_error("tcp client is already connected");
  }

  struct state {
    tcp_client* client = nullptr;
    ip_endpoint endpoint;
    uv_connect_t req{};
    sockaddr_storage addr{};
    std::coroutine_handle<> continuation{};
    std::shared_ptr<state> keep_alive{};
    std::optional<detail::stop_callback_t<std::function<void()>>> stop_callback{};
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

  struct connect_awaiter {
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
              op_state->client->runtime_->post([op_state]() {
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

      shared_state->client->runtime_->post([op_state = shared_state]() {
        if (op_state->completed.load(std::memory_order_acquire)) {
          return;
        }

        int rc = parse_ip_endpoint(op_state->endpoint, op_state->addr);
        if (rc < 0) {
          op_state->status = rc;
          op_state->complete();
          return;
        }

        op_state->started = true;
        rc = uv_tcp_connect(
            &op_state->req,
            op_state->client->handle_,
            reinterpret_cast<const sockaddr*>(&op_state->addr),
            [](uv_connect_t* req, int connect_status) {
              auto* self = static_cast<state*>(req->data);
              self->status = connect_status;
              self->complete();
            });

        if (rc < 0) {
          op_state->status = rc;
          op_state->complete();
        }
      });

      return true;
    }

    void await_resume() const {
      if (shared_state->status == UV_ECANCELED ||
          (shared_state->cancel_requested && shared_state->status == 0)) {
        throw operation_cancelled();
      }
      if (shared_state->status < 0) {
        throw_uv_error(shared_state->status, "uv_tcp_connect");
      }
    }
  };

  auto shared_state = std::make_shared<state>();
  shared_state->client = this;
  shared_state->endpoint = endpoint;
  co_await connect_awaiter{std::move(shared_state), stop_token};
  connected_ = true;
}

coro::task<std::size_t> tcp_client::write(std::string_view data,
                                          std::stop_token stop_token) {
  if (!connected_ || handle_ == nullptr) {
    throw std::runtime_error("tcp client is not connected");
  }

  co_return co_await detail::stream_write(
      *runtime_, handle_, std::string(data), stop_token);
}

coro::task<std::size_t> tcp_client::write_some(std::span<const char> data,
                                               std::stop_token stop_token) {
  if (!connected_ || handle_ == nullptr) {
    throw std::runtime_error("tcp client is not connected");
  }

  co_return co_await detail::stream_write_some(*runtime_, handle_, data, stop_token);
}

coro::task<std::string> tcp_client::read_some(std::size_t max_bytes,
                                              std::stop_token stop_token) {
  if (!connected_ || handle_ == nullptr) {
    throw std::runtime_error("tcp client is not connected");
  }

  co_return co_await detail::stream_read_some(*runtime_, handle_, max_bytes, stop_token);
}

coro::task<std::size_t> tcp_client::read_some_into(std::span<char> buffer,
                                                   std::stop_token stop_token) {
  if (!connected_ || handle_ == nullptr) {
    throw std::runtime_error("tcp client is not connected");
  }

  co_return co_await detail::stream_read_some_into(*runtime_, handle_, buffer, stop_token);
}

coro::task<std::string> tcp_client::read_exact(std::size_t total_bytes,
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

coro::task<std::size_t> tcp_client::read_exact_into(std::span<char> buffer,
                                                    std::stop_token stop_token) {
  std::size_t total = 0;

  while (total < buffer.size()) {
    if (stop_token.stop_requested()) {
      throw operation_cancelled();
    }
    auto chunk = co_await read_some_into(buffer.subspan(total), stop_token);
    if (chunk == 0) {
      break;
    }
    total += chunk;
  }

  co_return total;
}

coro::task<void> tcp_client::shutdown(std::stop_token stop_token) {
  if (!connected_ || handle_ == nullptr) {
    throw std::runtime_error("tcp client is not connected");
  }

  co_await detail::stream_shutdown(*runtime_, handle_, stop_token);
}

coro::task<void> tcp_client::set_nodelay(bool enabled, std::stop_token stop_token) {
  if (handle_ == nullptr) {
    throw std::runtime_error("tcp client handle is null");
  }
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  runtime_->call([this, enabled](uv_loop_t*) {
    const int rc = uv_tcp_nodelay(handle_, enabled ? 1 : 0);
    if (rc < 0) {
      throw_uv_error(rc, "uv_tcp_nodelay");
    }
  });
  co_return;
}

coro::task<void> tcp_client::set_keepalive(bool enabled,
                                           unsigned int delay,
                                           std::stop_token stop_token) {
  if (handle_ == nullptr) {
    throw std::runtime_error("tcp client handle is null");
  }
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  runtime_->call([this, enabled, delay](uv_loop_t*) {
    const int rc = uv_tcp_keepalive(handle_, enabled ? 1 : 0, delay);
    if (rc < 0) {
      throw_uv_error(rc, "uv_tcp_keepalive");
    }
  });
  co_return;
}

auto tcp_client::local_endpoint() const -> ip_endpoint {
  if (runtime_ == nullptr || handle_ == nullptr) {
    throw std::runtime_error("tcp client handle is null");
  }

  return runtime_->call([this](uv_loop_t*) {
    sockaddr_storage addr{};
    int len = sizeof(addr);
    const int rc =
        uv_tcp_getsockname(handle_, reinterpret_cast<sockaddr*>(&addr), &len);
    if (rc < 0) {
      throw_uv_error(rc, "uv_tcp_getsockname");
    }
    return sockaddr_to_endpoint(addr);
  });
}

auto tcp_client::remote_endpoint() const -> ip_endpoint {
  if (!connected_ || handle_ == nullptr || runtime_ == nullptr) {
    throw std::runtime_error("tcp client is not connected");
  }

  return runtime_->call([this](uv_loop_t*) {
    sockaddr_storage addr{};
    int len = sizeof(addr);
    const int rc =
        uv_tcp_getpeername(handle_, reinterpret_cast<sockaddr*>(&addr), &len);
    if (rc < 0) {
      throw_uv_error(rc, "uv_tcp_getpeername");
    }
    return sockaddr_to_endpoint(addr);
  });
}

tcp_server::tcp_server() : tcp_server(current_runtime()) {}

tcp_server::tcp_server(uv_runtime& runtime) : runtime_(&runtime) {
  handle_ = runtime_->call([this](uv_loop_t* loop) {
    auto* handle = new uv_tcp_t{};
    int rc = uv_tcp_init(loop, handle);
    if (rc < 0) {
      delete handle;
      throw_uv_error(rc, "uv_tcp_init");
    }
    handle->data = this;
    return handle;
  });
}

tcp_server::~tcp_server() { close(); }

void tcp_server::close() {
  if (runtime_ == nullptr || handle_ == nullptr) {
    return;
  }

  uv_tcp_t* server_handle = handle_;
  handle_ = nullptr;
  bound_ = false;
  listening_ = false;

  try {
    runtime_->call([this, server_handle](uv_loop_t*) {
      if (accept_waiter_) {
        accept_status_ = UV_ECANCELED;
        auto waiter = accept_waiter_;
        accept_waiter_ = {};
        waiter.resume();
      }

      for (auto* client : pending_clients_) {
        if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(client))) {
          uv_close(
              reinterpret_cast<uv_handle_t*>(client),
              [](uv_handle_t* h) { delete reinterpret_cast<uv_tcp_t*>(h); });
        }
      }
      pending_clients_.clear();

      if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(server_handle))) {
        uv_close(
            reinterpret_cast<uv_handle_t*>(server_handle),
            [](uv_handle_t* h) { delete reinterpret_cast<uv_tcp_t*>(h); });
      }
    });
  } catch (...) {
    // Destructors must not throw.
  }
}

coro::task<void> tcp_server::bind(const std::string& ip,
                                  int port,
                                  unsigned int flags,
                                  std::stop_token stop_token) {
  co_await bind(ip_endpoint{ip, port}, flags, stop_token);
}

coro::task<void> tcp_server::bind(const ip_endpoint& endpoint,
                                  unsigned int flags,
                                  std::stop_token stop_token) {
  if (handle_ == nullptr) {
    throw std::runtime_error("tcp server handle is null");
  }
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  runtime_->call([this, endpoint, flags](uv_loop_t*) {
    sockaddr_storage addr{};
    int rc = parse_ip_endpoint(endpoint, addr);
    if (rc < 0) {
      throw_uv_error(rc, "uv_ip*_addr");
    }

    rc = uv_tcp_bind(handle_, reinterpret_cast<const sockaddr*>(&addr), flags);
    if (rc < 0) {
      throw_uv_error(rc, "uv_tcp_bind");
    }

    bound_ = true;
  });

  co_return;
}

coro::task<void> tcp_server::set_simultaneous_accepts(bool enabled,
                                                      std::stop_token stop_token) {
  if (handle_ == nullptr) {
    throw std::runtime_error("tcp server handle is null");
  }
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  runtime_->call([this, enabled](uv_loop_t*) {
    const int rc = uv_tcp_simultaneous_accepts(handle_, enabled ? 1 : 0);
    if (rc < 0) {
      throw_uv_error(rc, "uv_tcp_simultaneous_accepts");
    }
  });
  co_return;
}

coro::task<void> tcp_server::listen(int backlog, std::stop_token stop_token) {
  if (handle_ == nullptr) {
    throw std::runtime_error("tcp server handle is null");
  }
  if (!bound_) {
    throw std::runtime_error("tcp server is not bound");
  }
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  runtime_->call([this, backlog](uv_loop_t*) {
    int rc = uv_listen(reinterpret_cast<uv_stream_t*>(handle_), backlog, &tcp_server::on_connection);
    if (rc < 0) {
      throw_uv_error(rc, "uv_listen");
    }

    listening_ = true;
  });

  co_return;
}

void tcp_server::on_connection(uv_stream_t* server, int status) {
  auto* self = static_cast<tcp_server*>(server->data);
  self->on_connection_impl(status);
}

void tcp_server::on_connection_impl(int status) {
  if (handle_ == nullptr) {
    return;
  }

  if (status < 0) {
    accept_status_ = status;
    if (accept_waiter_) {
      auto waiter = accept_waiter_;
      accept_waiter_ = {};
      waiter.resume();
    }
    return;
  }

  auto* client = new uv_tcp_t{};
  int rc = uv_tcp_init(runtime_->loop(), client);
  if (rc < 0) {
    delete client;
    accept_status_ = rc;
    if (accept_waiter_) {
      auto waiter = accept_waiter_;
      accept_waiter_ = {};
      waiter.resume();
    }
    return;
  }

  rc = uv_accept(
      reinterpret_cast<uv_stream_t*>(handle_),
      reinterpret_cast<uv_stream_t*>(client));
  if (rc < 0) {
    uv_close(
        reinterpret_cast<uv_handle_t*>(client),
        [](uv_handle_t* h) { delete reinterpret_cast<uv_tcp_t*>(h); });
    accept_status_ = rc;
    if (accept_waiter_) {
      auto waiter = accept_waiter_;
      accept_waiter_ = {};
      waiter.resume();
    }
    return;
  }

  pending_clients_.push_back(client);
  accept_status_ = 0;

  if (accept_waiter_) {
    auto waiter = accept_waiter_;
    accept_waiter_ = {};
    waiter.resume();
  }
}

coro::task<tcp_client> tcp_server::accept(std::stop_token stop_token) {
  if (!listening_ || handle_ == nullptr) {
    throw std::runtime_error("tcp server is not listening");
  }

  struct accept_awaiter {
    tcp_server& server;
    std::stop_token stop_token;
    std::coroutine_handle<> continuation{};
    std::optional<detail::stop_callback_t<std::function<void()>>> stop_callback{};
    int status = 0;

    bool await_ready() const noexcept {
      return stop_token.stop_requested() || !server.pending_clients_.empty() ||
             server.accept_status_ < 0;
    }

    bool await_suspend(std::coroutine_handle<> h) {
      continuation = h;
      if (stop_token.stop_requested()) {
        status = UV_ECANCELED;
        return false;
      }
      if (server.accept_waiter_) {
        status = UV_EBUSY;
        return false;
      }
      if (stop_token.stop_possible()) {
        stop_callback.emplace(stop_token, std::function<void()>(
            [this]() {
              server.runtime_->post([this]() {
                if (server.accept_waiter_ == continuation) {
                  server.accept_waiter_ = {};
                  server.accept_status_ = UV_ECANCELED;
                  auto waiter = continuation;
                  continuation = {};
                  if (waiter) {
                    waiter.resume();
                  }
                }
              });
            }));
      }
      server.accept_waiter_ = h;
      return true;
    }

    tcp_client await_resume() {
      stop_callback.reset();
      if (status == UV_ECANCELED || stop_token.stop_requested()) {
        throw operation_cancelled();
      }
      if (status < 0) {
        throw_uv_error(status, "tcp_server::accept");
      }

      if (server.accept_status_ < 0) {
        int rc = server.accept_status_;
        server.accept_status_ = 0;
        if (rc == UV_ECANCELED) {
          throw operation_cancelled();
        }
        throw_uv_error(rc, "uv_accept");
      }

      if (server.pending_clients_.empty()) {
        throw std::runtime_error("accept resumed without pending client");
      }

      uv_tcp_t* client_handle = server.pending_clients_.front();
      server.pending_clients_.pop_front();
      return tcp_client(*server.runtime_, client_handle, true);
    }
  };

  co_return co_await accept_awaiter{*this, stop_token};
}

auto tcp_server::local_endpoint() const -> ip_endpoint {
  if (runtime_ == nullptr || handle_ == nullptr) {
    throw std::runtime_error("tcp server handle is null");
  }

  return runtime_->call([this](uv_loop_t*) {
    sockaddr_storage addr{};
    int len = sizeof(addr);
    const int rc =
        uv_tcp_getsockname(handle_, reinterpret_cast<sockaddr*>(&addr), &len);
    if (rc < 0) {
      throw_uv_error(rc, "uv_tcp_getsockname");
    }
    return sockaddr_to_endpoint(addr);
  });
}

}  // namespace luvcoro
