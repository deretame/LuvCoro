#include "luvcoro/fs.hpp"

#include <atomic>
#include <coroutine>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <cerrno>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "luvcoro/error.hpp"

namespace luvcoro {
namespace {

struct scandir_entry {
  std::string name;
  uv_dirent_type_t type = UV_DIRENT_UNKNOWN;
};

template <typename Fn>
using stop_callback_t = std::stop_callback<std::decay_t<Fn>>;

#ifdef _WIN32
auto duplicate_file_descriptor(uv_file fd) -> uv_file {
  const int dup_fd = _dup(fd);
  if (dup_fd < 0) {
    throw_uv_error(uv_translate_sys_error(errno), "_dup");
  }
  return dup_fd;
}
#else
auto duplicate_file_descriptor(uv_file fd) -> uv_file {
  const int dup_fd = dup(fd);
  if (dup_fd < 0) {
    throw_uv_error(uv_translate_sys_error(errno), "dup");
  }
  return dup_fd;
}
#endif

#ifdef _WIN32
auto seek_file_descriptor(uv_file fd, std::int64_t offset) -> int {
  auto rc = _lseeki64(fd, offset, SEEK_SET);
  if (rc < 0) {
    return uv_translate_sys_error(errno);
  }
  return 0;
}
#else
auto seek_file_descriptor(uv_file fd, std::int64_t offset) -> int {
  auto rc = lseek(fd, static_cast<off_t>(offset), SEEK_SET);
  if (rc < 0) {
    return uv_translate_sys_error(errno);
  }
  return 0;
}
#endif

template <typename Result>
struct fs_result_storage {
  Result value{};
};

template <>
struct fs_result_storage<void> {};

template <typename Result, typename StartFn, typename FinishFn>
auto run_fs_request(uv_runtime& runtime,
                    std::stop_token stop_token,
                    const char* op_name,
                    StartFn&& start_fn,
                    FinishFn&& finish_fn) -> coro::task<Result> {
  using start_t = std::decay_t<StartFn>;
  using finish_t = std::decay_t<FinishFn>;

  struct state {
    uv_runtime* runtime = nullptr;
    const char* op_name = nullptr;
    start_t start_fn;
    finish_t finish_fn;
    uv_fs_t req{};
    std::coroutine_handle<> continuation{};
    std::shared_ptr<state> keep_alive{};
    std::optional<stop_callback_t<std::function<void()>>> stop_callback{};
    std::atomic<bool> completed = false;
    int status = 0;
    bool cancel_requested = false;
    bool started = false;
    bool cleanup_required = false;
    fs_result_storage<Result> result{};

    state(uv_runtime* runtime, const char* op_name, start_t start_fn, finish_t finish_fn)
        : runtime(runtime),
          op_name(op_name),
          start_fn(std::move(start_fn)),
          finish_fn(std::move(finish_fn)) {}

    void complete() {
      if (completed.exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      if (cleanup_required) {
        uv_fs_req_cleanup(&req);
        cleanup_required = false;
      }
      auto h = continuation;
      continuation = {};
      if (h) {
        h.resume();
      }
      stop_callback.reset();
      keep_alive.reset();
    }
  };

  struct awaiter {
    std::shared_ptr<state> shared_state;
    std::stop_token stop_token;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> h) {
      if (stop_token.stop_requested()) {
        shared_state->cancel_requested = true;
        shared_state->status = UV_ECANCELED;
        return false;
      }

      shared_state->continuation = h;
      shared_state->keep_alive = shared_state;
      shared_state->req.data = shared_state.get();

      if (stop_token.stop_possible()) {
        shared_state->stop_callback.emplace(stop_token, std::function<void()>(
            [op_state = shared_state]() {
              op_state->runtime->post([op_state]() {
                if (op_state->completed.load(std::memory_order_acquire)) {
                  return;
                }
                op_state->cancel_requested = true;
                if (!op_state->started) {
                  op_state->status = UV_ECANCELED;
                  op_state->complete();
                  return;
                }
                const int rc =
                    uv_cancel(reinterpret_cast<uv_req_t*>(&op_state->req));
                if (rc < 0 && rc != UV_EBUSY && rc != UV_ENOSYS) {
                  op_state->status = UV_ECANCELED;
                  op_state->complete();
                }
              });
            }));
      }

      shared_state->runtime->post([op_state = shared_state]() {
        if (op_state->completed.load(std::memory_order_acquire)) {
          return;
        }
        op_state->started = true;
        op_state->cleanup_required = true;
        const int rc = op_state->start_fn(
            op_state.get(),
            [](uv_fs_t* req) {
              auto* self = static_cast<state*>(req->data);
              if (req->result < 0) {
                self->status = static_cast<int>(req->result);
              } else {
                self->finish_fn(self);
              }
              self->complete();
            });
        if (rc < 0) {
          op_state->status = rc;
          op_state->complete();
        }
      });

      return true;
    }

    auto await_resume() -> Result {
      if (shared_state->status == UV_ECANCELED ||
          (shared_state->cancel_requested && shared_state->status == 0)) {
        throw operation_cancelled();
      }
      if (shared_state->status < 0) {
        throw_uv_error(shared_state->status, shared_state->op_name);
      }

      if constexpr (std::is_void_v<Result>) {
        return;
      } else {
        return std::move(shared_state->result.value);
      }
    }
  };

  auto shared_state = std::make_shared<state>(
      &runtime, op_name, std::forward<StartFn>(start_fn), std::forward<FinishFn>(finish_fn));
  co_return co_await awaiter{std::move(shared_state), stop_token};
}

auto stat_path(uv_runtime& runtime,
               const std::string& path,
               std::stop_token stop_token) -> coro::task<uv_stat_t> {
  auto owned_path = path;
  co_return co_await run_fs_request<uv_stat_t>(
      runtime,
      stop_token,
      "uv_fs_stat",
      [path = std::move(owned_path)](auto* state, uv_fs_cb cb) {
        return uv_fs_stat(
            state->runtime->loop(),
            &state->req,
            path.c_str(),
            cb);
      },
      [](auto* state) { state->result.value = state->req.statbuf; });
}

auto lstat_path(uv_runtime& runtime,
                const std::string& path,
                std::stop_token stop_token) -> coro::task<uv_stat_t> {
  auto owned_path = path;
  co_return co_await run_fs_request<uv_stat_t>(
      runtime,
      stop_token,
      "uv_fs_lstat",
      [path = std::move(owned_path)](auto* state, uv_fs_cb cb) {
        return uv_fs_lstat(
            state->runtime->loop(),
            &state->req,
            path.c_str(),
            cb);
      },
      [](auto* state) { state->result.value = state->req.statbuf; });
}

auto mkdir_path(uv_runtime& runtime,
                const std::string& path,
                int mode,
                std::stop_token stop_token) -> coro::task<void> {
  auto owned_path = path;
  co_await run_fs_request<void>(
      runtime,
      stop_token,
      "uv_fs_mkdir",
      [path = std::move(owned_path), mode](auto* state, uv_fs_cb cb) {
        return uv_fs_mkdir(
            state->runtime->loop(),
            &state->req,
            path.c_str(),
            mode,
            cb);
      },
      [](auto*) {});
}

auto rmdir_path(uv_runtime& runtime,
                const std::string& path,
                std::stop_token stop_token) -> coro::task<void> {
  auto owned_path = path;
  co_await run_fs_request<void>(
      runtime,
      stop_token,
      "uv_fs_rmdir",
      [path = std::move(owned_path)](auto* state, uv_fs_cb cb) {
        return uv_fs_rmdir(
            state->runtime->loop(),
            &state->req,
            path.c_str(),
            cb);
      },
      [](auto*) {});
}

auto unlink_path(uv_runtime& runtime,
                 const std::string& path,
                 std::stop_token stop_token) -> coro::task<void> {
  auto owned_path = path;
  co_await run_fs_request<void>(
      runtime,
      stop_token,
      "uv_fs_unlink",
      [path = std::move(owned_path)](auto* state, uv_fs_cb cb) {
        return uv_fs_unlink(
            state->runtime->loop(),
            &state->req,
            path.c_str(),
            cb);
      },
      [](auto*) {});
}

auto rename_path(uv_runtime& runtime,
                 const std::string& path,
                 const std::string& new_path,
                 std::stop_token stop_token) -> coro::task<void> {
  auto owned_path = path;
  auto owned_new_path = new_path;
  co_await run_fs_request<void>(
      runtime,
      stop_token,
      "uv_fs_rename",
      [path = std::move(owned_path), new_path = std::move(owned_new_path)](auto* state,
                                                                           uv_fs_cb cb) {
        return uv_fs_rename(
            state->runtime->loop(),
            &state->req,
            path.c_str(),
            new_path.c_str(),
            cb);
      },
      [](auto*) {});
}

auto copy_file_path(uv_runtime& runtime,
                    const std::string& path,
                    const std::string& new_path,
                    unsigned int flags,
                    std::stop_token stop_token) -> coro::task<void> {
  auto owned_path = path;
  auto owned_new_path = new_path;
  co_await run_fs_request<void>(
      runtime,
      stop_token,
      "uv_fs_copyfile",
      [path = std::move(owned_path), new_path = std::move(owned_new_path), flags](auto* state,
                                                                                   uv_fs_cb cb) {
        return uv_fs_copyfile(
            state->runtime->loop(),
            &state->req,
            path.c_str(),
            new_path.c_str(),
            static_cast<int>(flags),
            cb);
      },
      [](auto*) {});
}

auto link_path(uv_runtime& runtime,
               const std::string& path,
               const std::string& new_path,
               std::stop_token stop_token) -> coro::task<void> {
  auto owned_path = path;
  auto owned_new_path = new_path;
  co_await run_fs_request<void>(
      runtime,
      stop_token,
      "uv_fs_link",
      [path = std::move(owned_path), new_path = std::move(owned_new_path)](auto* state,
                                                                           uv_fs_cb cb) {
        return uv_fs_link(
            state->runtime->loop(),
            &state->req,
            path.c_str(),
            new_path.c_str(),
            cb);
      },
      [](auto*) {});
}

auto symlink_path(uv_runtime& runtime,
                  const std::string& path,
                  const std::string& new_path,
                  unsigned int flags,
                  std::stop_token stop_token) -> coro::task<void> {
  auto owned_path = path;
  auto owned_new_path = new_path;
  co_await run_fs_request<void>(
      runtime,
      stop_token,
      "uv_fs_symlink",
      [path = std::move(owned_path), new_path = std::move(owned_new_path), flags](auto* state,
                                                                                   uv_fs_cb cb) {
        return uv_fs_symlink(
            state->runtime->loop(),
            &state->req,
            path.c_str(),
            new_path.c_str(),
            static_cast<int>(flags),
            cb);
      },
      [](auto*) {});
}

auto readlink_path(uv_runtime& runtime,
                   const std::string& path,
                   std::stop_token stop_token) -> coro::task<std::string> {
  auto owned_path = path;
  co_return co_await run_fs_request<std::string>(
      runtime,
      stop_token,
      "uv_fs_readlink",
      [path = std::move(owned_path)](auto* state, uv_fs_cb cb) {
        return uv_fs_readlink(
            state->runtime->loop(),
            &state->req,
            path.c_str(),
            cb);
      },
      [](auto* state) {
        if (state->req.ptr != nullptr) {
          state->result.value = static_cast<const char*>(state->req.ptr);
        }
      });
}

auto realpath_path(uv_runtime& runtime,
                   const std::string& path,
                   std::stop_token stop_token) -> coro::task<std::string> {
  auto owned_path = path;
  co_return co_await run_fs_request<std::string>(
      runtime,
      stop_token,
      "uv_fs_realpath",
      [path = std::move(owned_path)](auto* state, uv_fs_cb cb) {
        return uv_fs_realpath(
            state->runtime->loop(),
            &state->req,
            path.c_str(),
            cb);
      },
      [](auto* state) {
        if (state->req.ptr != nullptr) {
          state->result.value = static_cast<const char*>(state->req.ptr);
        }
      });
}

auto chmod_path(uv_runtime& runtime,
                const std::string& path,
                int mode,
                std::stop_token stop_token) -> coro::task<void> {
  auto owned_path = path;
  co_await run_fs_request<void>(
      runtime,
      stop_token,
      "uv_fs_chmod",
      [path = std::move(owned_path), mode](auto* state, uv_fs_cb cb) {
        return uv_fs_chmod(
            state->runtime->loop(),
            &state->req,
            path.c_str(),
            mode,
            cb);
      },
      [](auto*) {});
}

auto ftruncate_file(uv_runtime& runtime,
                    uv_file fd,
                    std::int64_t size,
                    std::stop_token stop_token) -> coro::task<void> {
  co_await run_fs_request<void>(
      runtime,
      stop_token,
      "uv_fs_ftruncate",
      [fd, size](auto* state, uv_fs_cb cb) {
        return uv_fs_ftruncate(
            state->runtime->loop(),
            &state->req,
            fd,
            size,
            cb);
      },
      [](auto*) {});
}

auto open_file_async(uv_runtime& runtime,
                     const std::string& path,
                     int flags,
                     int mode,
                     std::stop_token stop_token) -> coro::task<uv_file> {
  auto owned_path = path;
  co_return co_await run_fs_request<uv_file>(
      runtime,
      stop_token,
      "uv_fs_open",
      [path = std::move(owned_path), flags, mode](auto* state, uv_fs_cb cb) {
        return uv_fs_open(
            state->runtime->loop(),
            &state->req,
            path.c_str(),
            flags,
            mode,
            cb);
      },
      [](auto* state) {
        state->result.value = static_cast<uv_file>(state->req.result);
      });
}

auto close_file_async(uv_runtime& runtime,
                      uv_file fd,
                      std::stop_token stop_token) -> coro::task<void> {
  co_await run_fs_request<void>(
      runtime,
      stop_token,
      "uv_fs_close",
      [fd](auto* state, uv_fs_cb cb) {
        return uv_fs_close(
            state->runtime->loop(),
            &state->req,
            fd,
            cb);
      },
      [](auto*) {});
}

auto read_file_async_into(uv_runtime& runtime,
                          uv_file fd,
                          std::span<char> buffer,
                          std::int64_t offset,
                          std::stop_token stop_token) -> coro::task<std::size_t> {
  if (buffer.empty()) {
    co_return 0;
  }

  co_return co_await run_fs_request<std::size_t>(
      runtime,
      stop_token,
      "uv_fs_read",
      [fd, buffer, offset](auto* state, uv_fs_cb cb) {
        auto uvbuf =
            uv_buf_init(buffer.data(), static_cast<unsigned int>(buffer.size()));
        return uv_fs_read(
            state->runtime->loop(), &state->req, fd, &uvbuf, 1, offset, cb);
      },
      [](auto* state) {
        state->result.value = static_cast<std::size_t>(state->req.result);
      });
}

auto read_file_async(uv_runtime& runtime,
                     uv_file fd,
                     std::size_t max_bytes,
                     std::int64_t offset,
                     std::stop_token stop_token)
    -> coro::task<std::string> {
  if (max_bytes == 0) {
    co_return std::string{};
  }

  std::string out(max_bytes, '\0');
  auto size = co_await read_file_async_into(
      runtime, fd, std::span<char>(out.data(), out.size()), offset, stop_token);
  out.resize(size);
  co_return out;
}

auto write_file_async(uv_runtime& runtime,
                      uv_file fd,
                      std::span<const char> data,
                      std::int64_t offset,
                      std::stop_token stop_token)
    -> coro::task<std::size_t> {
  if (data.empty()) {
    co_return 0;
  }

  co_return co_await run_fs_request<std::size_t>(
      runtime,
      stop_token,
      "uv_fs_write",
      [fd, data, offset](auto* state, uv_fs_cb cb) {
        auto uvbuf = uv_buf_init(const_cast<char*>(data.data()),
                                 static_cast<unsigned int>(data.size()));
        return uv_fs_write(
            state->runtime->loop(), &state->req, fd, &uvbuf, 1, offset, cb);
      },
      [](auto* state) {
        state->result.value = static_cast<std::size_t>(state->req.result);
      });
}

auto write_file_async_owned(uv_runtime& runtime,
                            uv_file fd,
                            std::string_view data,
                            std::int64_t offset,
                            std::stop_token stop_token) -> coro::task<std::size_t> {
  std::string owned(data);
  co_return co_await write_file_async(runtime,
                                      fd,
                                      std::span<const char>(owned.data(), owned.size()),
                                      offset,
                                      stop_token);
}

auto sendfile_async(uv_runtime& runtime,
                    uv_file out_fd,
                    std::int64_t out_offset,
                    uv_file in_fd,
                    std::int64_t in_offset,
                    std::size_t max_bytes,
                    std::stop_token stop_token) -> coro::task<std::size_t> {
  if (max_bytes == 0) {
    co_return 0;
  }

  co_return co_await run_fs_request<std::size_t>(
      runtime,
      stop_token,
      "uv_fs_sendfile",
      [out_fd, out_offset, in_fd, in_offset, max_bytes](auto* state, uv_fs_cb cb) {
        const int seek_rc = seek_file_descriptor(out_fd, out_offset);
        if (seek_rc < 0) {
          return seek_rc;
        }

        return uv_fs_sendfile(
            state->runtime->loop(),
            &state->req,
            out_fd,
            in_fd,
            in_offset,
            max_bytes,
            cb);
      },
      [](auto* state) {
        state->result.value = static_cast<std::size_t>(state->req.result);
      });
}

auto scandir_sync(uv_runtime& runtime, const std::string& path) -> std::vector<scandir_entry> {
  return runtime.call([&path](uv_loop_t* loop) {
    uv_fs_t req{};
    int rc = uv_fs_scandir(loop, &req, path.c_str(), 0, nullptr);
    if (rc < 0) {
      uv_fs_req_cleanup(&req);
      throw_uv_error(rc, "uv_fs_scandir");
    }

    std::vector<scandir_entry> entries;
    uv_dirent_t ent{};
    while ((rc = uv_fs_scandir_next(&req, &ent)) != UV_EOF) {
      if (rc < 0) {
        uv_fs_req_cleanup(&req);
        throw_uv_error(rc, "uv_fs_scandir_next");
      }

      std::string name = ent.name;
      if (name == "." || name == "..") {
        continue;
      }
      entries.push_back(scandir_entry{std::move(name), ent.type});
    }

    uv_fs_req_cleanup(&req);
    return entries;
  });
}

auto join_path(const std::string& base, const std::string& name) -> std::string {
  return (std::filesystem::path(base) / name).string();
}

}  // namespace

file::file() : file(current_runtime()) {}

file::file(uv_runtime& runtime) : runtime_(&runtime) {}

file::~file() { close_sync_noexcept(); }

file::file(file&& other) noexcept : runtime_(other.runtime_), fd_(other.fd_), offset_(other.offset_) {
  other.runtime_ = nullptr;
  other.fd_ = -1;
  other.offset_ = 0;
}

file& file::operator=(file&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  close_sync_noexcept();

  runtime_ = other.runtime_;
  fd_ = other.fd_;
  offset_ = other.offset_;

  other.runtime_ = nullptr;
  other.fd_ = -1;
  other.offset_ = 0;
  return *this;
}

void file::close_sync_noexcept() {
  if (runtime_ == nullptr || fd_ < 0) {
    return;
  }

  const uv_file fd = fd_;
  fd_ = -1;
  offset_ = 0;

  try {
    runtime_->call([fd](uv_loop_t* loop) {
      uv_fs_t req{};
      const int rc = uv_fs_close(loop, &req, fd, nullptr);
      uv_fs_req_cleanup(&req);
      if (rc < 0) {
        throw_uv_error(rc, "uv_fs_close");
      }
    });
  } catch (...) {
    // Destructors must not throw.
  }
}

coro::task<void> file::open(const std::string& path,
                            int flags,
                            int mode,
                            std::stop_token stop_token) {
  if (is_open()) {
    co_await close(stop_token);
  }

  fd_ = co_await open_file_async(*runtime_, path, flags, mode, stop_token);
  offset_ = 0;
}

coro::task<void> file::open(uv_file fd, std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }
  if (fd < 0) {
    throw std::runtime_error("file descriptor is invalid");
  }

