# IOCP socket/acceptor service-plumbing dedup — Windows implementation spec

**Audience:** an agent working on a Windows machine with MSVC, able to build and run the
IOCP backend. This task is Windows-only and cannot be compiled or tested on Linux, which
is why it's split out as a spec rather than done inline with the io_uring work.

**Status:** proposed (#4 in `tasks/proactor-dedup-decisions.md`). Not yet started.

---

## Background

The corosio "proactor dedup" effort deduplicated the completion-based backends (IOCP +
io_uring). Read `tasks/proactor-dedup-decisions.md` first — especially entries #11–#14 and
the "Implementation progress" section. The short version of what already landed:

- **Op envelope** (`proactor_op`) + completion-tail helpers
  (`proactor_drain_if_shutdown` / `proactor_resume`) are shared by IOCP and io_uring
  (`native/detail/proactor/`). IOCP's `overlapped_op` derives from `proactor_op`.
- **Scheduler** and **socket accessors** were deduplicated along the *io_uring↔reactor*
  axis (io_uring is a hybrid that aligns with the reactor family). **IOCP deliberately
  stays separate** there — it's the parallel-GQCS outlier, exactly as Boost.Asio keeps
  `win_iocp_io_context` standalone.
- **io_uring service plumbing** was deduplicated *within io_uring* via two small bases:
  - `io_uring/io_uring_socket_service_base.hpp` — `io_uring_socket_service_base<Derived,
    ServiceBase, Socket>`: shared construct/destroy/shutdown/close/scheduler() + the
    impl map, for the 4 socket services.
  - `io_uring/io_uring_file_service_base.hpp` — `io_uring_file_service_base<Derived,
    ServiceBase, File>`: same idea for the 2 file services.
  **These are the model to mirror for IOCP.** Read them — they're short (~120–140 lines).

This task does the analogous dedup for the **IOCP services**, which today hand-duplicate
the same construct/destroy/shutdown/close plumbing across `win_tcp_service`,
`win_udp_service`, `win_local_stream_service`, `win_file_service`, and
`win_random_access_file_service` (and the acceptor services).

## Scope and explicit non-goals

- **DO:** factor the duplicated service lifecycle plumbing into a shared IOCP service base
  (or two — see below), leaving each concrete service with only its protocol-specific bits.
- **DO NOT** collapse IOCP's split-object model (the `win_*_socket` wrapper +
  `win_*_socket_internal` pair). That collapse was explicitly declined as too risky for
  the value (decisions log; the Phase-3 redirect). The shared base must work *with* the
  split-object model as-is.
- **DO NOT** touch the scheduler, the wait reactor, the op types, or the WSARecv/WSASend/
  ConnectEx/AcceptEx submission logic. Only the service lifecycle plumbing.
- Keep it **behavior-neutral** (pure refactor). The io_uring service dedups were
  behavior-neutral and verified 125/125; aim for the same.

## The current IOCP service shape (confirm before editing)

Read these to confirm the structure hasn't drifted from this spec:
`win_tcp_service.hpp`, `win_udp_service.hpp`, `win_local_stream_service.hpp`,
`win_tcp_acceptor_service.hpp`, `win_local_stream_acceptor_service.hpp`,
`win_file_service.hpp`, `win_random_access_file_service.hpp` (all under
`include/boost/corosio/native/detail/iocp/`).

`win_tcp_service` (representative) has, per `win_tcp_service.hpp`:

```cpp
class win_tcp_service
    : private win_wsa_init                 // RAII WSAStartup/WSACleanup refcount
    , public capy::execution_context::service
    , public io_object::io_service
{
    // lifecycle (duplicated across services):
    io_object::implementation* construct() override;
    void destroy(io_object::implementation* p) override;
    void close(io_object::handle& h) override;
    void shutdown() override;
    void destroy_impl(win_tcp_socket& impl);
    void unregister_impl(win_tcp_socket_internal& impl);
    void destroy_acceptor_impl(win_tcp_acceptor& impl);

    // SPLIT-OBJECT tracking: four intrusive lists
    intrusive_list<win_tcp_socket_internal>  socket_list_;        // the "internal" state
    intrusive_list<win_tcp_acceptor_internal> acceptor_list_;
    intrusive_list<win_tcp_socket>           socket_wrapper_list_; // the public wrappers
    intrusive_list<win_tcp_acceptor>         acceptor_wrapper_list_;

    win_scheduler& sched_;
    void* iocp_;                            // CreateIoCompletionPort handle
    LPFN_CONNECTEX connect_ex_ = nullptr;   // MSWSock extension fns (TCP-specific)
    LPFN_ACCEPTEX  accept_ex_  = nullptr;
    void load_extension_functions();
};
```

Construction pattern (per `win_tcp_acceptor_service.hpp` ~987): `construct()` does
`make_shared<win_tcp_socket_internal>(*this)`, pushes the raw internal into
`socket_list_`, heap-`new`s a `win_tcp_socket` wrapper holding the internal shared_ptr,
pushes the wrapper into `socket_wrapper_list_`, and returns the wrapper. `destroy()` calls
`wrapper.close_internal()` then `destroy_impl()` (removes wrapper from the wrapper list +
deletes it). `shutdown()` drains `socket_list_` + `acceptor_list_`, calling
`close_socket()` on each internal. The service dtor deletes the wrapper-list entries.
The internal's dtor calls `svc_.unregister_impl(*this)` to drop out of `socket_list_`.

**What's duplicated across win_tcp/udp/local_stream services:** the whole
construct/destroy/close/shutdown/destroy_impl/unregister_impl dance + the two
socket-side intrusive lists (internal + wrapper). **What's per-service:** the impl
types, `open_socket`/`open_datagram_socket`/`bind`, the `win_wsa_init` base, the IOCP
handle + `CreateIoCompletionPort` association, and (TCP/acceptor only) the MSWSock
extension-fn loading + the acceptor lists.

## Recommended design

Mirror `io_uring_socket_service_base`, but parameterized on **both** the wrapper and the
internal type because IOCP is split-object. Suggested:

```cpp
// include/boost/corosio/native/detail/iocp/win_socket_service_base.hpp
template<class Derived, class ServiceBase, class Wrapper, class Internal>
class win_socket_service_base
    : private win_wsa_init
    , public ServiceBase          // capy::execution_context::service + io_object::io_service
{
protected:
    explicit win_socket_service_base(capy::execution_context& ctx);   // sets sched_, iocp_
public:
    io_object::implementation* construct() override;   // make internal + wrapper, 2 lists
    void destroy(io_object::implementation* p) override;
    void close(io_object::handle& h) override;
    void shutdown() override;                            // drain socket_list_, close_socket()
    win_scheduler& scheduler() const noexcept;
    void* iocp() const noexcept;
protected:
    void unregister_impl(Internal& impl);                // called from Internal dtor
    win_scheduler& sched_;
    void*          iocp_;
    win_mutex      mutex_;
    intrusive_list<Internal> socket_list_;
    intrusive_list<Wrapper>  socket_wrapper_list_;
};
```

Concrete services derive and add only `open_socket`/`bind`/extension-fn loading:

```cpp
class win_tcp_service final
    : public win_socket_service_base<win_tcp_service, /*ServiceBase*/ <existing bases>,
                                     win_tcp_socket, win_tcp_socket_internal>
{
    // open_socket / bind_socket / connect_ex_ / accept_ex_ / load_extension_functions
    // + the acceptor-side lists & destroy_acceptor_impl (TCP/local_stream only)
};
```

Requirements the base imposes on its type params (verify each impl satisfies them —
they already do today):
- `Internal`: `intrusive_list<Internal>::node`, `enable_shared_from_this<Internal>`, a
  `close_socket()` method, and an `Internal(Derived&)` ctor.
- `Wrapper`: `intrusive_list<Wrapper>::node`, a `close_internal()` method, and a
  `Wrapper(shared_ptr<Internal>)` ctor.

