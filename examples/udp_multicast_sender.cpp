#include <chrono>
#include <iostream>
#include <string>

#include <coro/coro.hpp>

#include "luvcoro/io.hpp"
#include "luvcoro/runtime.hpp"
#include "luvcoro/udp.hpp"

namespace {

constexpr auto kMulticastPort = 7204;
constexpr const char* kMulticastGroup = "239.1.2.3";
const luvcoro::ip_endpoint kMulticastEndpoint{kMulticastGroup, kMulticastPort};

coro::task<void> run_sender() {
  luvcoro::udp_socket socket;
  const std::string payload = "hello from multicast sender";

  co_await socket.bind("0.0.0.0", 0);
  co_await socket.set_multicast_ttl(8);
  co_await socket.set_multicast_loop(true);

  co_await luvcoro::sleep_for(std::chrono::milliseconds(25));
  co_await luvcoro::io::send_some_to(socket, kMulticastEndpoint, luvcoro::buffer(payload));

  std::cout << "sent to " << kMulticastGroup << ":" << kMulticastPort << " -> " << payload
            << std::endl;
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_sender());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
