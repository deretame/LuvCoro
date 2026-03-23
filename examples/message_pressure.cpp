#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>

#include <coro/coro.hpp>

#include "luvcoro/message.hpp"
#include "luvcoro/runtime.hpp"

namespace {

constexpr int kProducerCount = 4;
constexpr int kConsumerCount = 4;
constexpr int kMessagesPerProducer = 250;
constexpr int kChannelCapacity = 16;
constexpr int kSentinel = -1;

auto expected_sum() -> long long {
  long long sum = 0;
  for (int producer = 0; producer < kProducerCount; ++producer) {
    for (int i = 0; i < kMessagesPerProducer; ++i) {
      sum += static_cast<long long>(producer * 100000 + i);
    }
  }
  return sum;
}

coro::task<void> producer_task(luvcoro::message_channel<int>& channel, int producer_id) {
  co_await luvcoro::blocking([&channel, producer_id]() {
    for (int i = 0; i < kMessagesPerProducer; ++i) {
      channel.send(producer_id * 100000 + i);
    }
  });
}

coro::task<void> consumer_task(luvcoro::message_channel<int>& channel,
                               std::atomic<int>& consumed,
                               std::atomic<long long>& sum) {
  while (true) {
    auto value = co_await channel.recv_for(std::chrono::seconds(2));
    if (!value.has_value()) {
      throw std::runtime_error("message pressure consumer timed out");
    }
    if (*value == kSentinel) {
      co_return;
    }

    consumed.fetch_add(1, std::memory_order_relaxed);
    sum.fetch_add(*value, std::memory_order_relaxed);
  }
}

coro::task<void> run_message_pressure() {
  luvcoro::message_channel<int> channel(kChannelCapacity);
  std::atomic<int> consumed{0};
  std::atomic<long long> sum{0};

  co_await coro::when_all(
      consumer_task(channel, consumed, sum),
      consumer_task(channel, consumed, sum),
      consumer_task(channel, consumed, sum),
      consumer_task(channel, consumed, sum),
      [&]() -> coro::task<void> {
        co_await coro::when_all(
            producer_task(channel, 0),
            producer_task(channel, 1),
            producer_task(channel, 2),
            producer_task(channel, 3));

        for (int i = 0; i < kConsumerCount; ++i) {
          co_await channel.async_send(kSentinel);
        }
      }());

  const auto total_messages = kProducerCount * kMessagesPerProducer;
  if (consumed.load(std::memory_order_relaxed) != total_messages) {
    throw std::runtime_error("message pressure consumed count mismatch");
  }
  if (sum.load(std::memory_order_relaxed) != expected_sum()) {
    throw std::runtime_error("message pressure checksum mismatch");
  }

  channel.close();
  std::cout << "message pressure consumed: " << total_messages << std::endl;
}

}  // namespace

int main() {
  try {
    luvcoro::uv_runtime_options options;
    options.thread_pool_size = 8;

    luvcoro::uv_runtime runtime(options);
    luvcoro::run_with_runtime(runtime, run_message_pressure());
    std::cout << "message pressure ok" << std::endl;
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
