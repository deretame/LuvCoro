#include <iostream>
#include <stdexcept>

#include <coro/coro.hpp>

#include "luvcoro/dns.hpp"
#include "luvcoro/runtime.hpp"

namespace {

coro::task<void> run_dns_lookup() {
  auto endpoints = co_await luvcoro::dns::resolve("localhost", 7001, AF_UNSPEC, SOCK_STREAM);
  if (endpoints.empty()) {
    throw std::runtime_error("dns resolve returned no endpoints");
  }

  std::cout << "resolved localhost:" << std::endl;
  for (const auto& endpoint : endpoints) {
    std::cout << "  " << endpoint.address << ":" << endpoint.port
              << (endpoint.ipv6 ? " (ipv6)" : " (ipv4)") << std::endl;
  }

  auto info = co_await luvcoro::dns::reverse_lookup(endpoints.front());
  std::cout << "reverse lookup: host=" << info.host << " service=" << info.service << std::endl;
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime runtime;
    luvcoro::run_with_runtime(runtime, run_dns_lookup());
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
