#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <thread>

#include <coro/coro.hpp>

#include "luvcoro/runtime.hpp"
#include "luvcoro/task_scope.hpp"
#include "luvcoro/tcp.hpp"
#include "luvcoro/udp.hpp"

namespace {

coro::task<void> wait_accept_cancel(luvcoro::tcp_server& server,
                                    std::atomic<bool>& cancelled,
                                    std::stop_token stop_token) {
  try {
    (void)co_await server.accept(stop_token);
    throw std::runtime_error("tcp accept unexpectedly completed");
  } catch (const luvcoro::operation_cancelled&) {
    cancelled.store(true, std::memory_order_relaxed);
  }
}

coro::task<void> wait_udp_cancel(luvcoro::udp_socket& socket,
                                 std::atomic<bool>& cancelled,
                                 std::stop_token stop_token) {
  try {
    (void)co_await socket.recv_from(1024, stop_token);
    throw std::runtime_error("udp recv unexpectedly completed");
  } catch (const luvcoro::operation_cancelled&) {
    cancelled.store(true, std::memory_order_relaxed);
  }
}

coro::task<void> wait_sleep_cancel(std::atomic<bool>& cancelled,
                                   std::stop_token stop_token) {
  using namespace std::chrono_literals;

  try {
    co_await luvcoro::sleep_for(1s, stop_token);
    throw std::runtime_error("sleep unexpectedly completed");
  } catch (const luvcoro::operation_cancelled&) {
    cancelled.store(true, std::memory_order_relaxed);
  }
}

coro::task<void> run_io_cancel() {
  using namespace std::chrono_literals;

  luvcoro::tcp_server server;
  co_await server.bind("127.0.0.1", 0);
  co_await server.listen();

  luvcoro::udp_socket socket;
  co_await socket.bind("127.0.0.1", 0);

  luvcoro::task_scope scope;
  std::stop_source stop_source;

  std::atomic<bool> accept_cancelled = false;
  std::atomic<bool> recv_cancelled = false;
  std::atomic<bool> sleep_cancelled = false;
  bool join_cancelled = false;

  scope.spawn([&](std::stop_token child_stop) {
    return wait_accept_cancel(server, accept_cancelled, child_stop);
  });

  scope.spawn([&](std::stop_token child_stop) {
    return wait_udp_cancel(socket, recv_cancelled, child_stop);
  });

  scope.spawn([&](std::stop_token child_stop) {
    return wait_sleep_cancel(sleep_cancelled, child_stop);
  });

  std::jthread canceller([&stop_source]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    stop_source.request_stop();
  });

  try {
    co_await scope.join(stop_source.get_token());
  } catch (const luvcoro::operation_cancelled&) {
    join_cancelled = true;
  }

  if (!join_cancelled || !accept_cancelled.load(std::memory_order_relaxed) ||
      !recv_cancelled.load(std::memory_order_relaxed) ||
      !sleep_cancelled.load(std::memory_order_relaxed)) {
    throw std::runtime_error("unified cancellation did not stop all pending operations");
  }

  std::cout << "io cancellation ok" << std::endl;
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_io_cancel());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