  if (is_open()) {
    co_await close(stop_token);
  }

  fd_ = duplicate_file_descriptor(fd);
  offset_ = 0;
}

coro::task<std::string> file::read(std::size_t max_bytes, std::stop_token stop_token) {
  auto data = co_await read_some(max_bytes, stop_token);
  co_return data;
}

coro::task<std::string> file::read_some(std::size_t max_bytes, std::stop_token stop_token) {
  auto data = co_await read_at(max_bytes, offset_, stop_token);
  offset_ += static_cast<std::int64_t>(data.size());
  co_return data;
}

coro::task<std::size_t> file::read_some_into(std::span<char> buffer,
                                             std::stop_token stop_token) {
  auto size = co_await read_at_into(buffer, offset_, stop_token);
  offset_ += static_cast<std::int64_t>(size);
  co_return size;
}

coro::task<std::string> file::read_exact(std::size_t total_bytes,
                                         std::stop_token stop_token) {
  std::string out;
  out.reserve(total_bytes);

  while (out.size() < total_bytes) {
    if (stop_token.stop_requested()) {
      throw operation_cancelled();
    }
    auto chunk = co_await read_some(total_bytes - out.size(), stop_token);
    if (chunk.empty()) {
      break;
    }
    out += chunk;
  }

  co_return out;
}

