#pragma once

#include <stdexcept>
#include <string>

#include <uv.h>

namespace luvcoro {

class uv_error : public std::runtime_error {
 public:
  uv_error(int code, std::string message)
      : std::runtime_error(std::move(message)), code_(code) {}

  int code() const noexcept { return code_; }

 private:
  int code_;
};

class operation_cancelled : public std::runtime_error {
 public:
  explicit operation_cancelled(std::string message = "operation cancelled")
      : std::runtime_error(std::move(message)) {}
};

class operation_timeout : public std::runtime_error {
 public:
  explicit operation_timeout(std::string message = "operation timed out")
      : std::runtime_error(std::move(message)) {}
};

[[noreturn]] inline void throw_uv_error(int code, const char* op) {
  std::string message = std::string(op) + " failed: " + uv_strerror(code);
  throw uv_error(code, std::move(message));
}

}  // namespace luvcoro
