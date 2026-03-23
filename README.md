# LuvCoro

A sync-style async I/O library built on top of **libcoro + libuv**.

- All I/O is implemented via `libuv` (`uv_fs_*`, `uv_tcp_*`, `uv_timer_*`)
- No use of libcoro networking/io scheduler modules
- Dependencies are pulled from source code directly

## Dependency bootstrap (pixi only as Python env)

```bash
pixi run --manifest-path script python script/bootstrap.py deps
```

Update local deps:

```bash
pixi run --manifest-path script python script/bootstrap.py deps --update
```

This pulls source trees into:

- `third_party/libuv`
- `third_party/libcoro`

You can also run the same script with any Python directly:

```bash
python script/bootstrap.py deps
```

## Build

Root CMake only builds the `luvcoro` library.

Build is fully independent from pixi. CMake only consumes already-present source trees under `third_party/`.

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

## Build examples

Examples have their own CMake project under `examples/` and pull the library in
through `add_subdirectory(..)`.

```bash
cmake -S examples -B build/examples -G Ninja
cmake --build build/examples
```

## Examples

```bash
./build/examples/luvcoro_echo_client
```

`examples/tcp_echo_client.cpp` connects to `127.0.0.1:7001`, sends one line, and reads one echoed chunk.

Additional examples:

- `luvcoro_tcp_echo_server`: manual TCP echo server for `luvcoro_echo_client`
- `luvcoro_tcp_roundtrip`: self-contained TCP smoke test in one process
- `luvcoro_blocking_roundtrip`: offload blocking work to libuv's thread pool
- `luvcoro_blocking_parallel`: run multiple blocking jobs concurrently on libuv's thread pool
- `luvcoro_blocking_cancel`: cancel queued blocking jobs and cooperatively stop running ones
- `luvcoro_dns_lookup`: resolve and reverse-lookup hostnames through libuv
- `luvcoro_pipe_roundtrip`: named-pipe / local-socket stream roundtrip over libuv pipes
- `luvcoro_process_roundtrip`: spawn a child process with piped stdin/stdout and wait for exit
- `luvcoro_message_roundtrip`: cross-thread message passing through libuv async wakeups
- `luvcoro_message_bounded`: bounded channel backpressure plus `recv_for(...)` timeout checks
- `luvcoro_message_pressure`: multi-producer/multi-consumer stress test over a bounded channel
- `luvcoro_message_iteration`: consume a channel with blocking range-for syntax
- `luvcoro_message_async_iteration`: consume a channel with generator-style `co_await` iteration
- `luvcoro_message_async_foreach`: consume a channel with `luvcoro::co_foreach(...)`
- `luvcoro_message_async_foreach_parallel`: process channel messages with bounded async concurrency
- `luvcoro_message_async_timeout_iteration`: stop generator-style iteration after a receive timeout
- `luvcoro_message_async_cancel`: stop generator-style iteration with `std::stop_source`
- `luvcoro_message_select`: wait on multiple channels and pick the first ready one
- `luvcoro_timer_roundtrip`: one-shot waits, `sleep_until(...)`, and cancellable `steady_timer`
- `luvcoro_signal_roundtrip`: wait for process signals through a libuv-backed `signal_set`
- `luvcoro_sync_roundtrip`: coordinate coroutines with `async_mutex`, `async_condition_variable`, and `async_semaphore`
- `luvcoro_local_sync_roundtrip`: use single-runtime-thread `local_*` synchronization primitives without internal system locks
- `luvcoro_tty_roundtrip`: inspect stdout as a tty and write through a `tty_stream`
- `luvcoro_stdio_roundtrip`: open stdout through a `stdio_stream` that auto-detects tty/pipe/file
- `luvcoro_cancel_timeout`: cooperative cancellation plus timeout/deadline control helpers
- `luvcoro_structured_scope`: structured concurrency with sibling cancellation on failure
- `luvcoro_spawn_detached`: start a fire-and-forget coroutine from a normal sync function
- `luvcoro_spawn_logged`: start a detached coroutine and log failures through a handler
- `luvcoro_spawn_timer`: schedule detached coroutines with relative or absolute timer delays
- `luvcoro_spawn_periodic`: compare fixed-delay, immediate, and fixed-rate periodic tasks
- `luvcoro_io_cancel`: cancel pending TCP/UDP/timer work through one shared stop source
- `luvcoro_udp_echo_server`: manual UDP echo server
- `luvcoro_udp_echo_client`: UDP client for the echo server
- `luvcoro_udp_multicast_sender`: send one multicast datagram to `239.1.2.3:7204`
- `luvcoro_udp_multicast_receiver`: join `239.1.2.3:7204` and receive one datagram
- `luvcoro_udp_roundtrip`: self-contained UDP smoke test in one process
- `luvcoro_fs_roundtrip`: write/read/verify a temp file plus stat/rename/copy/link operations
- `luvcoro_fs_watch_roundtrip`: watch a directory and a file with libuv fs event/poll watchers
- `luvcoro_sendfile_roundtrip`: move bytes between open files with `uv_fs_sendfile`

