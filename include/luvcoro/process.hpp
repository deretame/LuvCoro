#pragma once

#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

#include <coro/coro.hpp>

#include "luvcoro/pipe.hpp"
#include "luvcoro/runtime.hpp"

namespace luvcoro {

struct process_stdio_options {
  bool stdin_pipe = false;
  bool stdout_pipe = false;
  bool stderr_pipe = false;
};

struct process_options {
  std::string file;
  std::vector<std::string> args{};
  std::vector<std::string> env{};
  std::string cwd{};
  bool detached = false;
  bool windows_hide = false;
  bool windows_hide_console = false;
  bool windows_verbatim_arguments = false;
  process_stdio_options stdio{};
};

struct process_exit_status {
  std::int64_t exit_status = 0;
  int term_signal = 0;
};

class process {
 public:
  process();
  explicit process(uv_runtime& runtime);
  ~process();

  process(const process&) = delete;
  process& operator=(const process&) = delete;
  process(process&&) = delete;
  process& operator=(process&&) = delete;

  coro::task<void> spawn(const process_options& options, std::stop_token stop_token = {});
  coro::task<process_exit_status> wait(std::stop_token stop_token = {});
  void kill(int signum = 15);
  void close();

  auto running() const noexcept -> bool;
  auto pid() const noexcept -> int;

  auto stdin_pipe() noexcept -> pipe_client* { return stdin_pipe_.get(); }
  auto stdout_pipe() noexcept -> pipe_client* { return stdout_pipe_.get(); }
  auto stderr_pipe() noexcept -> pipe_client* { return stderr_pipe_.get(); }

  auto stdin_pipe() const noexcept -> const pipe_client* { return stdin_pipe_.get(); }
  auto stdout_pipe() const noexcept -> const pipe_client* { return stdout_pipe_.get(); }
  auto stderr_pipe() const noexcept -> const pipe_client* { return stderr_pipe_.get(); }

 private:
  struct state;

  uv_runtime* runtime_ = nullptr;
  std::shared_ptr<state> state_{};
  std::unique_ptr<pipe_client> stdin_pipe_{};
  std::unique_ptr<pipe_client> stdout_pipe_{};
  std::unique_ptr<pipe_client> stderr_pipe_{};
};

}  // namespace luvcoro
