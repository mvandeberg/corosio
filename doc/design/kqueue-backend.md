# kqueue Backend Design Document

## 1. Overview

This document describes the design for adding a kqueue-based I/O backend to
corosio, targeting macOS and FreeBSD (with OpenBSD, NetBSD, and DragonFlyBSD
also supported). The kqueue backend follows the same single-reactor thread
model used by the epoll and select backends, providing behavioral parity
across all POSIX platforms.

kqueue is the closest analog to epoll: both are O(1) event notification
mechanisms with edge-triggered support, persistent fd registration, and
efficient reactor wakeup. The implementation will mirror the epoll backend's
architecture almost line-for-line, with syscall-level substitutions.

## 2. File Inventory

### Public Headers (new)

| File | Purpose |
|---|---|
| `include/boost/corosio/kqueue_context.hpp` | Public context class, mirrors `epoll_context.hpp` |

### Public Headers (modified)

| File | Change |
|---|---|
| `include/boost/corosio/io_context.hpp` | Uncomment `kqueue_context` include and alias |

### Implementation Files (new)

| File | Purpose |
|---|---|
| `src/corosio/src/detail/kqueue/scheduler.hpp` | `kqueue_scheduler` class declaration |
| `src/corosio/src/detail/kqueue/scheduler.cpp` | `kqueue_scheduler` implementation |
| `src/corosio/src/detail/kqueue/sockets.hpp` | `kqueue_socket_impl`, `kqueue_socket_service` |
| `src/corosio/src/detail/kqueue/sockets.cpp` | Socket operation implementations |
| `src/corosio/src/detail/kqueue/acceptors.hpp` | `kqueue_acceptor_impl`, `kqueue_acceptor_service` |
| `src/corosio/src/detail/kqueue/acceptors.cpp` | Acceptor operation implementations |
| `src/corosio/src/detail/kqueue/op.hpp` | Operation state types (`kqueue_op`, etc.) |
| `src/corosio/src/kqueue_context.cpp` | `kqueue_context` constructor/destructor |

### Implementation Files (modified)

| File | Change |
|---|---|
| `include/boost/corosio/detail/platform.hpp` | Already detects kqueue; no changes needed |

### Build Files (modified)

| File | Change |
|---|---|
| `CMakeLists.txt` | Sources auto-discovered via `GLOB_RECURSE`; no changes needed |
| `build/Jamfile` | Sources auto-discovered via `glob-tree-ex`; no changes needed |

**Note:** Both build systems use recursive globbing, so new source files
in `src/corosio/src/detail/kqueue/` will be picked up automatically. No
build file modifications are required.

## 3. Architecture

### 3.1 Class Hierarchy

```
scheduler (abstract, detail/scheduler.hpp)
  |
  +-- capy::execution_context::service
  |
  +-- kqueue_scheduler (detail/kqueue/scheduler.hpp)

socket_service (abstract, detail/socket_service.hpp)
  |
  +-- kqueue_socket_service (detail/kqueue/sockets.hpp)

acceptor_service (abstract, detail/socket_service.hpp)
  |
  +-- kqueue_acceptor_service (detail/kqueue/acceptors.hpp)

tcp_socket::socket_impl (abstract, tcp_socket.hpp)
  |
  +-- kqueue_socket_impl (detail/kqueue/sockets.hpp)

tcp_acceptor::acceptor_impl (abstract, tcp_acceptor.hpp)
  |
  +-- kqueue_acceptor_impl (detail/kqueue/acceptors.hpp)

scheduler_op (abstract, detail/scheduler_op.hpp)
  |
  +-- kqueue_op (detail/kqueue/op.hpp)
      |
      +-- kqueue_connect_op
      +-- kqueue_read_op
      +-- kqueue_write_op
      +-- kqueue_accept_op
```

### 3.2 Service Registration

`kqueue_context` constructor installs services:

```cpp
kqueue_context::kqueue_context(unsigned concurrency_hint)
{
    auto& sched = make_service<kqueue_scheduler>(*this, concurrency_hint);
    sched_ = &sched;
    make_service<kqueue_socket_service>(*this);
    make_service<kqueue_acceptor_service>(*this);
}
```