## Core API

- `luvcoro::ip_endpoint`
- `luvcoro::mutable_buffer` / `luvcoro::const_buffer` / `luvcoro::buffer(...)`
- `luvcoro::dns::resolve(...)` / `luvcoro::dns::reverse_lookup(...)`
- `luvcoro::pipe_client` (`connect/write/read_some/read_exact/shutdown/close`)
- `luvcoro::pipe_server` (`bind/listen/accept/close`)
- `luvcoro::process` (`spawn/wait/kill/stdio pipes`)
- `luvcoro::process_options`
- `luvcoro::process_exit_status`
- `luvcoro::uv_runtime`
- `luvcoro::uv_runtime_options`
- `luvcoro::runtime_scope`
- `luvcoro::run_with_runtime(...)`
- `luvcoro::blocking(...)`
- `luvcoro::spawn(...)`
- `luvcoro::spawn_logged(...)`
- `luvcoro::spawn_after(...)` / `luvcoro::spawn_at(...)`
- `luvcoro::spawn_logged_after(...)` / `luvcoro::spawn_logged_at(...)`
- `luvcoro::periodic_task`
- `luvcoro::spawn_every(...)` / `luvcoro::spawn_logged_every(...)`
- `luvcoro::spawn_every_immediate(...)` / `luvcoro::spawn_logged_every_immediate(...)`
- `luvcoro::spawn_every_fixed_rate(...)` / `luvcoro::spawn_logged_every_fixed_rate(...)`
- `luvcoro::wait_for_stop(...)`
- `luvcoro::message_channel<T>` (`send/try_send/async_send/recv/recv_for/try_recv/close`)
- `luvcoro::sleep_for(...)` / `luvcoro::sleep_until(...)`
- `luvcoro::steady_timer`
- `luvcoro::signal_set` (`add/remove/clear/next/next_for/signals/close`)
- `luvcoro::async_mutex` / `luvcoro::async_mutex_guard`
- `luvcoro::async_condition_variable`
- `luvcoro::async_semaphore`
- `luvcoro::local_async_mutex` / `luvcoro::local_async_mutex_guard`
- `luvcoro::local_async_condition_variable`
- `luvcoro::local_async_semaphore`
- `luvcoro::local_async_event`
- `luvcoro::tty_stream`
- `luvcoro::stdio_stream`
- `luvcoro::guess_handle(...)` / `luvcoro::is_tty(...)` / `luvcoro::reset_tty_mode(...)`
- `luvcoro::with_stop_token(...)` / `luvcoro::with_timeout(...)` / `luvcoro::with_deadline(...)`
- `luvcoro::task_scope` / `luvcoro::run_scoped(...)`
- `luvcoro::file` (`open/read/write/read_at/write_at/read_all/truncate/seek/tell/close`)
- `luvcoro::fs::file_status`
- `luvcoro::fs::directory_entry`
- `luvcoro::fs` (`stat/lstat/exists/is_directory/file_size/list_directory/create_directory/create_directories/rename/copy_file/create_hard_link/create_symbolic_link/read_link/real_path/chmod/remove_file/remove_directory/remove_all/read_file/write_file/append_file/sendfile`)
- `luvcoro::fs::event_watcher` (`start/stop/next/next_for/events/close`)
- `luvcoro::fs::poll_watcher` (`start/stop/next/next_for/events/close`)
- `luvcoro::tcp_client` (`connect/write/read_some/read_exact/shutdown/set_nodelay/set_keepalive/local_endpoint/remote_endpoint/close`)
- `luvcoro::tcp_server` (`bind/set_simultaneous_accepts/listen/accept/local_endpoint/close`)
- `luvcoro::udp_socket` (`bind/set_broadcast/set_multicast_loop/set_multicast_ttl/set_multicast_interface/join_multicast_group/leave_multicast_group/send_to/recv_from/close`)
- `luvcoro::io` (`read_some/read_some_for/read_exact/read_exact_for/read_exactly/read_until/read_until_for/read_until_any/read_until_any_for/read_until_any_result/read_line/write/write_for/write_all/read_some_into/read_exact_into/read_exactly_into/write_some/copy/copy_n/send_to/receive/receive_for/receive_into/receive_into_for`)

