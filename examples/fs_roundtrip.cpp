#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

#include <coro/coro.hpp>
#include <uv.h>

#include "luvcoro/fs.hpp"
#include "luvcoro/io.hpp"
#include "luvcoro/runtime.hpp"

namespace {

coro::task<void> run_fs_roundtrip() {
  const auto root = (std::filesystem::current_path() / "luvcoro_fs_roundtrip").string();
  const auto nested = (std::filesystem::path(root) / "nested").string();
  const auto path = (std::filesystem::path(nested) / "sample.txt").string();
  const auto copy_path = (std::filesystem::path(nested) / "sample_copy.txt").string();
  const auto renamed_path = (std::filesystem::path(nested) / "sample_renamed.txt").string();
  const auto hard_link_path = (std::filesystem::path(nested) / "sample_hardlink.txt").string();
  const auto symlink_path = (std::filesystem::path(nested) / "sample_symlink.txt").string();
  const std::string initial = "filesystem";
  const std::string suffix = " roundtrip";

  co_await luvcoro::fs::remove_all(root);
  co_await luvcoro::fs::create_directories(nested);

  if (!(co_await luvcoro::fs::exists(nested)) || !(co_await luvcoro::fs::is_directory(nested))) {
    throw std::runtime_error("directory creation failed");
  }

  co_await luvcoro::fs::write_file(path, initial);
  co_await luvcoro::fs::append_file(path, suffix);

  auto text = co_await luvcoro::fs::read_file(path);
  if (text != initial + suffix) {
    throw std::runtime_error("fs read_file content mismatch");
  }

  {
    luvcoro::file f;
    co_await f.open(path, UV_FS_O_RDWR, 0);
    char prefix_buffer[4] = {};
    char next_buffer[6] = {};

    auto prefix_size = co_await luvcoro::io::read_some_into(f, luvcoro::buffer(prefix_buffer));
    if (prefix_size != 4 || std::string(prefix_buffer, prefix_size) != "file") {
      throw std::runtime_error("fs streaming read mismatch");
    }

    auto next_size = co_await luvcoro::io::read_exactly_into(f, luvcoro::buffer(next_buffer));
    if (next_size != 6 || std::string(next_buffer, next_size) != "system") {
      throw std::runtime_error("fs second streaming read mismatch");
    }

    auto replaced = co_await f.write_at(" async", 10);
    if (replaced != 6) {
      throw std::runtime_error("fs write_at size mismatch");
    }

    auto slice = co_await f.read_at(6, 10);
    if (slice != " async") {
      throw std::runtime_error("fs read_at mismatch");
    }

    co_await f.truncate(16);
    co_await f.close();
  }

  auto truncated = co_await luvcoro::fs::read_file(path);
  if (truncated != "filesystem async") {
    throw std::runtime_error("fs truncate result mismatch");
  }

  auto size = co_await luvcoro::fs::file_size(path);
  if (size != truncated.size()) {
    throw std::runtime_error("fs file_size mismatch");
  }

  {
    luvcoro::file source;
    luvcoro::file target;
    co_await source.open(path, UV_FS_O_RDONLY, 0);
    co_await target.open(copy_path, UV_FS_O_CREAT | UV_FS_O_TRUNC | UV_FS_O_WRONLY, 0644);

    auto copied = co_await luvcoro::io::copy_n(source, target, truncated.size());
    if (copied != truncated.size()) {
      throw std::runtime_error("fs copy size mismatch");
    }

    co_await source.close();
    co_await target.close();
  }

  auto copied_text = co_await luvcoro::fs::read_file(copy_path);
  if (copied_text != truncated) {
    throw std::runtime_error("fs copy content mismatch");
  }

  co_await luvcoro::fs::rename(copy_path, renamed_path);
  if (co_await luvcoro::fs::exists(copy_path)) {
    throw std::runtime_error("fs rename left source behind");
  }
  if (!(co_await luvcoro::fs::exists(renamed_path))) {
    throw std::runtime_error("fs rename did not create destination");
  }

  auto listed = co_await luvcoro::fs::list_directory(nested);
  bool saw_sample = false;
  bool saw_renamed = false;
  for (const auto& entry : listed) {
    if (entry.name == "sample.txt") {
      saw_sample = true;
    }
    if (entry.name == "sample_renamed.txt") {
      saw_renamed = true;
    }
  }
  if (!saw_sample || !saw_renamed) {
    throw std::runtime_error("fs list_directory missing expected entries");
  }

  auto file_info = co_await luvcoro::fs::stat(path);
  if (!file_info.is_regular_file() || file_info.size() != truncated.size()) {
    throw std::runtime_error("fs stat mismatch");
  }

  auto resolved = co_await luvcoro::fs::real_path(path);
  if (resolved.empty()) {
    throw std::runtime_error("fs real_path returned empty result");
  }

  co_await luvcoro::fs::copy_file(path, copy_path, luvcoro::fs::copy_flags::exclusive);
  if (!(co_await luvcoro::fs::exists(copy_path))) {
    throw std::runtime_error("fs copy_file failed");
  }

  co_await luvcoro::fs::create_hard_link(path, hard_link_path);
  auto hard_link_text = co_await luvcoro::fs::read_file(hard_link_path);
  if (hard_link_text != truncated) {
    throw std::runtime_error("fs hard link content mismatch");
  }

  bool symlink_created = false;
  try {
    co_await luvcoro::fs::create_symbolic_link(path, symlink_path);
    auto link_target = co_await luvcoro::fs::read_link(symlink_path);
    if (link_target.empty()) {
      throw std::runtime_error("fs read_link returned empty target");
    }
    (void)co_await luvcoro::fs::lstat(symlink_path);
    symlink_created = true;
  } catch (const luvcoro::uv_error& ex) {
    if (ex.code() != UV_EPERM && ex.code() != UV_ENOTSUP && ex.code() != UV_EACCES) {
      throw;
    }
  }

  co_await luvcoro::fs::chmod(path, 0644);

  if (symlink_created && !(co_await luvcoro::fs::exists(symlink_path))) {
    throw std::runtime_error("fs symlink should exist after creation");
  }
  if (symlink_created) {
    co_await luvcoro::fs::remove_file(symlink_path);
  }

  co_await luvcoro::fs::remove_all(root);
  if (co_await luvcoro::fs::exists(root)) {
    throw std::runtime_error("fs remove_all failed");
  }
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_fs_roundtrip());
    std::cout << "fs roundtrip ok" << std::endl;
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