This mirrors `epoll_context` exactly. The `key_type = socket_service` on
`kqueue_socket_service` enables `use_service<socket_service>()` to find it.

### 3.3 Thread Model

Identical to the epoll backend:

1. **One reactor thread** calls `kevent()` (equivalent of `epoll_wait()`)
2. **Other threads** wait on `std::condition_variable` for handler work
3. **`wake_one_thread_and_unlock()`** wakes exactly one thread per posted item
4. No thundering herd; matches IOCP semantics

## 4. Syscall Mapping: epoll to kqueue

| Concept | epoll | kqueue |
|---|---|---|
| Create instance | `epoll_create1(EPOLL_CLOEXEC)` | `kqueue()` + `fcntl(F_SETFD, FD_CLOEXEC)` |
| Wait for events | `epoll_wait(epfd, events, max, timeout_ms)` | `kevent(kq, NULL, 0, events, max, &timeout)` |
| Register fd | `epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev)` | `kevent(kq, changelist, nchanges, NULL, 0, NULL)` |
| Modify fd | `epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev)` | Same as register (re-add with new flags) |
| Deregister fd | `epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL)` | `EV_DELETE` in changelist |
| Edge-triggered | `EPOLLET` | `EV_CLEAR` |
| Read readiness | `EPOLLIN` | `EVFILT_READ` |
| Write readiness | `EPOLLOUT` | `EVFILT_WRITE` |
| Error | `EPOLLERR \| EPOLLHUP` | `EV_EOF` flag + `fflags` for error |
| Reactor wakeup | `eventfd` + `EPOLLIN \| EPOLLET` | `EVFILT_USER` + `NOTE_TRIGGER` |
| Timeout format | `int` (milliseconds) | `struct timespec` (seconds + nanoseconds) |

### 4.1 Key Structural Difference: Changelist

epoll uses individual `epoll_ctl()` calls per fd. kqueue batches
registrations via a changelist passed to `kevent()`. This is an
optimization opportunity but not a requirement. The initial
implementation should use separate `kevent()` calls for registration
(changelist with nchanges=1) to match the epoll backend's structure.

A future optimization could batch pending registrations into the
`kevent()` wait call's changelist parameter, avoiding extra syscalls.

### 4.2 Key Structural Difference: Per-Filter Events

epoll monitors a single fd with a combined event mask (`EPOLLIN | EPOLLOUT`).
kqueue uses separate filter registrations (`EVFILT_READ` and `EVFILT_WRITE`
are independent entries). Registration requires two `kevent` calls (or
one call with a changelist of size 2) per fd:

```c
struct kevent changes[2];
EV_SET(&changes[0], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, udata);
EV_SET(&changes[1], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, udata);
kevent(kq, changes, 2, NULL, 0, NULL);
```

### 4.3 Timeout Conversion

epoll uses millisecond `int` timeouts. kqueue uses `struct timespec*`
(NULL = block forever, {0,0} = poll). The conversion from microseconds:

```cpp
struct timespec ts;
if (timeout_us < 0) {
    ts_ptr = nullptr;  // block forever
} else {
    ts.tv_sec = timeout_us / 1000000;
    ts.tv_nsec = (timeout_us % 1000000) * 1000;
    ts_ptr = &ts;
}
```

This is more precise than epoll's millisecond resolution.

## 5. Descriptor State and Operation Model

### 5.1 Persistent Registration (Same as epoll)

Like the epoll backend, fds are registered once and stay registered until
close. This avoids repeated `kevent()` calls for each operation. The
`EV_CLEAR` flag provides edge-triggered semantics equivalent to `EPOLLET`.

### 5.2 `descriptor_data` (Reusable from epoll, with minor changes)

```cpp
struct descriptor_data
{
    std::atomic<kqueue_op*> read_op{nullptr};
    std::atomic<kqueue_op*> write_op{nullptr};
    std::atomic<kqueue_op*> connect_op{nullptr};
    std::atomic<bool> read_ready{false};
    std::atomic<bool> write_ready{false};
    int fd = -1;
    bool is_registered = false;
};
```

This is identical to the epoll `descriptor_data`. The `read_ready` and
`write_ready` flags cache edge events that arrive before an operation is
registered (same race window as epoll + `EPOLLET`).

