#include <iostream>
#include <string>
#include <string_view>

#include <coro/coro.hpp>

#include "luvcoro/io.hpp"
#include "luvcoro/runtime.hpp"
#include "luvcoro/tcp.hpp"

constexpr std::string_view kLineDelimiters[] = {"\r\n", "\n"};

coro::task<void> run_server() {
  luvcoro::tcp_server server;

  co_await server.bind("127.0.0.1", 7001);
  co_await server.listen();

  std::cout << "tcp echo server listening on 127.0.0.1:7001" << std::endl;

  auto client = co_await server.accept();
  auto reader = luvcoro::io::make_stream_buffer(client);
  auto result = co_await luvcoro::io::read_until_any_result(reader, kLineDelimiters);
  std::cout << "received delimiter[" << result.delimiter_index << "]: " << result.data << std::endl;
  co_await luvcoro::io::write_all(client, result.data);
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
