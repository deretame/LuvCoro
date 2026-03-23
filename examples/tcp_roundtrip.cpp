#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <string>

#include <coro/coro.hpp>

#include "luvcoro/io.hpp"
#include "luvcoro/runtime.hpp"
#include "luvcoro/tcp.hpp"

namespace {

constexpr auto kTcpPort = 7101;
const std::string kPayload = "tcp roundtrip from luvcoro";
const luvcoro::ip_endpoint kServerEndpoint{"127.0.0.1", kTcpPort};
constexpr std::string_view kLineDelimiters[] = {"\r\n", "\n"};

coro::task<void> tcp_server_task() {
  luvcoro::tcp_server server;
  co_await server.bind(kServerEndpoint);
  co_await server.set_simultaneous_accepts(true);
  co_await server.listen();

  auto bound = server.local_endpoint();
  if (bound.port != kTcpPort) {
    throw std::runtime_error("tcp server bound unexpected port");
  }

  auto client = co_await luvcoro::accept_for(server, std::chrono::seconds(2));
  auto reader = luvcoro::io::make_stream_buffer(client);
  auto request = co_await luvcoro::io::read_exactly(reader, kPayload.size());
  if (request != kPayload) {
    throw std::runtime_error("tcp server received unexpected payload");
  }

  co_await luvcoro::io::write_all(client, "echo:" + request + "\n");
}

coro::task<void> tcp_client_task() {
  co_await luvcoro::sleep_for(std::chrono::milliseconds(25));

  luvcoro::tcp_client client;
  co_await luvcoro::connect_for(client, kServerEndpoint, std::chrono::seconds(2));
  co_await client.set_nodelay(true);
  co_await client.set_keepalive(true, 30);

  auto local = client.local_endpoint();
  auto remote = client.remote_endpoint();
  if (local.address.empty() || remote.port != kTcpPort) {
    throw std::runtime_error("tcp endpoints were not reported correctly");
  }

  auto reader = luvcoro::io::make_stream_buffer(client);
  co_await luvcoro::io::write_all(client, kPayload);

  auto reply = co_await luvcoro::io::read_until_any(reader, kLineDelimiters);
  if (reply != "echo:" + kPayload + "\n") {
    throw std::runtime_error("tcp client received unexpected reply");
  }
}

coro::task<void> run_tcp_roundtrip() {
  co_await coro::when_all(tcp_server_task(), tcp_client_task());
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_tcp_roundtrip());
    std::cout << "tcp roundtrip ok" << std::endl;
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
