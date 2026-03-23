#include "luvcoro/process.hpp"

#include <atomic>
#include <csignal>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <uv.h>

#include "luvcoro/error.hpp"

namespace luvcoro {
namespace {

template <typename Fn>
using stop_callback_t = std::stop_callback<std::decay_t<Fn>>;

}  // namespace

struct process::state {
  explicit state(uv_runtime& runtime) : runtime(&runtime) {}

  uv_runtime* runtime = nullptr;
  uv_process_t* handle = nullptr;
  std::shared_ptr<state> keep_alive{};
  std::coroutine_handle<> waiter{};
  std::atomic<bool> exited = false;
  int wait_status = 0;
  std::int64_t exit_status = 0;
  int term_signal = 0;
  int pid = 0;
  bool running = false;
};

process::process() : process(current_runtime()) {}

process::process(uv_runtime& runtime)
    : runtime_(&runtime),
      state_(std::make_shared<state>(runtime)) {}

process::~process() { close(); }

coro::task<void> process::spawn(const process_options& options,
                                std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }
  if (options.file.empty()) {
    throw std::invalid_argument("process file must not be empty");
  }
  if (state_->running) {
    throw std::runtime_error("process is already running");
  }
  if (state_->handle != nullptr) {
    close();
    state_ = std::make_shared<state>(*runtime_);
  }

  stdin_pipe_.reset();
  stdout_pipe_.reset();
  stderr_pipe_.reset();

  if (options.stdio.stdin_pipe) {
    stdin_pipe_ = std::make_unique<pipe_client>(*runtime_);
  }
  if (options.stdio.stdout_pipe) {
    stdout_pipe_ = std::make_unique<pipe_client>(*runtime_);
  }
  if (options.stdio.stderr_pipe) {
    stderr_pipe_ = std::make_unique<pipe_client>(*runtime_);
  }

  auto state = state_;
  runtime_->call([this, state, options](uv_loop_t* loop) {
    auto* handle = new uv_process_t{};
    handle->data = state.get();
    state->handle = handle;
    state->keep_alive = state;
    state->wait_status = 0;
    state->exit_status = 0;
    state->term_signal = 0;
    state->pid = 0;
    state->running = false;
    state->exited.store(false, std::memory_order_release);

    std::vector<std::string> owned_args;
    owned_args.reserve(options.args.size() + 1);
    owned_args.push_back(options.file);
    for (const auto& arg : options.args) {
      owned_args.push_back(arg);
    }

    std::vector<char*> arg_ptrs;
    arg_ptrs.reserve(owned_args.size() + 1);
    for (auto& arg : owned_args) {
      arg_ptrs.push_back(arg.data());
    }
    arg_ptrs.push_back(nullptr);

    std::vector<std::string> owned_env = options.env;
    std::vector<char*> env_ptrs;
    if (!owned_env.empty()) {
      env_ptrs.reserve(owned_env.size() + 1);
      for (auto& entry : owned_env) {
        env_ptrs.push_back(entry.data());
      }
      env_ptrs.push_back(nullptr);
    }

    std::vector<uv_stdio_container_t> stdio(3);
    for (int i = 0; i < 3; ++i) {
      stdio[i].flags = UV_INHERIT_FD;
      stdio[i].data.fd = i;
    }

    if (stdin_pipe_ != nullptr) {
      stdio[0].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_READABLE_PIPE);
      stdio[0].data.stream = reinterpret_cast<uv_stream_t*>(stdin_pipe_->handle_);
    }
    if (stdout_pipe_ != nullptr) {
      stdio[1].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
      stdio[1].data.stream = reinterpret_cast<uv_stream_t*>(stdout_pipe_->handle_);
    }
    if (stderr_pipe_ != nullptr) {
      stdio[2].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
      stdio[2].data.stream = reinterpret_cast<uv_stream_t*>(stderr_pipe_->handle_);
    }

    uv_process_options_t uv_options{};
    uv_options.exit_cb = [](uv_process_t* process_handle,
                            int64_t exit_status,
                            int term_signal) {
      auto* process_state = static_cast<process::state*>(process_handle->data);
      if (process_state == nullptr) {
        return;
      }

      process_state->exit_status = exit_status;
      process_state->term_signal = term_signal;
      process_state->running = false;
      process_state->exited.store(true, std::memory_order_release);

      if (process_state->waiter) {
        auto waiter = process_state->waiter;
        process_state->waiter = {};
        waiter.resume();
      }
    };
    uv_options.file = options.file.c_str();
    uv_options.args = arg_ptrs.data();
    uv_options.env = env_ptrs.empty() ? nullptr : env_ptrs.data();
    uv_options.cwd = options.cwd.empty() ? nullptr : options.cwd.c_str();
    uv_options.flags = 0;
    if (options.detached) {
      uv_options.flags |= UV_PROCESS_DETACHED;
    }
    if (options.windows_hide) {
      uv_options.flags |= UV_PROCESS_WINDOWS_HIDE;
    }
    if (options.windows_hide_console) {
      uv_options.flags |= UV_PROCESS_WINDOWS_HIDE_CONSOLE;
    }
    if (options.windows_verbatim_arguments) {
      uv_options.flags |= UV_PROCESS_WINDOWS_VERBATIM_ARGUMENTS;
    }
    uv_options.stdio_count = static_cast<int>(stdio.size());
    uv_options.stdio = stdio.data();

    const int rc = uv_spawn(loop, handle, &uv_options);
    if (rc < 0) {
      state->handle = nullptr;
      state->keep_alive.reset();
      delete handle;
      throw_uv_error(rc, "uv_spawn");
    }

    state->running = true;
    state->pid = static_cast<int>(uv_process_get_pid(handle));
    if (stdin_pipe_ != nullptr) {
      stdin_pipe_->connected_ = true;
    }
    if (stdout_pipe_ != nullptr) {
      stdout_pipe_->connected_ = true;
    }
    if (stderr_pipe_ != nullptr) {
      stderr_pipe_->connected_ = true;
    }
  });

  co_return;
}

