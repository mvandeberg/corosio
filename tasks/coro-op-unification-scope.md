# Scope: unify the op envelope across reactor + io_uring + IOCP (`coro_op`)

**Status:** proposed / not started. This is a design + plan, not yet implemented. It touches
the *working* reactor op layer and the Windows-only IOCP op, so it needs sign-off + (for the
IOCP part) a Windows build before landing.

## Why

`reactor_op` (+ `reactor_op_base`) and `proactor_op` independently define **the same
coroutine-op envelope**, and `reactor_op_complete.hpp` repeats `proactor_resume`'s tail
idiom inline ~7×. So corosio currently carries **two near-identical op envelopes + two
completion-tail helper sets**. `proactor_op` deduplicated the io_uring↔IOCP half; this
finishes the job by collapsing all of it — reactor + io_uring + IOCP — onto one envelope.

Shared today (duplicated): `h`, `cont_op`, `ex`, `ec_out`, `bytes_out`, `cancelled`,
`stop_cb` + a `canceller` struct, `impl_ptr`, `start(stop_token…)`, `request_cancel()`,
and the completion tail (`stop_cb.reset()` → resume on executor → drop `impl_ptr`).

## Target layering

```
scheduler_op                                  (existing: func-ptr/virtual dispatch)
  └── coro_op : scheduler_op                  (NEW — the shared envelope; replaces proactor_op)
        fields:  h, cont_op, ex, ec_out, bytes_out, is_read, empty_buffer,
                 cancelled, stop_cb, impl_ptr
        struct canceller { coro_op* op; operator()() -> op->on_cancel(); }
        void start(stop_token const&);        // arm stop_cb
        void request_cancel() noexcept;        // set cancelled (release)
        virtual void on_cancel() noexcept { request_cancel(); }   // backend hook
        │
        ├── reactor_op_base : coro_op          (reactor-only result model)
        │       int errn; std::size_t bytes_transferred;
        │       virtual void perform_io();      // re-run syscall on readiness
        │       void complete(int err, std::size_t bytes);
        │     └── reactor_op : reactor_op_base
        │           int fd; Socket* socket_impl_; Acceptor* acceptor_impl_;
        │           on_cancel() override -> route via socket_impl_/acceptor_impl_
        │           (was the pure-virtual cancel())
        │
        ├── io_uring_op : coro_op
        │       int res; unsigned cqe_flags; atomic<bool> sqe_set;
        │       cqe_func; prep_func; io_uring_scheduler* sched_;
        │       on_cancel() override -> submit ASYNC_CANCEL SQE (existing body)
        │
        └── overlapped_op : OVERLAPPED, coro_op            (Windows)
                long ready_; DWORD dwError, bytes_transferred; cancel_func_;
                on_cancel() override -> request_cancel() + cancel_func_ (existing body)
```