## Runtime binding

If you don't want to thread `uv_runtime&` through every constructor, bind one runtime to the current thread:

```cpp
luvcoro::uv_runtime runtime;
luvcoro::runtime_scope scope(runtime);
```

After that, `luvcoro::file`, `luvcoro::tcp_client`, `luvcoro::tcp_server`, `luvcoro::udp_socket`,
`luvcoro::sleep_for(...)`, `luvcoro::sleep_until(...)`, and `luvcoro::steady_timer`
can use the current runtime implicitly.

If you want the binding to be automatic for one call site, use:

```cpp
luvcoro::uv_runtime runtime;
luvcoro::run_with_runtime(runtime, some_task());
```

If you need to configure libuv's worker pool, pass `uv_runtime_options` when
creating the runtime:

```cpp
luvcoro::uv_runtime_options options;
options.thread_pool_size = 8;

luvcoro::uv_runtime runtime(options);
```

This controls libuv's global thread pool, which is shared across all loops, so
set it before your process starts using threadpool-backed operations.

To offload blocking work onto that pool:

```cpp
auto value = co_await luvcoro::blocking([]() {
  return expensive_blocking_call();
});
```

`blocking(...)` also accepts a `std::stop_token` as its first argument after the
runtime, matching the rest of the library:

```cpp
std::stop_source stop_source;

co_await luvcoro::blocking(
    stop_source.get_token(),
    [](std::stop_token stop_token) {
      while (!stop_token.stop_requested()) {
        do_some_cpu_work();
      }
      throw luvcoro::operation_cancelled();
    });
```

Queued thread-pool work can usually be cancelled through `uv_cancel(...)`.
Once a blocking task has already started running on a worker thread, stopping it
is cooperative: the callable needs to accept and observe `std::stop_token`.

And if you want to fan out multiple blocking jobs in parallel:

```cpp
co_await coro::when_all(
    luvcoro::blocking([] { do_job_a(); }),
    luvcoro::blocking([] { do_job_b(); }),
    luvcoro::blocking([] { do_job_c(); }));
```

If you need to launch a detached coroutine from an ordinary synchronous
function, use `spawn(...)`:

```cpp
coro::task<void> background_job() {
  co_await luvcoro::sleep_for(std::chrono::milliseconds(10));
}

void kick_background_job() {
  luvcoro::spawn(background_job());
}
```

`spawn(...)` is fire-and-forget:

- it starts the coroutine on the bound/runtime loop
- it intentionally does not propagate coroutine exceptions to the caller
- it returns `false` only if there is no current runtime bound, or the runtime
  is already shutting down

If you want detached failures to be visible without rethrowing into the caller,
use `spawn_logged(...)`:

```cpp
luvcoro::spawn_logged(
    "background job",
    background_job(),
    [](std::string_view name, std::exception_ptr exception) noexcept {
      try {
        if (exception != nullptr) {
          std::rethrow_exception(exception);
        }
      } catch (const std::exception& ex) {
        std::cerr << name << ": " << ex.what() << std::endl;
      }
    });
```

If you omit the handler, `spawn_logged(...)` prints a simple message to `stderr`.

If you want to schedule detached work to start later, use `spawn_after(...)`
or `spawn_at(...)`:

