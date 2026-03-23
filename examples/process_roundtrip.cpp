#include <algorithm>
#include <chrono>
#include <cctype>
#include <iostream>
#include <stdexcept>
#include <string>

#include <coro/coro.hpp>

#include "luvcoro/io.hpp"
#include "luvcoro/process.hpp"
#include "luvcoro/runtime.hpp"

using namespace std::chrono_literals;

namespace {

auto run_child() -> int {
  std::string line;
  if (!std::getline(std::cin, line)) {
    std::cerr << "child failed to read stdin" << std::endl;
    return 2;
  }

  std::transform(line.begin(),
                 line.end(),
                 line.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });

  std::cout << line << std::endl;
  return 0;
}

coro::task<void> run_process_roundtrip(const std::string& self_path) {
  luvcoro::process proc;
  luvcoro::process_options options;
  options.file = self_path;
  options.args = {"--child-upper"};
  options.stdio.stdin_pipe = true;
  options.stdio.stdout_pipe = true;
  options.stdio.stderr_pipe = true;

  co_await proc.spawn(options);

  if (proc.stdin_pipe() == nullptr || proc.stdout_pipe() == nullptr ||
      proc.stderr_pipe() == nullptr) {
    throw std::runtime_error("process pipes were not created");
  }

  co_await luvcoro::sleep_for(50ms);

  if (!proc.running()) {
    auto exit_status = co_await proc.wait();
    auto stderr_text = co_await proc.stderr_pipe()->read_some(1024);
    throw std::runtime_error(
        "child exited before stdin write, exit=" + std::to_string(exit_status.exit_status) +
        ", stderr=" + stderr_text);
  }

  co_await proc.stdin_pipe()->write("hello process\n");
  co_await proc.stdin_pipe()->shutdown();

  auto reader = luvcoro::io::make_stream_buffer(*proc.stdout_pipe());
  auto line = co_await luvcoro::io::read_line(reader, false, {});
  if (line != "HELLO PROCESS") {
    throw std::runtime_error("process stdout returned unexpected payload");
  }

  auto exit_status = co_await proc.wait();
  if (exit_status.exit_status != 0 || exit_status.term_signal != 0) {
    throw std::runtime_error("child process exited unexpectedly");
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc > 1 && std::string_view(argv[1]) == "--child-upper") {
      return run_child();
    }

    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_process_roundtrip(argv[0]));
    std::cout << "process roundtrip ok" << std::endl;
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