### 5.3 Operation Flow (Same as epoll)

1. Socket operation (e.g., `read_some`) tries non-blocking I/O immediately
2. If EAGAIN: store op pointer in `descriptor_data.read_op` via atomic
3. Check `read_ready` flag (in case event fired before op was stored)
4. If ready: claim op via atomic exchange, perform I/O, post to scheduler
5. If not ready: wait for `kevent()` to deliver `EVFILT_READ`
6. Reactor claims op via atomic exchange, calls `perform_io()`, posts result

### 5.4 Cancellation (Same as epoll)

Uses `std::stop_token` + `std::stop_callback`. Cancellation sets the
`cancelled` atomic and claims the op from `descriptor_data` via atomic
exchange. Whoever wins the exchange (reactor or cancel) is responsible
for posting the op to the completion queue.

## 6. Reactor Wakeup Mechanism

### 6.1 Primary: `EVFILT_USER`

`EVFILT_USER` is the kqueue equivalent of Linux's `eventfd`. It provides
zero-overhead user-triggered events without kernel resources.

**Registration (constructor):**
```cpp
struct kevent ev;
EV_SET(&ev, KQUEUE_WAKEUP_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
kevent(kq_, &ev, 1, nullptr, 0, nullptr);
```

`KQUEUE_WAKEUP_IDENT` is an arbitrary identifier (e.g., 0). The `EV_CLEAR`
flag ensures the event resets after delivery (edge-triggered).

**Trigger (interrupt_reactor):**
```cpp
struct kevent ev;
EV_SET(&ev, KQUEUE_WAKEUP_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
kevent(kq_, &ev, 1, nullptr, 0, nullptr);
```

**Detection (run_reactor):**
```cpp
if (events[i].filter == EVFILT_USER) {
    // Reactor was interrupted; continue loop
    continue;
}
```

### 6.2 Fallback: Self-Pipe

There are reports of `EVFILT_USER` not reliably waking a concurrent
`kevent()` call on some FreeBSD versions. If this proves to be an issue
during testing, a self-pipe fallback (identical to the select backend's
approach) can be used instead:

```cpp
pipe(pipe_fds_);
// Set non-blocking, close-on-exec
// Register pipe_fds_[0] with EVFILT_READ
```

**Recommendation:** Start with `EVFILT_USER`. It works on macOS and modern
FreeBSD. Only add the self-pipe fallback if testing reveals reliability
issues on target platforms.

### 6.3 Deduplication

The epoll backend uses an atomic `eventfd_armed_` flag with
`compare_exchange_strong` to avoid redundant `eventfd` writes. The kqueue
backend should use the same pattern for `EVFILT_USER` triggers:

```cpp
void interrupt_reactor() const
{
    bool expected = false;
    if (wakeup_armed_.compare_exchange_strong(expected, true,
            std::memory_order_release, std::memory_order_relaxed))
    {
        struct kevent ev;
        EV_SET(&ev, KQUEUE_WAKEUP_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
        kevent(kq_, &ev, 1, nullptr, 0, nullptr);
    }
}
```

Reset `wakeup_armed_` to `false` when the `EVFILT_USER` event is received.

## 7. Event Processing (run_reactor)

The reactor loop structure mirrors the epoll backend:

