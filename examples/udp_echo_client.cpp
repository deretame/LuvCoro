#include <chrono>
#include <iostream>
#include <string>

#include <coro/coro.hpp>

#include "luvcoro/runtime.hpp"
#include "luvcoro/udp.hpp"

coro::task<void> run_client() {
  luvcoro::udp_socket socket;
  co_await socket.bind("127.0.0.1", 7003);

  co_await luvcoro::sleep_for(std::chrono::milliseconds(25));

  co_await socket.send_to("127.0.0.1", 7002, "hello from udp client");
  auto reply = co_await socket.recv_from();
  std::cout << "reply: " << reply.data << std::endl;
}

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_client());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