coro::task<std::size_t> file::read_exact_into(std::span<char> buffer,
                                              std::stop_token stop_token) {
  std::size_t total = 0;

  while (total < buffer.size()) {
    if (stop_token.stop_requested()) {
      throw operation_cancelled();
    }
    auto chunk = co_await read_some_into(buffer.subspan(total), stop_token);
    if (chunk == 0) {
      break;
    }
    total += chunk;
  }

  co_return total;
}

coro::task<std::size_t> file::write(std::string_view data,
                                    std::stop_token stop_token) {
  auto written = co_await write_at(data, offset_, stop_token);
  offset_ += static_cast<std::int64_t>(written);
  co_return written;
}

coro::task<std::size_t> file::write_some(std::span<const char> data,
                                         std::stop_token stop_token) {
  auto written = co_await write_some_at(data, offset_, stop_token);
  offset_ += static_cast<std::int64_t>(written);
  co_return written;
}

coro::task<std::string> file::read_at(std::size_t max_bytes,
                                      std::int64_t offset,
                                      std::stop_token stop_token) {
  if (!is_open()) {
    throw std::runtime_error("file is not open");
  }

  co_return co_await read_file_async(*runtime_, fd_, max_bytes, offset, stop_token);
}

coro::task<std::size_t> file::read_at_into(std::span<char> buffer,
                                           std::int64_t offset,
                                           std::stop_token stop_token) {
  if (!is_open()) {
    throw std::runtime_error("file is not open");
  }

  co_return co_await read_file_async_into(*runtime_, fd_, buffer, offset, stop_token);
}