One shared `coro_op_complete.hpp`:
- `bool coro_drain_if_shutdown(void* owner, coro_op*)` — stop_cb reset + shutdown-drain
  suicide (= today's `proactor_drain_if_shutdown`).
- `void coro_resume(coro_op*)` — resume-on-executor + drop impl_ptr (= today's
  `proactor_resume`, and the idiom inlined ~7× in `reactor_op_complete.hpp`).

The **result decode stays per-backend** (it's the genuine reactor-vs-proactor difference):
reactor `errn`→ec, io_uring `res`→ec, IOCP `dwError`→ec. Only the envelope + the tail are
shared; the "what does this completion mean" logic is not.

## Per-backend deltas

- **io_uring_op / overlapped_op:** trivially re-base from `proactor_op` to `coro_op` (a
  rename — they already derive from the envelope). Their `on_cancel()` overrides are
  unchanged.
- **reactor_op_base / reactor_op:** the real work.
  - Move `h`/`cont_op`/`ex`/`ec_out`/`bytes_out`/`stop_cb`/`canceller`/`start`/
    `request_cancel`/`cancelled`/`impl_ptr` up into `coro_op`; `reactor_op_base` keeps
    `errn`/`bytes_transferred`/`perform_io`/`complete`; `reactor_op` keeps `fd` +
    `socket_impl_`/`acceptor_impl_`.
  - **Cancellation hook:** today `reactor_op` has a pure-virtual `cancel()` (each concrete
    op overrides, routing via the stored impl ptr). Rename/retarget those overrides to
    `on_cancel()` so the unified `coro_op::canceller` drives them. Mechanical across the
    reactor concrete ops.
  - **`start()` signature:** reactor's `start(token, Socket*/Acceptor*)` stores the impl
    ptr *and* arms `stop_cb`. Split: `coro_op::start(token)` arms `stop_cb`; `reactor_op`
    sets `socket_impl_`/`acceptor_impl_` then calls the base (keep a thin
    `start(token, impl)` on `reactor_op`).
  - **`is_read`:** reactor uses a virtual `is_read_operation()`; the envelope uses a `bool
    is_read` field. Switch reactor concrete ops to set the field, and update
    `reactor_op_complete.hpp`'s eof decode (`op.is_read_operation()` →
    `op.is_read`). Mechanical.
- **Completion tail:** point `reactor_op_complete.hpp` and the io_uring handlers at the
  shared `coro_resume`/`coro_drain_if_shutdown`; delete `proactor_op_complete.hpp` and the
  ~7 inline tail repeats.

## Location / naming

- New: `native/detail/coro_op.hpp` + `native/detail/coro_op_complete.hpp` (top-level under
  `native/detail/`, since it's genuinely cross-backend — not "reactor", not "proactor").
- Remove `native/detail/proactor/proactor_op_base.hpp` + `proactor_op_complete.hpp` (folded
  into `coro_op`). The `proactor/` directory then has nothing left → delete it. (This also
  resolves the earlier finding that `proactor/` overstated cross-backend sharing.)

## Phased plan (each phase: tree builds + tests green before the next)

1. **A — rename/move (low risk, local + Windows).** `proactor_op` → `coro_op`,
   `proactor_op_complete` → `coro_op_complete`, moved to `native/detail/`. Update the io_uring
   + IOCP includes and the 3 io_uring handler files. io_uring/reactor verified on Linux;
   hand the IOCP include/rename to Windows. Pure rename — no behavior change.
2. **B — reactor onto `coro_op` (the risky part, local-testable).** Re-base
   `reactor_op_base`/`reactor_op`; reconcile the cancellation hook (`cancel()`→`on_cancel()`),
   `start()`, and `is_read`. Verify epoll/kqueue/select **and** io_uring (125/125). This is
   the regression-prone step — the reactor cancellation path is subtle.
3. **C — unify the completion tail (local + Windows).** Route `reactor_op_complete.hpp` and
   the io_uring handlers through `coro_resume`/`coro_drain_if_shutdown`; delete the
   duplicate helper file + inline repeats. Verify all backends; hand IOCP to Windows.

IOCP touch is limited to the `overlapped_op` re-parent (Phase A) + completion-tail include
(Phase C) — both mechanical, Windows-verified, same as the Phase 1 IOCP work.

## Cost / benefit (honest)

- **Removed:** one full duplicate envelope (~35–50 lines of fields + start/cancel wiring) and
  the duplicate completion-tail helpers (~7 inline tail repeats in `reactor_op_complete`
  collapse to one helper; `proactor_op_complete.hpp` deleted). Plus the conceptual win: **one
  op envelope for the whole library**, and the `proactor/` directory disappears (no more
  "looks shared but isn't").
- **Cost / risk:** Phase B edits the *working* reactor op layer — the cancellation-hook and
  `is_read` reconciliations are mechanical but the reactor cancel path is subtle (the kind of
  thing that caused the Phase 2 spin). Mitigated by the reactor test suite (cancel,
  socket_stress: cancel_close/stop_token) on every step. IOCP re-parent is Windows-verified.
- **Verdict:** worth doing *if* we want the op layer to be honestly unified. It's the
  op-layer analogue of "io_uring joins the reactors." If we don't do it, the cleaner
  fallback is to stop treating `proactor_op` as a grand shared layer (rename it, move the
  io_uring-only `proactor_op_complete` into `io_uring/`) and accept two envelopes — but that
  leaves the duplication this scope removes.

## Verification

- Per phase: `cmake --build build_cmake --target tests`; `ctest -R "epoll|select|io_uring"`
  (full 125 suite at the end of B and C). The cancel/stop_token/cancel_close stress tests are
  the regression gate for Phase B.
- A benchmark pass (io_context + socket/local throughput+latency, vs the pre-change commit)
  to confirm the envelope move is perf-neutral — the op envelope is on the hot path, unlike
  the service plumbing, so this one genuinely needs measuring (watch for any change from the
  `is_read` field-vs-virtual switch and the unified `start()`).
- Record the outcome as entry #15 in `proactor-dedup-decisions.md`.