```cpp
luvcoro::spawn_after(
    std::chrono::milliseconds(25),
    []() -> coro::task<void> {
      co_await do_background_work();
    });

luvcoro::spawn_at(
    std::chrono::steady_clock::now() + std::chrono::milliseconds(50),
    []() -> coro::task<void> {
      co_await do_background_work();
    });
```

The logged variants behave the same way, but route detached failures through the
same handler model as `spawn_logged(...)`:

```cpp
luvcoro::spawn_logged_after(
    "delayed background job",
    std::chrono::milliseconds(25),
    []() -> coro::task<void> {
      throw std::runtime_error("boom");
      co_return;
    });
```

If you want a detached coroutine to run repeatedly, use `spawn_every(...)`.
It returns a `periodic_task` handle that you can cancel later:

```cpp
auto ticker = luvcoro::spawn_every(
    std::chrono::milliseconds(100),
    [](std::stop_token stop_token) -> coro::task<void> {
      if (stop_token.stop_requested()) {
        co_return;
      }
      co_await do_periodic_work();
    });

ticker.cancel();
```

`spawn_every(...)` waits one interval before each tick, then invokes your
callable again until you cancel the returned handle or the callable throws.

If you want exceptions from that periodic detached task to be visible, use
`spawn_logged_every(...)`.

If you want the first tick to run immediately instead of waiting one interval,
use `spawn_every_immediate(...)`.

If you want ticks aligned to absolute time slots instead of drifting by the
task body's execution time, use `spawn_every_fixed_rate(...)`. The fixed-rate
variant skips past missed slots and resumes on the next future deadline.

For captured arguments, prefer a normal coroutine function or a non-coroutine
lambda that returns a task, instead of a directly-invoked capturing coroutine
lambda.

If you need a one-shot time point wait instead of a relative delay:

```cpp
co_await luvcoro::sleep_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(25));
```

And if you want a reusable timer object:

```cpp
luvcoro::steady_timer timer;
timer.expires_after(std::chrono::milliseconds(250));
co_await timer.wait();
```

You can cancel that timer from another coroutine or thread:

```cpp
timer.cancel();
```

For process-level signals, use `signal_set` and consume them with the same
channel-style API shape as the rest of the library:

```cpp
luvcoro::signal_set signals;
co_await signals.add(SIGINT);

auto notification = co_await signals.next_for(std::chrono::seconds(5));
if (notification && notification->signum == SIGINT) {
  // begin shutdown
}
```

On Windows, signal delivery depends on console control events, so automated
signal smoke tests can be more environment-sensitive than the rest of the
examples.

For cooperative cancellation and timeout control, pass a `std::stop_token`
through your coroutine body:

```cpp
co_await luvcoro::with_timeout(
    std::chrono::milliseconds(50),
    [](std::stop_token stop_token) -> coro::task<void> {
      while (true) {
        co_await luvcoro::sleep_for(std::chrono::milliseconds(250), stop_token);
      }
    });
```

If the timeout expires first, `with_timeout(...)` throws `luvcoro::operation_timeout`.
If an external `std::stop_source` wins first, `with_stop_token(...)` throws
`luvcoro::operation_cancelled`.

That model is cooperative: the wrapped operation needs to observe the provided
`std::stop_token` for cancellation to stop work promptly.

For coroutine-to-coroutine coordination inside the same runtime, there are
also async synchronization primitives:

```cpp
luvcoro::async_mutex mutex;
luvcoro::async_condition_variable cv;
luvcoro::async_semaphore done(0);

auto lock = co_await mutex.scoped_lock();
co_await cv.wait(mutex, [] { return ready_flag; });
lock.unlock();
done.release();
```

If you know a primitive will only ever be touched from the bound runtime
thread, use the lighter `local_*` variants instead. They keep the same
coroutine-facing shape but avoid internal system locks:

```cpp
luvcoro::local_async_mutex mutex;
luvcoro::local_async_condition_variable cv;
luvcoro::local_async_event ready;
luvcoro::local_async_semaphore done(0);

auto lock = co_await mutex.scoped_lock();
co_await cv.wait(mutex, [] { return ready_flag; });
lock.unlock();

ready.set();
co_await done.acquire();
```

