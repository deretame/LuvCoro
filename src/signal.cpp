#include "luvcoro/signal.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "luvcoro/error.hpp"

namespace luvcoro {

struct signal_set::state {
  struct entry {
    std::weak_ptr<state> owner{};
    uv_signal_t* handle = nullptr;
    std::shared_ptr<entry> keep_alive_on_close{};
    int signum = 0;

    static void on_signal(uv_signal_t* handle, int signum) {
      auto* self = static_cast<entry*>(handle->data);
      if (self == nullptr) {
        return;
      }

      auto owner = self->owner.lock();
      if (owner == nullptr || owner->closed.load(std::memory_order_acquire) ||
          owner->out_events == nullptr) {
        return;
      }

      signal_notification notification{};
      notification.signum = signum;
      (void)owner->out_events->try_send(std::move(notification));
    }
  };

  static void close_entry(const std::shared_ptr<entry>& signal_entry) {
    if (signal_entry == nullptr || signal_entry->handle == nullptr) {
      return;
    }

    uv_signal_t* handle = signal_entry->handle;
    signal_entry->handle = nullptr;

    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(handle))) {
      (void)uv_signal_stop(handle);
      signal_entry->keep_alive_on_close = signal_entry;
      uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* raw_handle) {
        auto* self = static_cast<state::entry*>(raw_handle->data);
        delete reinterpret_cast<uv_signal_t*>(raw_handle);
        if (self != nullptr) {
          self->keep_alive_on_close.reset();
        }
      });
    } else {
      delete handle;
    }
  }

  explicit state(uv_runtime& runtime, message_channel<signal_notification>* out_events)
      : runtime(&runtime), out_events(out_events) {}

  uv_runtime* runtime = nullptr;
  message_channel<signal_notification>* out_events = nullptr;
  std::mutex mutex{};
  std::unordered_map<int, std::shared_ptr<entry>> entries{};
  std::atomic<bool> closed = false;
};

signal_set::signal_set()
    : signal_set(current_runtime()) {}

signal_set::signal_set(uv_runtime& runtime)
    : runtime_(&runtime),
      events_(runtime),
      state_(std::make_shared<state>(runtime, &events_)) {}

signal_set::~signal_set() { close(); }

coro::task<void> signal_set::add(int signum, std::stop_token stop_token) {
  if (state_ == nullptr) {
    throw std::runtime_error("signal_set is closed");
  }
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  runtime_->call([state = state_, signum](uv_loop_t* loop) {
    if (state->closed.load(std::memory_order_acquire)) {
      throw std::runtime_error("signal_set is closed");
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->entries.contains(signum)) {
      return;
    }

    auto entry = std::make_shared<state::entry>();
    entry->owner = state;
    entry->signum = signum;

    auto* handle = new uv_signal_t{};
    const int init_rc = uv_signal_init(loop, handle);
    if (init_rc < 0) {
      delete handle;
      throw_uv_error(init_rc, "uv_signal_init");
    }

    handle->data = entry.get();
    const int start_rc = uv_signal_start(handle, &state::entry::on_signal, signum);
    if (start_rc < 0) {
      delete handle;
      throw_uv_error(start_rc, "uv_signal_start");
    }

    entry->handle = handle;
    state->entries.emplace(signum, std::move(entry));
  });

  co_return;
}

coro::task<void> signal_set::remove(int signum, std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }
  if (state_ == nullptr) {
    co_return;
  }

  runtime_->call([state = state_, signum](uv_loop_t*) {
    std::shared_ptr<state::entry> entry;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      auto it = state->entries.find(signum);
      if (it == state->entries.end()) {
        return;
      }
      entry = std::move(it->second);
      state->entries.erase(it);
    }

    state::close_entry(entry);
  });

  co_return;
}

coro::task<void> signal_set::clear(std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }
  if (state_ == nullptr) {
    co_return;
  }

  runtime_->call([state = state_](uv_loop_t*) {
    std::vector<std::shared_ptr<state::entry>> entries;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      entries.reserve(state->entries.size());
      for (auto& [_, entry] : state->entries) {
        entries.push_back(std::move(entry));
      }
      state->entries.clear();
    }

    for (const auto& entry : entries) {
      state::close_entry(entry);
    }
  });

  co_return;
}

auto signal_set::next() -> coro::task<signal_notification> {
  co_return co_await events_.recv();
}

auto signal_set::contains(int signum) const -> bool {
  if (state_ == nullptr) {
    return false;
  }

  return runtime_->call([state = state_, signum](uv_loop_t*) {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->entries.contains(signum);
  });
}

auto signal_set::registered() const -> std::vector<int> {
  if (state_ == nullptr) {
    return {};
  }

  auto registered = runtime_->call([state = state_](uv_loop_t*) {
    std::vector<int> signums;
    std::lock_guard<std::mutex> lock(state->mutex);
    signums.reserve(state->entries.size());
    for (const auto& [signum, _] : state->entries) {
      signums.push_back(signum);
    }
    return signums;
  });

  std::sort(registered.begin(), registered.end());
  return registered;
}

void signal_set::close() {
  if (state_ == nullptr) {
    if (!events_.is_closed()) {
      events_.close();
    }
    return;
  }

  auto state = std::move(state_);
  state->closed.store(true, std::memory_order_release);
  events_.close();

  std::vector<std::shared_ptr<state::entry>> entries;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    entries.reserve(state->entries.size());
    for (auto& [_, entry] : state->entries) {
      entries.push_back(std::move(entry));
    }
    state->entries.clear();
  }

  try {
    runtime_->post([entries = std::move(entries)]() {
      for (const auto& entry : entries) {
        state::close_entry(entry);
      }
    });
  } catch (...) {
  }
}

}  // namespace luvcoro