```cpp
void kqueue_scheduler::run_reactor(std::unique_lock<std::mutex>& lock)
{
    struct timespec* ts_ptr = /* calculate from timer service */;
    lock.unlock();

    struct kevent events[128];
    int nev = kevent(kq_, nullptr, 0, events, 128, ts_ptr);
    int saved_errno = errno;

    timer_svc_->process_expired();

    if (nev < 0 && saved_errno != EINTR)
        throw_system_error(...);

    lock.lock();

    int completions_queued = 0;
    for (int i = 0; i < nev; ++i)
    {
        // Skip wakeup event
        if (events[i].filter == EVFILT_USER)
        {
            wakeup_armed_.store(false, std::memory_order_relaxed);
            continue;
        }

        auto* desc = static_cast<descriptor_data*>(events[i].udata);

        // Error handling
        int err = 0;
        if (events[i].flags & EV_ERROR)
        {
            err = static_cast<int>(events[i].data);
            if (err == 0) err = EIO;
        }
        else if (events[i].flags & EV_EOF && events[i].fflags != 0)
        {
            err = static_cast<int>(events[i].fflags);
            if (err == 0) err = EIO;
        }

        if (events[i].filter == EVFILT_READ)
        {
            // Same logic as epoll EPOLLIN handling
            auto* op = desc->read_op.exchange(nullptr, std::memory_order_acq_rel);
            if (op) { /* perform_io, post, etc. */ }
            else { desc->read_ready.store(true, std::memory_order_release); }
        }

        if (events[i].filter == EVFILT_WRITE)
        {
            // Same logic as epoll EPOLLOUT handling
            // Check connect_op first, then write_op
        }

        // Error propagation to all pending ops (same as epoll)
        if (err) { /* cancel all pending ops with error */ }
    }

    // Wake idle workers
    if (completions_queued > 0) { /* notify_one/all */ }
}
```

### 7.1 Error Detection Differences

| Condition | epoll | kqueue |
|---|---|---|
| Socket error | `EPOLLERR` flag | `EV_EOF` with `fflags != 0` |
| Peer hangup | `EPOLLHUP` flag | `EV_EOF` flag on `EVFILT_READ` |
| Error code retrieval | `getsockopt(SO_ERROR)` | `events[i].fflags` (errno) or `getsockopt(SO_ERROR)` |
| Registration error | errno from `epoll_ctl` | `EV_ERROR` flag, error in `data` field |

**Important:** When kqueue reports `EV_ERROR`, the error code is in
`events[i].data`, not `fflags`. When it reports `EV_EOF`, the socket
error (if any) is in `events[i].fflags`. These are different from epoll's
`getsockopt(SO_ERROR)` pattern.

However, for reliability and consistency, always call `getsockopt(SO_ERROR)`
to retrieve the actual error code, same as the epoll backend does. The
kqueue error fields are useful for short-circuit detection but should not
be the sole source of error information.

## 8. Socket and Acceptor Services

### 8.1 Socket Creation

macOS and FreeBSD do not support `SOCK_NONBLOCK` or `SOCK_CLOEXEC` in the
`socket()` call (these are Linux extensions). Use `fcntl()` instead:

```cpp
int fd = ::socket(AF_INET, SOCK_STREAM, 0);
// Set non-blocking
int flags = ::fcntl(fd, F_GETFL, 0);
::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
// Set close-on-exec
::fcntl(fd, F_SETFD, FD_CLOEXEC);
```

This matches the select backend's approach.

### 8.2 Accept

macOS and FreeBSD do not support `accept4()` (Linux extension). Use
`accept()` + `fcntl()`:

```cpp
int accepted = ::accept(listen_fd, ...);
// Set non-blocking + close-on-exec via fcntl (same as select backend)
```

This matches the select backend's `perform_io()` in `select_accept_op`.

### 8.3 SIGPIPE Prevention

The epoll backend uses `MSG_NOSIGNAL` with `sendmsg()`. macOS does not
support `MSG_NOSIGNAL`. Options:

1. **`SO_NOSIGPIPE` socket option (macOS/BSD):** Set once per socket at
   creation time. This is the simplest approach.
2. **`signal(SIGPIPE, SIG_IGN)`:** Process-wide, affects all sockets.
   Not ideal for a library.

**Recommendation:** Set `SO_NOSIGPIPE` on each socket during
`open_socket()` and on accepted sockets:

```cpp
int one = 1;
::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
```

Then use `sendmsg()` without `MSG_NOSIGNAL` (or use `writev()` directly,
since SIGPIPE is suppressed at the socket level).

**Portability note:** `SO_NOSIGPIPE` is available on macOS, FreeBSD,
OpenBSD, NetBSD, and DragonFlyBSD. It is **not** available on Linux
(where `MSG_NOSIGNAL` is used instead). Since the kqueue backend only
compiles on BSD/macOS, this is fine.

### 8.4 Socket Options (Identical)

All `setsockopt`/`getsockopt` calls for TCP_NODELAY, SO_KEEPALIVE,
SO_RCVBUF, SO_SNDBUF, SO_LINGER are identical across platforms. Copy
from the epoll backend verbatim.

