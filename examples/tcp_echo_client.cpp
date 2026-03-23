#include <chrono>
#include <iostream>
#include <string>

#include <coro/coro.hpp>

#include "luvcoro/io.hpp"
#include "luvcoro/runtime.hpp"
#include "luvcoro/tcp.hpp"

coro::task<void> run_echo() {
  luvcoro::tcp_client client;
  auto reader = luvcoro::io::make_stream_buffer(client);

  co_await client.connect("127.0.0.1", 7001);
  co_await luvcoro::io::write_all(client, "hello from luvcoro\\n");

  auto reply = co_await luvcoro::io::read_line(reader);
  std::cout << "echo: " << reply << std::endl;

  co_await luvcoro::sleep_for(std::chrono::milliseconds(50));
}

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_echo());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
