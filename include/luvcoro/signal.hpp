#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

#include <coro/coro.hpp>
#include <uv.h>

#include "luvcoro/message.hpp"
#include "luvcoro/runtime.hpp"

namespace luvcoro {

struct signal_notification {
  int signum = 0;
};

class signal_set {
 public:
  using async_signal = typename message_channel<signal_notification>::async_message;

  signal_set();
  explicit signal_set(uv_runtime& runtime);
  ~signal_set();

  signal_set(const signal_set&) = delete;
  signal_set& operator=(const signal_set&) = delete;
  signal_set(signal_set&&) = delete;
  signal_set& operator=(signal_set&&) = delete;

  coro::task<void> add(int signum, std::stop_token stop_token = {});
  coro::task<void> remove(int signum, std::stop_token stop_token = {});
  coro::task<void> clear(std::stop_token stop_token = {});

  auto next() -> coro::task<signal_notification>;

  template <typename Rep, typename Period>
  auto next_for(std::chrono::duration<Rep, Period> timeout)
      -> coro::task<std::optional<signal_notification>> {
    co_return co_await events_.recv_for(timeout);
  }

  auto next_async() -> async_signal { return events_.next(); }

  template <typename Rep, typename Period>
  auto next_async_for(std::chrono::duration<Rep, Period> timeout) -> async_signal {
    return events_.next_for(timeout);
  }

  template <typename Rep, typename Period>
  auto next_async_until(std::stop_token stop_token,
                        std::chrono::duration<Rep, Period> poll_interval =
                            std::chrono::milliseconds(50)) -> async_signal {
    return events_.next_until(stop_token, poll_interval);
  }

  auto signals() -> coro::generator<async_signal> { return events_.messages(); }

  template <typename Rep, typename Period>
  auto signals_for(std::chrono::duration<Rep, Period> timeout)
      -> coro::generator<async_signal> {
    return events_.messages_for(timeout);
  }

  template <typename Rep, typename Period>
  auto signals_until(std::stop_token stop_token,
                     std::chrono::duration<Rep, Period> poll_interval =
                         std::chrono::milliseconds(50))
      -> coro::generator<async_signal> {
    return events_.messages_until(stop_token, poll_interval);
  }

  auto contains(int signum) const -> bool;
  auto registered() const -> std::vector<int>;

  void close();

 private:
  struct state;

  uv_runtime* runtime_ = nullptr;
  message_channel<signal_notification> events_;
  std::shared_ptr<state> state_{};
};

}  // namespace luvcoro