Those `local_*` primitives are intentionally single-runtime-thread tools:

- call them only from the loop thread bound to that `uv_runtime`
- use the regular `async_*` primitives when other threads may notify, release,
  or contend directly

For console-style standard streams, use `tty_stream` when `uv_guess_handle(...)`
reports a tty:

```cpp
if (luvcoro::is_tty(1)) {
  luvcoro::tty_stream tty;
  co_await tty.open_stdout();

  auto size = tty.winsize();
  co_await tty.write("hello tty\n");
}
```

If you want one wrapper that works whether standard I/O is attached to a tty,
pipe, or redirected file, use `stdio_stream` instead:

```cpp
luvcoro::stdio_stream out;
co_await out.open_stdout();

co_await out.write("hello stdio\n");
```

`stdio_stream` dispatches to `tty_stream`, `pipe_client`, or `file`
automatically based on `uv_guess_handle(...)`.

The same control helpers also work with thread-pool offloaded blocking work:

```cpp
co_await luvcoro::with_timeout(
    std::chrono::milliseconds(50),
    [](std::stop_token stop_token) -> coro::task<void> {
      co_await luvcoro::blocking(stop_token, [](std::stop_token inner_stop_token) {
        while (!inner_stop_token.stop_requested()) {
          do_some_cpu_work();
        }
        throw luvcoro::operation_cancelled();
      });
    });
```

That gives `blocking(...)` the same cancellation and timeout entry points as
the rest of the library. If the blocking job is already running on a libuv
worker, it still needs to observe the token cooperatively.

For sibling-task lifetimes, use `task_scope`:

```cpp
luvcoro::task_scope scope;
scope.spawn([](std::stop_token stop_token) -> coro::task<void> {
  co_await do_child_work(stop_token);
});
scope.spawn([](std::stop_token stop_token) -> coro::task<void> {
  co_await do_other_child_work(stop_token);
});
co_await scope.join();
```

If one child throws, `task_scope` requests stop on the sibling tasks, waits for
them to finish, and then rethrows the first failure from `join()`.

All async I/O entry points also accept `std::stop_token` as their last
parameter. That includes DNS, TCP, UDP, file operations, and the higher-level
helpers in `luvcoro::io`.

For stream-style IPC, libuv pipes are wrapped with the same read/write shape as
TCP streams:

```cpp
luvcoro::pipe_server server;
co_await server.bind(pipe_name);
co_await server.listen();

auto client = co_await luvcoro::accept_for(server, std::chrono::seconds(2));
co_await client.write("hello\n");
co_await client.shutdown();
```

That means `luvcoro::io::make_stream_buffer(...)`, `read_until(...)`, and the
other stream helpers work unchanged on pipes.

For child process management, use `process` with optional piped stdio:

```cpp
luvcoro::process proc;
luvcoro::process_options options;
options.file = program_path;
options.args = {"--worker"};
options.stdio.stdin_pipe = true;
options.stdio.stdout_pipe = true;

co_await proc.spawn(options);
co_await proc.stdin_pipe()->write("work\n");
co_await proc.stdin_pipe()->shutdown();

auto reader = luvcoro::io::make_stream_buffer(*proc.stdout_pipe());
auto line = co_await luvcoro::io::read_line(reader);
auto status = co_await proc.wait();
```

That gives you one cancellation style across the library:

```cpp
std::stop_source stop_source;

co_await client.connect(endpoint, stop_source.get_token());
auto data = co_await socket.recv_from(4096, stop_source.get_token());
auto text = co_await file.read_all(stop_source.get_token());
```

`task_scope::join(...)` also accepts a `std::stop_token`. If that external stop
token fires first, the scope requests stop on all child tasks, waits for them
to finish, and then throws `luvcoro::operation_cancelled`.

If you need to push messages into the runtime from another thread, use
`message_channel<T>`, which is backed by `uv_async_t` for loop-thread wakeups:

```cpp
luvcoro::message_channel<std::string> channel;

co_await luvcoro::blocking([&]() {
  channel.send("hello from worker");
});

auto message = co_await channel.recv();
```

