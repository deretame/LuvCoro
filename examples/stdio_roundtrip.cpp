#include <iostream>
#include <string>

#include <coro/coro.hpp>

#include "luvcoro/luvcoro.hpp"

coro::task<void> run_stdio_roundtrip() {
  luvcoro::stdio_stream out;
  co_await out.open_stdout();

  std::string line = "stdio roundtrip ok: ";
  line += luvcoro::to_string(out.kind());
  line += "\n";

  co_await out.write(line);
}

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_stdio_roundtrip());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}