coro::task<std::size_t> file::write_at(std::string_view data,
                                       std::int64_t offset,
                                       std::stop_token stop_token) {
  if (!is_open()) {
    throw std::runtime_error("file is not open");
  }

  co_return co_await write_file_async_owned(*runtime_, fd_, data, offset, stop_token);
}

coro::task<std::size_t> file::write_some_at(std::span<const char> data,
                                            std::int64_t offset,
                                            std::stop_token stop_token) {
  if (!is_open()) {
    throw std::runtime_error("file is not open");
  }

  co_return co_await write_file_async(*runtime_, fd_, data, offset, stop_token);
}

coro::task<std::string> file::read_all(std::stop_token stop_token) {
  std::string out;
  while (true) {
    if (stop_token.stop_requested()) {
      throw operation_cancelled();
    }
    auto chunk = co_await read(64 * 1024, stop_token);
    if (chunk.empty()) {
      break;
    }
    out += chunk;
  }
  co_return out;
}

coro::task<void> file::truncate(std::int64_t size, std::stop_token stop_token) {
  if (!is_open()) {
    throw std::runtime_error("file is not open");
  }

  co_await ftruncate_file(*runtime_, fd_, size, stop_token);
  if (offset_ > size) {
    offset_ = size;
  }
}

coro::task<void> file::close(std::stop_token stop_token) {
  if (!is_open()) {
    co_return;
  }

  const uv_file fd = fd_;
  fd_ = -1;
  offset_ = 0;
  co_await close_file_async(*runtime_, fd, stop_token);
}

