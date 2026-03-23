#include <iostream>

#include <coro/coro.hpp>

#include "luvcoro/runtime.hpp"
#include "luvcoro/udp.hpp"

coro::task<void> run_server() {
  luvcoro::udp_socket socket;
  co_await socket.bind("127.0.0.1", 7002);

  std::cout << "udp echo server listening on 127.0.0.1:7002" << std::endl;

  auto datagram = co_await socket.recv_from();
  std::cout << "received from " << datagram.ip << ":" << datagram.port << " -> " << datagram.data
            << std::endl;
  co_await socket.send_to(datagram.ip, datagram.port, datagram.data);
}

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_server());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
