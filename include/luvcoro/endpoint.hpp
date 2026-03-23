#pragma once

#include <string>

namespace luvcoro {

struct ip_endpoint {
  std::string address;
  int port = 0;
  bool ipv6 = false;
};

}  // namespace luvcoro
