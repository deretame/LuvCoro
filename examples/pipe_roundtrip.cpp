#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include <coro/coro.hpp>

#include "luvcoro/io.hpp"
#include "luvcoro/pipe.hpp"
#include "luvcoro/runtime.hpp"

using namespace std::chrono_literals;

namespace {

auto test_pipe_name() -> std::string {
#ifdef _WIN32
  return R"(\\.\pipe\luvcoro_pipe_roundtrip)";
#else
  return "luvcoro_pipe_roundtrip.sock";
#endif
}

coro::task<void> pipe_server_task(const std::string& name) {
  luvcoro::pipe_server server;
  co_await server.bind(name);
  co_await server.set_pending_instances(4);
  co_await server.listen();

  if (server.local_name().empty()) {
    throw std::runtime_error("pipe server did not report a local name");
  }

  auto client = co_await luvcoro::accept_for(server, 2s);
  auto reader = luvcoro::io::make_stream_buffer(client);
  auto request = co_await luvcoro::io::read_until_for(reader, "\n", 2s);
  if (request != "ping\n") {
    throw std::runtime_error("pipe server received unexpected payload");
  }

  co_await client.write("pong\n");
  co_await client.shutdown();
}

coro::task<void> pipe_client_task(const std::string& name) {
  co_await luvcoro::sleep_for(25ms);

  luvcoro::pipe_client client;
  co_await luvcoro::connect_for(client, name, 2s);
  if (client.peer_name().empty()) {
    throw std::runtime_error("pipe client did not report a peer name");
  }
  co_await client.write("ping\n");

  auto reader = luvcoro::io::make_stream_buffer(client);
  auto response = co_await luvcoro::io::read_until_for(reader, "\n", 2s);
  if (response != "pong\n") {
    throw std::runtime_error("pipe client received unexpected response");
  }
}

coro::task<void> run_pipe_roundtrip() {
#ifndef _WIN32
  std::filesystem::remove(test_pipe_name());
#endif

  const auto name = test_pipe_name();
  co_await coro::when_all(pipe_server_task(name), pipe_client_task(name));

#ifndef _WIN32
  std::filesystem::remove(name);
#endif
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_pipe_roundtrip());
    std::cout << "pipe roundtrip ok" << std::endl;
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