## 9. Lifetime Management

Identical to the epoll backend:

- `kqueue_socket_impl` inherits `enable_shared_from_this`
- Service owns impls via `shared_ptr` in an `unordered_map`
- Pending ops hold `impl_ptr` (shared_ptr) to prevent use-after-free
- Cancel captures `shared_from_this()` before posting op

No changes needed from the epoll pattern.

## 10. macOS vs FreeBSD Differences

### 10.1 Differences That Affect This Implementation

| Area | macOS | FreeBSD | Impact |
|---|---|---|---|
| `EVFILT_USER` reliability | Works | Reports of issues with concurrent `kevent()` | May need self-pipe fallback on FreeBSD |
| `accept4()` | Not available | Available since FreeBSD 10 | Use `accept()` + `fcntl()` everywhere for simplicity |
| `SOCK_NONBLOCK`/`SOCK_CLOEXEC` | Not available | Available since FreeBSD 10 | Use `fcntl()` everywhere for simplicity |
| `MSG_NOSIGNAL` | Not available | Available | Use `SO_NOSIGPIPE` on all BSD |
| `EV_ADD` in returned flags | Set | Not set (FreeBSD < 9) | Test flags with `&`, never `==` |
| `EVFILT_TIMER` precision | `NOTE_SECONDS`, `NOTE_USECONDS`, `NOTE_NSECONDS`, plus macOS-only `NOTE_CRITICAL`, `NOTE_BACKGROUND`, `NOTE_LEEWAY` | `NOTE_SECONDS`, `NOTE_USECONDS`, `NOTE_NSECONDS` | Use common subset only |
| `NOTE_ABSTIME` naming | `NOTE_ABSOLUTE` | `NOTE_ABSTIME` | Use relative timers only (common subset) |
| `kevent64()` / `kevent_qos()` | Available | Not available | Don't use; standard `kevent()` sufficient |
| `kqueue()` close-on-exec | Not atomic | `kqueue1(O_CLOEXEC)` on FreeBSD 14+ | Use `kqueue()` + `fcntl()` for portability |

### 10.2 Recommended `#ifdef` Guards

```cpp
// Only if EVFILT_USER proves unreliable on FreeBSD:
#if defined(__FreeBSD__)
    // Use self-pipe for reactor wakeup
#else
    // Use EVFILT_USER
#endif
```

The above should only be added if testing reveals a real issue. Start
without any platform-specific guards.

### 10.3 Differences That Do NOT Affect This Implementation

- `kevent64()` / `kevent_qos()` (macOS-only) -- not needed
- `EVFILT_MACHPORT` (macOS-only) -- not relevant
- `EV_UDATA_SPECIFIC` (macOS-only) -- not needed
- `EVFILT_AIO` (FreeBSD-only) -- not relevant (we use non-blocking I/O)
- Power management flags (`NOTE_CRITICAL`, `NOTE_BACKGROUND`) -- not needed
- `kqueue1()` (FreeBSD 14+) -- portability concern, use `kqueue()` + `fcntl()`

## 11. Comparison with Existing Backends

### 11.1 kqueue vs epoll (Closest Analog)

| Aspect | epoll | kqueue | Notes |
|---|---|---|---|
| Thread model | Single reactor + condvar | Same | Identical |
| Edge-triggered | `EPOLLET` | `EV_CLEAR` | Same semantics |
| Reactor wakeup | `eventfd` (edge-triggered) | `EVFILT_USER` + `NOTE_TRIGGER` | Both zero-copy |
| Registration | `epoll_ctl(ADD/MOD/DEL)` | `kevent()` changelist | kqueue can batch |
| Event granularity | Combined mask per fd | Separate per filter | kqueue needs 2 entries per fd |
| Timeout | milliseconds (`int`) | `struct timespec` | kqueue is more precise |
| Error reporting | `EPOLLERR`/`EPOLLHUP` + `getsockopt` | `EV_EOF` + `fflags`/`getsockopt` | Different detection, same result |
| Socket creation | `SOCK_NONBLOCK \| SOCK_CLOEXEC` | `fcntl()` | Linux extensions not available |
| Accept | `accept4()` | `accept()` + `fcntl()` | Linux extension not available |
| SIGPIPE | `MSG_NOSIGNAL` | `SO_NOSIGPIPE` | Different mechanism, same effect |
| Descriptor state | `descriptor_data` with atomics | Same | Identical |
| Operation model | Try-first, then register | Same | Identical |
| Cancellation | Atomic exchange on `descriptor_data` | Same | Identical |

