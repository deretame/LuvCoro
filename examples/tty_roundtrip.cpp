#include <iostream>
#include <stdexcept>

#include "luvcoro/runtime.hpp"
#include "luvcoro/tty.hpp"

namespace {

coro::task<void> run_tty_roundtrip() {
  if (!luvcoro::is_tty(1)) {
    std::cout << "tty roundtrip skipped: stdout is not a tty" << std::endl;
    co_return;
  }

  luvcoro::tty_stream tty;
  co_await tty.open_stdout();

  auto size = tty.winsize();
  auto line = "tty winsize: " + std::to_string(size.width) + "x" + std::to_string(size.height) +
              "\n";
  auto written = co_await tty.write(line);
  if (written != line.size()) {
    throw std::runtime_error("tty write wrote an unexpected byte count");
  }

  co_await tty.close();
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_tty_roundtrip());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
