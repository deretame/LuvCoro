#include "luvcoro/fs.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "luvcoro/error.hpp"

namespace luvcoro::fs {
namespace {

template <typename Fn>
using stop_callback_t = std::stop_callback<std::decay_t<Fn>>;

}  // namespace

struct event_watcher::state {
  explicit state(uv_runtime& runtime, message_channel<event_notification>* out_events)
      : runtime(&runtime), out_events(out_events) {}

  uv_runtime* runtime = nullptr;
  message_channel<event_notification>* out_events = nullptr;
  uv_fs_event_t* handle = nullptr;
  std::shared_ptr<state> keep_alive_on_close{};
  bool closing = false;
  bool closed = false;
  std::string path{};

  static void on_event(uv_fs_event_t* handle,
                       const char* filename,
                       int events,
                       int status) {
    auto* self = static_cast<state*>(handle->data);
    if (self == nullptr || self->closed || self->out_events == nullptr) {
      return;
    }

    event_notification notification{};
    notification.watch_path = self->path;
    if (filename != nullptr) {
      notification.filename = filename;
    }
    notification.events = static_cast<unsigned int>(events);
    notification.status = status;
    (void)self->out_events->try_send(std::move(notification));
  }
};

struct poll_watcher::state {
  explicit state(uv_runtime& runtime, message_channel<poll_notification>* out_events)
      : runtime(&runtime), out_events(out_events) {}

  uv_runtime* runtime = nullptr;
  message_channel<poll_notification>* out_events = nullptr;
  uv_fs_poll_t* handle = nullptr;
  std::shared_ptr<state> keep_alive_on_close{};
  bool closing = false;
  bool closed = false;
  std::string path{};

  static void on_poll(uv_fs_poll_t* handle,
                      int status,
                      const uv_stat_t* previous,
                      const uv_stat_t* current) {
    auto* self = static_cast<state*>(handle->data);
    if (self == nullptr || self->closed || self->out_events == nullptr) {
      return;
    }

    poll_notification notification{};
    notification.watch_path = self->path;
    notification.status = status;
    if (previous != nullptr) {
      notification.previous = *previous;
    }
    if (current != nullptr) {
      notification.current = *current;
    }
    (void)self->out_events->try_send(std::move(notification));
  }
};

event_watcher::event_watcher()
    : event_watcher(current_runtime()) {}

event_watcher::event_watcher(uv_runtime& runtime)
    : runtime_(&runtime),
      events_(runtime),
      state_(std::make_shared<state>(runtime, &events_)) {
  state_->handle = runtime_->call([state = state_](uv_loop_t* loop) {
    auto* handle = new uv_fs_event_t{};
    const int rc = uv_fs_event_init(loop, handle);
    if (rc < 0) {
      delete handle;
      throw_uv_error(rc, "uv_fs_event_init");
    }
    handle->data = state.get();
    return handle;
  });
}

event_watcher::~event_watcher() { close(); }

coro::task<void> event_watcher::start(const std::string& path,
                                      watch_flags flags,
                                      std::stop_token stop_token) {
  if (state_ == nullptr || state_->handle == nullptr) {
    throw std::runtime_error("fs event watcher is closed");
  }
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  runtime_->call([state = state_, path, flags](uv_loop_t*) {
    if (state->closed) {
      throw std::runtime_error("fs event watcher is closed");
    }
    if (state->path == path) {
      (void)uv_fs_event_stop(state->handle);
    } else if (!state->path.empty()) {
      (void)uv_fs_event_stop(state->handle);
    }

    const int rc = uv_fs_event_start(
        state->handle,
        &state::on_event,
        path.c_str(),
        static_cast<unsigned int>(flags));
    if (rc < 0) {
      throw_uv_error(rc, "uv_fs_event_start");
    }

    state->path = path;
  });

  path_ = path;
  active_ = true;
  co_return;
}

coro::task<void> event_watcher::stop(std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }
  if (state_ == nullptr || state_->handle == nullptr || !active_) {
    co_return;
  }

  runtime_->call([state = state_](uv_loop_t*) {
    if (!state->closed) {
      const int rc = uv_fs_event_stop(state->handle);
      if (rc < 0) {
        throw_uv_error(rc, "uv_fs_event_stop");
      }
      state->path.clear();
    }
  });

  active_ = false;
  path_.clear();
  co_return;
}

auto event_watcher::next() -> coro::task<event_notification> {
  co_return co_await events_.recv();
}

