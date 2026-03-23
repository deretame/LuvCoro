#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

#include <coro/coro.hpp>

#include "luvcoro/io.hpp"
#include "luvcoro/runtime.hpp"
#include "luvcoro/udp.hpp"

namespace {

constexpr auto kServerPort = 7102;
constexpr auto kClientPort = 7103;
const std::string kPayload = "udp roundtrip from luvcoro";
const luvcoro::ip_endpoint kServerEndpoint{"127.0.0.1", kServerPort};
const luvcoro::ip_endpoint kClientEndpoint{"127.0.0.1", kClientPort};

coro::task<void> udp_server_task() {
  luvcoro::udp_socket socket;
  co_await socket.bind(kServerEndpoint);
  std::array<char, 1024> buffer{};

  auto datagram = co_await luvcoro::io::receive_into(socket, luvcoro::buffer(buffer));
  const std::string payload(buffer.data(), datagram.size);
  if (payload != kPayload) {
    throw std::runtime_error("udp server received unexpected payload");
  }

  const std::string reply = "echo:" + payload;
  co_await luvcoro::io::send_some_to(socket, datagram.peer, luvcoro::buffer(reply));
}

coro::task<void> udp_client_task() {
  luvcoro::udp_socket socket;
  co_await socket.bind(kClientEndpoint);
  std::array<char, 1024> buffer{};

  co_await luvcoro::sleep_for(std::chrono::milliseconds(25));
  co_await luvcoro::io::send_some_to(socket, kServerEndpoint, luvcoro::buffer(kPayload));

  auto reply = co_await luvcoro::io::receive_into(socket, luvcoro::buffer(buffer));
  if (std::string(buffer.data(), reply.size) != "echo:" + kPayload) {
    throw std::runtime_error("udp client received unexpected reply");
  }
}

coro::task<void> run_udp_roundtrip() {
  co_await coro::when_all(udp_server_task(), udp_client_task());
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_udp_roundtrip());
    std::cout << "udp roundtrip ok" << std::endl;
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