**Bottom line:** The kqueue backend is ~95% identical to epoll. The
differences are limited to:
- Syscall names/signatures
- Socket and accept creation (fcntl instead of flags)
- SIGPIPE prevention mechanism
- Error field locations in events
- Timeout format

### 11.2 kqueue vs select (Reference for POSIX Patterns)

| Aspect | select | kqueue |
|---|---|---|
| Scalability | O(n) per iteration, FD_SETSIZE limit | O(1), no fd limit |
| Registration | `unordered_map<int, fd_state>` + rebuild `fd_set` | Persistent kernel state |
| Reactor wakeup | Self-pipe | `EVFILT_USER` (or self-pipe fallback) |
| Cancellation | Tri-state `registered` atomic | Atomic exchange on `descriptor_data` |
| Socket creation | `socket()` + `fcntl()` | Same |
| Accept | `accept()` + `fcntl()` | Same |
| SIGPIPE | `MSG_NOSIGNAL` | `SO_NOSIGPIPE` |

The select backend's POSIX compatibility patterns (fcntl, accept, etc.)
are directly reusable for kqueue. The cancellation model differs: select
uses a tri-state `registered` atomic because it needs explicit
register/deregister with the scheduler's `registered_fds_` map. kqueue,
like epoll, uses persistent kernel registration with atomic op pointers
in `descriptor_data`, which is simpler.

### 11.3 kqueue vs IOCP (Architectural Differences)

| Aspect | IOCP | kqueue |
|---|---|---|
| Model | Proactor (true async) | Reactor (readiness notification) |
| Completion | OS completes I/O, delivers result | OS signals readiness, app does I/O |
| Thread model | `GetQueuedCompletionStatus` | Single reactor + condvar |
| Socket impl | Two-layer (`wrapper` + `internal`) | Single layer with `shared_from_this` |
| Operation | `OVERLAPPED` + `overlapped_op` | `kqueue_op` (inherits `scheduler_op`) |
| Cancellation | `CancelIoEx()` | Atomic exchange on `descriptor_data` |

The IOCP backend is fundamentally different (proactor vs reactor). No
code sharing is possible beyond the abstract interfaces.

## 12. Traps and Pitfalls

### 12.1 `EV_CLEAR` Semantics (CRITICAL)