namespace fs {

coro::task<file_status> stat(const std::string& path, std::stop_token stop_token) {
  auto statbuf = co_await stat_path(current_runtime(), path, stop_token);
  co_return file_status{statbuf};
}

coro::task<file_status> lstat(const std::string& path, std::stop_token stop_token) {
  auto statbuf = co_await lstat_path(current_runtime(), path, stop_token);
  co_return file_status{statbuf};
}

coro::task<bool> exists(const std::string& path, std::stop_token stop_token) {
  try {
    (void)co_await stat(path, stop_token);
    co_return true;
  } catch (const uv_error& ex) {
    if (ex.code() == UV_ENOENT) {
      co_return false;
    }
    throw;
  }
}

coro::task<bool> is_directory(const std::string& path, std::stop_token stop_token) {
  auto status = co_await stat(path, stop_token);
  co_return status.is_directory();
}

coro::task<std::uint64_t> file_size(const std::string& path, std::stop_token stop_token) {
  auto status = co_await stat(path, stop_token);
  co_return status.size();
}

coro::task<std::vector<directory_entry>> list_directory(const std::string& path,
                                                        std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  auto raw_entries = scandir_sync(current_runtime(), path);
  std::vector<directory_entry> entries;
  entries.reserve(raw_entries.size());
  for (auto& entry : raw_entries) {
    entries.push_back(directory_entry{std::move(entry.name), entry.type});
  }
  co_return entries;
}

coro::task<void> create_directory(const std::string& path,
                                  int mode,
                                  std::stop_token stop_token) {
  bool already_exists = false;
  try {
    co_await mkdir_path(current_runtime(), path, mode, stop_token);
  } catch (const uv_error& ex) {
    if (ex.code() == UV_EEXIST) {
      already_exists = true;
    } else {
      throw;
    }
  }

  if (already_exists && !(co_await is_directory(path, stop_token))) {
    throw std::runtime_error("path exists but is not a directory: " + path);
  }
}

coro::task<void> create_directories(const std::string& path,
                                    int mode,
                                    std::stop_token stop_token) {
  auto target = std::filesystem::path(path).lexically_normal();
  if (target.empty()) {
    co_return;
  }

  auto target_str = target.string();
  if (co_await exists(target_str, stop_token)) {
    co_return;
  }

  auto parent = target.parent_path();
  if (!parent.empty() && parent != target) {
    auto parent_str = parent.string();
    if (!parent_str.empty() && !(co_await exists(parent_str, stop_token))) {
      co_await create_directories(parent_str, mode, stop_token);
    }
  }

  if (!(co_await exists(target_str, stop_token))) {
    co_await create_directory(target_str, mode, stop_token);
  }
}

coro::task<void> rename(const std::string& path,
                        const std::string& new_path,
                        std::stop_token stop_token) {
  co_await rename_path(current_runtime(), path, new_path, stop_token);
}

coro::task<void> copy_file(const std::string& path,
                           const std::string& new_path,
                           copy_flags flags,
                           std::stop_token stop_token) {
  co_await copy_file_path(current_runtime(),
                          path,
                          new_path,
                          static_cast<unsigned int>(flags),
                          stop_token);
}

coro::task<void> create_hard_link(const std::string& path,
                                  const std::string& new_path,
                                  std::stop_token stop_token) {
  co_await link_path(current_runtime(), path, new_path, stop_token);
}

coro::task<void> create_symbolic_link(const std::string& path,
                                      const std::string& new_path,
                                      symlink_flags flags,
                                      std::stop_token stop_token) {
  co_await symlink_path(current_runtime(),
                        path,
                        new_path,
                        static_cast<unsigned int>(flags),
                        stop_token);
}

coro::task<std::string> read_link(const std::string& path, std::stop_token stop_token) {
  co_return co_await readlink_path(current_runtime(), path, stop_token);
}

coro::task<std::string> real_path(const std::string& path, std::stop_token stop_token) {
  co_return co_await realpath_path(current_runtime(), path, stop_token);
}

coro::task<void> chmod(const std::string& path,
                       int mode,
                       std::stop_token stop_token) {
  co_await chmod_path(current_runtime(), path, mode, stop_token);
}

coro::task<void> remove_file(const std::string& path, std::stop_token stop_token) {
  co_await unlink_path(current_runtime(), path, stop_token);
}

coro::task<void> remove_directory(const std::string& path, std::stop_token stop_token) {
  co_await rmdir_path(current_runtime(), path, stop_token);
}

coro::task<void> remove_all(const std::string& path, std::stop_token stop_token) {
  if (!(co_await exists(path, stop_token))) {
    co_return;
  }

  if (co_await is_directory(path, stop_token)) {
    auto entries = scandir_sync(current_runtime(), path);
    for (const auto& entry : entries) {
      if (stop_token.stop_requested()) {
        throw operation_cancelled();
      }
      co_await remove_all(join_path(path, entry.name), stop_token);
    }
    bool retry_remove_directory = false;
    try {
      co_await remove_directory(path, stop_token);
    } catch (const uv_error& ex) {
      if (ex.code() != UV_ENOTEMPTY) {
        throw;
      }
      retry_remove_directory = true;
    }

    if (retry_remove_directory) {
      std::vector<std::filesystem::directory_entry> leftovers;
      for (const auto& entry : std::filesystem::directory_iterator(path)) {
        leftovers.push_back(entry);
      }

      for (const auto& entry : leftovers) {
        if (stop_token.stop_requested()) {
          throw operation_cancelled();
        }

        std::error_code ec;
        const auto status = entry.symlink_status(ec);
        if (ec) {
          throw std::runtime_error("failed to inspect leftover directory entry");
        }

        const auto child = entry.path().string();
        if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
          std::filesystem::remove(entry.path(), ec);
          if (ec) {
            throw std::runtime_error("failed to remove leftover directory entry");
          }
          continue;
        }

        co_await remove_all(child, stop_token);
      }

      co_await remove_directory(path, stop_token);
    }
  } else {
    co_await remove_file(path, stop_token);
  }
}