If you want a bounded channel with backpressure, pass a capacity and use
`async_send(...)` inside coroutines:

```cpp
luvcoro::message_channel<std::string> channel(16);

co_await channel.async_send("payload");
auto message = co_await channel.recv_for(std::chrono::milliseconds(100));
```

`send(...)` is still available for external threads and will block when a
bounded queue is full. If you call it on the runtime thread and it would block,
it throws so you can switch to `async_send(...)` instead.

If you are on a non-runtime thread and want blocking iteration, `message_channel`
also supports range-for directly:

```cpp
for (const auto& message : channel) {
  handle(message);
}
```

That form keeps pulling messages until the channel is closed and drained.

If you want coroutine-friendly generator style, use `channel.messages()`:

```cpp
for (auto next_message : channel.messages()) {
  auto message = co_await next_message;
  if (!message.has_value()) {
    break;
  }

  handle(*message);
}
```

Because this is standard C++20 coroutine machinery rather than a language-level
`for co_await`, each generator item is itself awaitable.

If you want the same style but with an idle timeout, use `messages_for(...)`:

```cpp
for (auto next_message : channel.messages_for(std::chrono::milliseconds(50))) {
  auto message = co_await next_message;
  if (!message.has_value()) {
    break;
  }

  handle(*message);
}
```

And if you want a small helper instead of writing the loop manually:

```cpp
co_await luvcoro::co_foreach(
    channel.messages(),
    [](std::string message) -> coro::task<void> {
      handle(message);
      co_return;
    });
```

If you want to stop iteration from a `std::stop_source`, use `messages_until(...)`:

```cpp
std::stop_source stop_source;

for (auto next_message : channel.messages_until(
         stop_source.get_token(), std::chrono::milliseconds(25))) {
  auto message = co_await next_message;
  if (!message.has_value()) {
    break;
  }

  handle(*message);
}
```

If you want bounded callback concurrency, pass a window size to `co_foreach(...)`:

```cpp
co_await luvcoro::co_foreach(
    channel.messages(),
    4,
    [](payload_type message) -> coro::task<void> {
      co_await process_async(message);
    });
```

And if you want to wait on multiple channels at once, use `select(...)`:

```cpp
auto result = co_await luvcoro::select(numbers, words);
```

For DNS-style work backed by libuv:

```cpp
auto endpoints = co_await luvcoro::dns::resolve("localhost", 7001);
auto info = co_await luvcoro::dns::reverse_lookup(endpoints.front());
```

## Unified style

Stream-like objects (`luvcoro::file`, `luvcoro::tcp_client`) can be used directly through the same helper layer:

```cpp
auto chunk = co_await luvcoro::io::read_some(file_or_tcp_client, 4096);
auto data = co_await luvcoro::io::read_exact(file_or_tcp_client, 128);
co_await luvcoro::io::write_all(file_or_tcp_client, "hello");
```

If you want to pass a wrapped handle around explicitly, `as_stream(...)` / `as_datagram_socket(...)`
and the buffered variants are still available.

For delimiter-based protocols, create a buffered stream wrapper so extra bytes can be retained safely:

```cpp
auto reader = luvcoro::io::make_stream_buffer(tcp_client);
auto line = co_await luvcoro::io::read_until(reader, "\n");
co_await luvcoro::io::write_all(tcp_client, line);
```

If you want line-oriented behavior directly, use `read_line(...)`:

```cpp
auto reader = luvcoro::io::make_stream_buffer(tcp_client);
auto line = co_await luvcoro::io::read_line(reader);
```

If your protocol accepts multiple delimiters, use `read_until_any(...)`:

```cpp
constexpr std::string_view delimiters[] = {"\r\n", "\n"};
auto reader = luvcoro::io::make_stream_buffer(tcp_client);
auto line = co_await luvcoro::io::read_until_any(reader, delimiters);
```

If you also want to know which delimiter matched, use `read_until_any_result(...)`:

```cpp
constexpr std::string_view delimiters[] = {"\r\n", "\n"};
auto reader = luvcoro::io::make_stream_buffer(tcp_client);
auto result = co_await luvcoro::io::read_until_any_result(reader, delimiters);
```

If you need strict fixed-size reads, use `read_exactly(...)`:

