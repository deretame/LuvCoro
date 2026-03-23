#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "luvcoro/fs.hpp"
#include "luvcoro/io.hpp"
#include "luvcoro/runtime.hpp"

namespace {

auto roundtrip_root() -> std::filesystem::path {
  return std::filesystem::temp_directory_path() / "luvcoro_sendfile_roundtrip";
}

coro::task<void> run_sendfile_roundtrip() {
  const auto root = roundtrip_root();
  const auto source_path = root / "source.txt";
  const auto target_path = root / "target.txt";
  const std::string payload = "sendfile payload";

  if (std::filesystem::exists(root)) {
    co_await luvcoro::fs::remove_all(root.string());
  }

  co_await luvcoro::fs::create_directories(root.string());
  co_await luvcoro::fs::write_file(source_path.string(), payload);

  luvcoro::file source;
  co_await source.open(source_path.string(), UV_FS_O_RDONLY);

  luvcoro::file target;
  co_await target.open(
      target_path.string(), UV_FS_O_CREAT | UV_FS_O_TRUNC | UV_FS_O_WRONLY, 0644);

  auto transferred = co_await luvcoro::io::sendfile(target, source, payload.size());
  if (transferred != payload.size()) {
    throw std::runtime_error("sendfile transferred an unexpected byte count");
  }

  co_await source.close();
  co_await target.close();

  auto copied = co_await luvcoro::fs::read_file(target_path.string());
  if (copied != payload) {
    throw std::runtime_error("sendfile copied unexpected content");
  }

  co_await luvcoro::fs::remove_all(root.string());
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_sendfile_roundtrip());
    std::cout << "sendfile roundtrip ok" << std::endl;
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