void event_watcher::close() {
  if (state_ == nullptr || state_->handle == nullptr) {
    if (!events_.is_closed()) {
      events_.close();
    }
    active_ = false;
    path_.clear();
    return;
  }

  auto state = state_;
  uv_fs_event_t* handle = state->handle;
  state->handle = nullptr;
  state->closed = true;
  state->path.clear();
  active_ = false;
  path_.clear();
  events_.close();

  try {
    runtime_->post([state, handle]() {
      if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(handle))) {
        (void)uv_fs_event_stop(handle);
        state->keep_alive_on_close = state;
        uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* handle) {
          auto* self = static_cast<event_watcher::state*>(handle->data);
          delete reinterpret_cast<uv_fs_event_t*>(handle);
          if (self != nullptr) {
            self->keep_alive_on_close.reset();
          }
        });
      } else {
        state->keep_alive_on_close.reset();
      }
    });
  } catch (...) {
  }
}

poll_watcher::poll_watcher()
    : poll_watcher(current_runtime()) {}

poll_watcher::poll_watcher(uv_runtime& runtime)
    : runtime_(&runtime),
      events_(runtime),
      state_(std::make_shared<state>(runtime, &events_)) {
  state_->handle = runtime_->call([state = state_](uv_loop_t* loop) {
    auto* handle = new uv_fs_poll_t{};
    const int rc = uv_fs_poll_init(loop, handle);
    if (rc < 0) {
      delete handle;
      throw_uv_error(rc, "uv_fs_poll_init");
    }
    handle->data = state.get();
    return handle;
  });
}

poll_watcher::~poll_watcher() { close(); }

coro::task<void> poll_watcher::start_impl(const std::string& path,
                                          std::chrono::milliseconds interval,
                                          std::stop_token stop_token) {
  if (state_ == nullptr || state_->handle == nullptr) {
    throw std::runtime_error("fs poll watcher is closed");
  }
  if (interval.count() <= 0) {
    throw std::invalid_argument("fs poll interval must be positive");
  }
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }

  runtime_->call([state = state_, path, interval](uv_loop_t*) {
    if (state->closed) {
      throw std::runtime_error("fs poll watcher is closed");
    }

    (void)uv_fs_poll_stop(state->handle);

    const int rc = uv_fs_poll_start(
        state->handle,
        &state::on_poll,
        path.c_str(),
        static_cast<unsigned int>(interval.count()));
    if (rc < 0) {
      throw_uv_error(rc, "uv_fs_poll_start");
    }

    state->path = path;
  });

  path_ = path;
  interval_ = interval;
  active_ = true;
  co_return;
}

coro::task<void> poll_watcher::stop(std::stop_token stop_token) {
  if (stop_token.stop_requested()) {
    throw operation_cancelled();
  }
  if (state_ == nullptr || state_->handle == nullptr || !active_) {
    co_return;
  }

  runtime_->call([state = state_](uv_loop_t*) {
    if (!state->closed) {
      const int rc = uv_fs_poll_stop(state->handle);
      if (rc < 0) {
        throw_uv_error(rc, "uv_fs_poll_stop");
      }
      state->path.clear();
    }
  });

  active_ = false;
  interval_ = std::chrono::milliseconds(0);
  path_.clear();
  co_return;
}

auto poll_watcher::next() -> coro::task<poll_notification> {
  co_return co_await events_.recv();
}

void poll_watcher::close() {
  if (state_ == nullptr || state_->handle == nullptr) {
    if (!events_.is_closed()) {
      events_.close();
    }
    active_ = false;
    interval_ = std::chrono::milliseconds(0);
    path_.clear();
    return;
  }

  auto state = state_;
  uv_fs_poll_t* handle = state->handle;
  state->handle = nullptr;
  state->closed = true;
  state->path.clear();
  active_ = false;
  interval_ = std::chrono::milliseconds(0);
  path_.clear();
  events_.close();

  try {
    runtime_->post([state, handle]() {
      if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(handle))) {
        (void)uv_fs_poll_stop(handle);
        state->keep_alive_on_close = state;
        uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* handle) {
          auto* self = static_cast<poll_watcher::state*>(handle->data);
          delete reinterpret_cast<uv_fs_poll_t*>(handle);
          if (self != nullptr) {
            self->keep_alive_on_close.reset();
          }
        });
      } else {
        state->keep_alive_on_close.reset();
      }
    });
  } catch (...) {
  }
}

}  // namespace luvcoro::fs