```cpp
auto header = co_await luvcoro::io::read_exactly(tcp_client, 16);
```

For strict fixed-size reads into an existing buffer:

```cpp
char header[16];
co_await luvcoro::io::read_exactly_into(file_or_tcp_client, luvcoro::buffer(header));
```

Datagram-style sockets work the same way:

```cpp
co_await luvcoro::io::send_to(udp_socket, endpoint, "ping");
auto datagram = co_await luvcoro::io::receive(udp_socket);
```

For lower-allocation workflows, there is also a buffer-oriented API:

```cpp
char buffer[4096];
auto nread = co_await luvcoro::io::read_some_into(file_or_tcp_client, luvcoro::buffer(buffer));
auto nwritten = co_await luvcoro::io::write_some(file_or_tcp_client, luvcoro::buffer(buffer, nread));

char packet[1500];
auto recv = co_await luvcoro::io::receive_into(udp_socket, luvcoro::buffer(packet));
co_await luvcoro::io::send_some_to(udp_socket, recv.peer, luvcoro::buffer(packet, recv.size));
```

`udp_socket` also exposes common broadcast and multicast controls:

```cpp
co_await socket.set_broadcast(true);
co_await socket.set_multicast_loop(true);
co_await socket.set_multicast_ttl(8);
co_await socket.set_multicast_interface("0.0.0.0");
co_await socket.join_multicast_group("239.1.2.3");
co_await socket.leave_multicast_group("239.1.2.3");
```

You can also compose buffered streams with `copy(...)`:

```cpp
luvcoro::file source;
luvcoro::file target;
co_await source.open("in.txt");
co_await target.open("out.txt", UV_FS_O_CREAT | UV_FS_O_TRUNC | UV_FS_O_WRONLY, 0644);
co_await luvcoro::io::copy(source, target);
co_await luvcoro::io::copy_n(source, target, 1024);
```

For file-system notifications on top of libuv, use `fs::event_watcher` for
native change events and `fs::poll_watcher` when you want stat-based polling:

```cpp
luvcoro::fs::event_watcher dir_watch;
co_await dir_watch.start("tmp");

auto change = co_await dir_watch.next_for(std::chrono::seconds(2));
if (change && change->changed()) {
  // react to directory activity
}

luvcoro::fs::poll_watcher file_watch;
co_await file_watch.start("tmp/data.txt", std::chrono::milliseconds(100));
```

If you already have open `luvcoro::file` handles and want the libuv sendfile
path instead of a buffered copy loop, use `fs::sendfile(...)` or the forwarded
`io::sendfile(...)` helper:

```cpp
luvcoro::file source;
luvcoro::file target;

co_await source.open("in.bin", UV_FS_O_RDONLY);
co_await target.open("out.bin", UV_FS_O_CREAT | UV_FS_O_TRUNC | UV_FS_O_WRONLY, 0644);

auto transferred = co_await luvcoro::io::sendfile(target, source, 64 * 1024);
```

For file metadata and path operations, stay inside the same `luvcoro::fs`
namespace instead of dropping down to platform APIs:

```cpp
auto info = co_await luvcoro::fs::stat("data.txt");
auto entries = co_await luvcoro::fs::list_directory("data");
co_await luvcoro::fs::rename("data.txt", "data.old.txt");
co_await luvcoro::fs::copy_file("data.old.txt", "data.txt");
```

And when the platform allows it, you can also work with links and resolved paths:

```cpp
co_await luvcoro::fs::create_hard_link("data.txt", "data.link.txt");
co_await luvcoro::fs::create_symbolic_link("data.txt", "data.sym.txt");
auto target = co_await luvcoro::fs::read_link("data.sym.txt");
auto absolute = co_await luvcoro::fs::real_path("data.txt");
```

Network addresses can be passed around as `luvcoro::ip_endpoint`:

```cpp
luvcoro::ip_endpoint ep{"127.0.0.1", 7001};
co_await client.connect(ep);
co_await server.bind(ep);
co_await socket.send_to(ep, "ping");
```

## Cross-platform note

You can develop on Windows and run on Linux. Keep your business logic inside coroutines and rely on `libuv` operations only.