### Open questions to resolve while implementing (decide and record)

1. **Acceptors.** The acceptor services (`win_tcp_acceptor_service`,
   `win_local_stream_acceptor_service`) add a *second* pair of lists
   (`acceptor_list_` / `acceptor_wrapper_list_`) and `destroy_acceptor_impl`. Decide
   whether the base is parameterized to handle a second (acceptor) object pair, or whether
   acceptors keep their own list management on top of the socket base. The io_uring
   equivalent left acceptor services separate (their `construct()` diverged) — IOCP may
   reasonably do the same. Don't force it.
2. **`win_wsa_init` as a base.** It's a refcounted RAII WSAStartup. Putting it in the
   shared base means every concrete service still gets exactly one `win_wsa_init`
   subobject (good — the refcount handles multiplicity). Confirm the inheritance
   (`private win_wsa_init`) composes correctly through the template.
3. **The `ServiceBase` bases.** IOCP services inherit `capy::execution_context::service`
   *and* `io_object::io_service` (and the protocol service interface like `tcp_service`?).
   Check the exact base list of each concrete service and thread it through the template's
   `ServiceBase` parameter (it may need to be a pack or a single protocol-service type that
   already pulls in the others — match what the reactor/io_uring service bases do).
4. **Files.** `win_file_service` / `win_random_access_file_service` may follow the same
   split-object pattern or a simpler one (the random-access file uses heap ops). If they
   duplicate the same plumbing, give them a `win_file_service_base` analogous to
   `io_uring_file_service_base`; if their tracking differs, keep them separate. Mirror the
   io_uring split (separate socket vs file bases) where it fits.

## Step-by-step

1. Read `io_uring/io_uring_socket_service_base.hpp` and the committed io_uring service
   migrations (`git show 55ee2b05`, `git show a4f9dfa9`) as the model.
2. Read the IOCP service headers; confirm the structure above; list every method/member
   that is identical across `win_tcp/udp/local_stream` services.
3. Add `iocp/win_socket_service_base.hpp` with the shared plumbing.
4. Migrate `win_tcp_service`, `win_udp_service`, `win_local_stream_service` to derive from
   it, keeping only their per-protocol methods. (Acceptor + file services per the open
   questions above.)
5. Make each concrete socket/internal satisfy the base's type requirements (most already
   do; add a `close_socket()` / `close_internal()` only if missing).
6. Build (MSVC, the project's existing Windows CMake configure — see `msvc_build_log.txt`
   in the repo root for the prior invocation) and run the IOCP test suite:
   `ctest -R iocp --output-on-failure`. Pay special attention to the socket_stress
   (cancel_close, stop_token, sync_completion), tcp_socket, udp_socket, tcp_acceptor,
   local_stream variants — service teardown is where a plumbing refactor would break.
7. Confirm it's behavior-neutral: same pass/fail as a clean baseline build of the same
   commit without this change.

## Rules

- **Behavior-neutral**: do not change shutdown/close semantics; just relocate the shared
  code. If you find a semantic difference you think is worth making, STOP and flag it —
  don't fold it into the refactor.
- **Don't** collapse the split-object model, touch the scheduler/wait-reactor/op types, or
  change submission code.
- Keep the diff to the service headers + the new base header.
- Record the design decisions (acceptor handling, file handling, the ServiceBase param
  shape) as a new entry in `tasks/proactor-dedup-decisions.md` (#15), matching the format
  of #13/#14.
- Commit message style: `refactor(iocp): share socket-service plumbing via
  win_socket_service_base` with a body explaining the split-object accommodation and the
  acceptor/file decisions. End with the project's Co-Authored-By trailer.

## Why this is worth doing (and its limits)

It removes the same ~per-service construct/destroy/shutdown duplication the io_uring side
removed, keeping the two backends structurally parallel. It is **IOCP-internal** dedup
(it does not share code across backends — IOCP's split-object + GQCS model genuinely
differs from io_uring/reactor, per #11/#13). That's expected and fine; the cross-backend
sharing already happened at the op-envelope layer.