coro::task<std::string> read_file(const std::string& path, std::stop_token stop_token) {
  file f;
  co_await f.open(path, UV_FS_O_RDONLY, 0, stop_token);
  auto content = co_await f.read_all(stop_token);
  co_await f.close(stop_token);
  co_return content;
}

coro::task<void> write_file(const std::string& path,
                            std::string_view data,
                            int mode,
                            std::stop_token stop_token) {
  file f;
  co_await f.open(path, UV_FS_O_CREAT | UV_FS_O_TRUNC | UV_FS_O_WRONLY, mode, stop_token);
  co_await f.write(data, stop_token);
  co_await f.close(stop_token);
}

coro::task<void> append_file(const std::string& path,
                             std::string_view data,
                             int mode,
                             std::stop_token stop_token) {
  std::uint64_t offset = 0;
  if (co_await exists(path, stop_token)) {
    offset = co_await file_size(path, stop_token);
  }

  file f;
  co_await f.open(path, UV_FS_O_CREAT | UV_FS_O_WRONLY, mode, stop_token);
  co_await f.write_at(data, static_cast<std::int64_t>(offset), stop_token);
  co_await f.close(stop_token);
}

coro::task<std::size_t> sendfile(file& target,
                                 file& source,
                                 std::size_t max_bytes,
                                 std::stop_token stop_token) {
  co_return co_await sendfile(
      target, source, source.tell(), max_bytes, stop_token);
}

coro::task<std::size_t> sendfile(file& target,
                                 file& source,
                                 std::int64_t source_offset,
                                 std::size_t max_bytes,
                                 std::stop_token stop_token) {
  if (!target.is_open()) {
    throw std::runtime_error("sendfile target is not open");
  }
  if (!source.is_open()) {
    throw std::runtime_error("sendfile source is not open");
  }
  if (target.runtime() == nullptr || source.runtime() == nullptr) {
    throw std::runtime_error("sendfile file has no runtime");
  }
  if (target.runtime() != source.runtime()) {
    throw std::runtime_error("sendfile requires source and target to use the same runtime");
  }

  auto transferred = co_await sendfile_async(*source.runtime(),
                                             target.native_handle(),
                                             target.tell(),
                                             source.native_handle(),
                                             source_offset,
                                             max_bytes,
                                             stop_token);
  source.seek(source_offset + static_cast<std::int64_t>(transferred));
  target.seek(target.tell() + static_cast<std::int64_t>(transferred));
  co_return transferred;
}

}  // namespace fs

}  // namespace luvcoro