`EV_CLEAR` (kqueue's edge-triggered mode) has the same race condition as
`EPOLLET`: an event can arrive between the time an operation gets EAGAIN
and the time the op pointer is stored in `descriptor_data`. The existing
`read_ready`/`write_ready` atomic flags handle this correctly in the epoll
backend. The same pattern must be used for kqueue.

The sequence is:
1. Operation gets EAGAIN
2. Store op in `descriptor_data.read_op` (atomic)
3. Check `read_ready` flag (atomic exchange to false)
4. If was true: claim op, perform I/O

This is the most subtle part of the implementation. It must be copied
exactly from the epoll backend. Any deviation risks lost wakeups.

### 12.2 `EV_EOF` vs `EV_ERROR`

kqueue returns errors in two different ways depending on the situation:

- **`EV_ERROR` in `flags`:** The changelist registration itself failed.
  Error code is in `events[i].data`. This is analogous to `epoll_ctl()`
  returning -1.

- **`EV_EOF` in `flags`:** The peer disconnected or the fd hit an error
  condition. Error code (if any) is in `events[i].fflags`.

Do not confuse these. `EV_ERROR` should be treated as a programming error
(registration failure) and potentially thrown. `EV_EOF` is a normal I/O
completion condition.

### 12.3 Two Events Per fd

kqueue delivers `EVFILT_READ` and `EVFILT_WRITE` as separate events.
Unlike epoll (where a single event can have both `EPOLLIN | EPOLLOUT`),
a single `kevent()` call can return two events for the same fd. The event
processing loop must handle both independently. This is actually simpler
than epoll's combined mask.

### 12.4 `kevent()` Changelist Errors

When passing a changelist to `kevent()`, errors in individual changelist
entries are returned as `EV_ERROR` events in the eventlist (not as a
return value). If using the changelist for registration during the wait
call, check for `EV_ERROR` in the returned events.

For the initial implementation, use separate `kevent()` calls for
registration (changelist-only, no wait) and waiting (eventlist-only, no
changelist). This avoids this complexity.

### 12.5 Test Flags with Bitwise AND

FreeBSD < 9 does not set `EV_ADD` in returned event flags, but macOS
does. Always test with `events[i].flags & EV_EOF` instead of
`events[i].flags == EV_EOF`.

### 12.6 `kqueue()` is Not `CLOEXEC` by Default

Unlike `epoll_create1(EPOLL_CLOEXEC)`, `kqueue()` does not support a
close-on-exec flag. Call `fcntl(kq_, F_SETFD, FD_CLOEXEC)` after
`kqueue()`:

```cpp
kq_ = ::kqueue();
if (kq_ < 0) throw_system_error(...);
if (::fcntl(kq_, F_SETFD, FD_CLOEXEC) < 0)
{
    ::close(kq_);
    throw_system_error(...);
}
```

### 12.7 `EV_EOF` on Read Means Peer Shutdown, Not Necessarily Error

When `EVFILT_READ` returns with `EV_EOF` and `fflags == 0`, this means
the peer performed a graceful shutdown (sent FIN). The read will return
0 bytes (EOF), which the op's `operator()` already handles correctly
(sets `capy::error::eof`). Do not treat `EV_EOF` alone as an error.

### 12.8 Write After `EV_EOF`

When `EVFILT_WRITE` returns with `EV_EOF`, it means the read side of the
connection has been closed by the peer. However, writes may still succeed
(half-open connection). Only treat `EV_EOF` on `EVFILT_WRITE` as an error
if `fflags != 0`.

### 12.9 macOS kqueue Bug Detection

libevent tests for a known macOS kqueue bug where `EV_ERROR` is not
properly returned for invalid fds during registration. This library
should not need to handle this case because it only registers valid,
open fds. However, if defensive coding is desired, validate the return
of `kevent()` changelist operations.

### 12.10 Memory Ordering on `udata`

The `udata` field in `struct kevent` carries the `descriptor_data*`
pointer. This pointer is set during `register_descriptor()` and read
during event processing in the reactor. Both the epoll backend and this
design use `std::atomic_thread_fence(std::memory_order_seq_cst)` in
`update_descriptor_events()` to ensure op pointers stored in
`descriptor_data` are visible to the reactor thread. The same fence
is needed for kqueue.

## 13. Implementation Order

The recommended implementation sequence:

1. **`kqueue_context.hpp` + `kqueue_context.cpp`**: Minimal context class
2. **`op.hpp`**: Operation state types (adapt from epoll)
3. **`scheduler.hpp` + `scheduler.cpp`**: Scheduler with kqueue event loop
4. **`sockets.hpp` + `sockets.cpp`**: Socket service and impl
5. **`acceptors.hpp` + `acceptors.cpp`**: Acceptor service and impl
6. **`io_context.hpp`**: Uncomment kqueue alias
7. **Testing**: Run existing test suite on macOS/FreeBSD

Steps 1-5 can largely be done by copying the epoll backend and making
the substitutions described in this document. Step 6 is a one-line change.

## 14. Testing Strategy

The existing test suite should work unmodified since all tests use the
abstract `io_context` type alias. On kqueue platforms, `io_context` will
resolve to `kqueue_context`, and all tests should pass.

Additional kqueue-specific testing:

- Verify `EVFILT_USER` wakeup works on both macOS and FreeBSD
- Stress test with concurrent connections to exercise edge-triggered races
- Test `EV_EOF` handling (graceful shutdown, half-open connections)
- Test cancellation under load (stop_token + concurrent I/O)
- Test FD_CLOEXEC on kqueue fd and accepted sockets
- Test `SO_NOSIGPIPE` behavior (write to closed peer)