coro::task<process_exit_status> process::wait(std::stop_token stop_token) {
  if (state_->handle == nullptr) {
    throw std::runtime_error("process has not been spawned");
  }

  struct awaiter {
    std::shared_ptr<state> shared_state;
    std::stop_token stop_token;
    std::coroutine_handle<> continuation{};
    std::optional<stop_callback_t<std::function<void()>>> stop_callback{};

    bool await_ready() const noexcept {
      return stop_token.stop_requested() ||
             shared_state->exited.load(std::memory_order_acquire);
    }

    bool await_suspend(std::coroutine_handle<> h) {
      continuation = h;
      if (stop_token.stop_requested()) {
        shared_state->wait_status = UV_ECANCELED;
        return false;
      }
      if (shared_state->waiter) {
        shared_state->wait_status = UV_EBUSY;
        return false;
      }
      if (stop_token.stop_possible()) {
        stop_callback.emplace(stop_token, std::function<void()>(
            [this]() {
              shared_state->runtime->post([this]() {
                if (shared_state->waiter == continuation) {
                  shared_state->waiter = {};
                  shared_state->wait_status = UV_ECANCELED;
                  auto waiter = continuation;
                  continuation = {};
                  if (waiter) {
                    waiter.resume();
                  }
                }
              });
            }));
      }
      shared_state->waiter = h;
      return true;
    }

    auto await_resume() -> process_exit_status {
      stop_callback.reset();

      if (shared_state->wait_status == UV_ECANCELED || stop_token.stop_requested()) {
        shared_state->wait_status = 0;
        throw operation_cancelled();
      }
      if (shared_state->wait_status < 0) {
        const int rc = shared_state->wait_status;
        shared_state->wait_status = 0;
        throw_uv_error(rc, "process::wait");
      }
      if (!shared_state->exited.load(std::memory_order_acquire)) {
        throw std::runtime_error("process wait resumed before exit");
      }

      process_exit_status out{};
      out.exit_status = shared_state->exit_status;
      out.term_signal = shared_state->term_signal;
      return out;
    }
  };

  co_return co_await awaiter{state_, stop_token};
}

void process::kill(int signum) {
  if (state_->handle == nullptr || !state_->running) {
    throw std::runtime_error("process is not running");
  }

  runtime_->call([state = state_, signum](uv_loop_t*) {
    const int rc = uv_process_kill(state->handle, signum);
    if (rc < 0) {
      throw_uv_error(rc, "uv_process_kill");
    }
  });
}

void process::close() {
  if (stdin_pipe_ != nullptr) {
    stdin_pipe_->close();
    stdin_pipe_.reset();
  }
  if (stdout_pipe_ != nullptr) {
    stdout_pipe_->close();
    stdout_pipe_.reset();
  }
  if (stderr_pipe_ != nullptr) {
    stderr_pipe_->close();
    stderr_pipe_.reset();
  }

  if (runtime_ == nullptr || state_ == nullptr || state_->handle == nullptr) {
    return;
  }

  auto state = state_;
  uv_process_t* handle = state->handle;
  state->handle = nullptr;

  try {
    runtime_->post([state, handle]() {
      if (state->waiter) {
        state->wait_status = UV_ECANCELED;
        auto waiter = state->waiter;
        state->waiter = {};
        waiter.resume();
      }

      if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(handle))) {
        uv_close(
            reinterpret_cast<uv_handle_t*>(handle),
            [](uv_handle_t* h) {
              auto* process_state = static_cast<process::state*>(h->data);
              delete reinterpret_cast<uv_process_t*>(h);
              if (process_state != nullptr) {
                process_state->keep_alive.reset();
              }
            });
      } else {
        state->keep_alive.reset();
      }
    });
  } catch (...) {
  }
}

auto process::running() const noexcept -> bool {
  return state_ != nullptr && state_->running;
}

auto process::pid() const noexcept -> int {
  if (state_ == nullptr) {
    return 0;
  }
  return state_->pid;
}

}  // namespace luvcoro
