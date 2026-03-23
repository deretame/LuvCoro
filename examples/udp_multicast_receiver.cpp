#include <array>
#include <iostream>
#include <string>

#include <coro/coro.hpp>
#include <uv.h>

#include "luvcoro/buffer.hpp"
#include "luvcoro/io.hpp"
#include "luvcoro/runtime.hpp"
#include "luvcoro/udp.hpp"

namespace {

constexpr auto kMulticastPort = 7204;
constexpr const char* kMulticastGroup = "239.1.2.3";

coro::task<void> run_receiver() {
  luvcoro::udp_socket socket;
  std::array<char, 2048> packet{};

  co_await socket.bind("0.0.0.0", kMulticastPort, UV_UDP_REUSEADDR);
  co_await socket.join_multicast_group(kMulticastGroup);
  co_await socket.set_multicast_loop(true);

  std::cout << "udp multicast receiver listening on " << kMulticastGroup << ":" << kMulticastPort
            << std::endl;

  auto datagram = co_await luvcoro::io::receive_into(socket, luvcoro::buffer(packet));
  std::cout << "received from " << datagram.ip << ":" << datagram.port << " -> "
            << std::string(packet.data(), datagram.size) << std::endl;

  co_await socket.leave_multicast_group(kMulticastGroup);
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_receiver());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
