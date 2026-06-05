# Proactor Backend Deduplication — Decisions Log

This file records every architectural divergence between the IOCP and io_uring
proactor backends encountered while extracting the shared `proactor_*` skeleton,
the choice made for the shared backend, the rationale, and an A/B-test knob so the
choice can be revisited in a later optimization pass.

Status legend: **[decided]** chosen and recorded; **[implemented]** landed in the
shared backend; **[measured]** A/B numbers captured.

Each entry: *Current IOCP* / *Current io_uring* / *Chosen for shared backend* /
*Why* / *A/B-test knob*.

---

## 1. Op-submission model — [decided]

- **Current IOCP:** The socket method calls the Win32 API inline and synchronously
  (`WSARecv`/`WSASend`/`ConnectEx`/`AcceptEx`/`ReadFile`), one syscall per op, then
  branches: `WSA_IO_PENDING` → `on_pending(op)`; sync success/error →
  `on_completion(op, err, bytes)` (`win_scheduler.hpp:345-377`). Cannot batch.
- **Current io_uring:** Two-phase, data-driven. Each `io_uring_op` carries a
  `prep_func` pointer. `io_uring_submit_op` (`io_uring_socket_ops.hpp:446-498`) locks
  the ring, `io_uring_get_sqe` (flush+retry on full; synchronous EAGAIN if still
  full), `prep_func(op, sqe)`, `io_uring_sqe_set_data`, `inflight_inc()`, then a CAS
  (`submit_op_posted_exchange`) elects one submitter to post a single `submit_sqes_op`
  that batches the real `io_uring_submit_and_get_events`. Structurally *requires*
  `io_uring_get_sqe`.
- **Chosen for shared backend:** A single `submit(op)` seam on the socket templates
  (`Traits::submit_policy::submit_read/write/connect/...`) returning `{sync|pending}`
  with results pre-stored on the op for the sync case. Bodies fully divergent. Do
  **NOT** force IOCP through a fake-SQE `prep_func` indirection. Share only the op
  setup/teardown envelope around `submit`.
- **Why:** The two models are non-isomorphic; io_uring batches into one
  `io_uring_enter`, IOCP cannot batch at all. A fake SQE for IOCP buys nothing (still
  one WSARecv/op) and adds an indirect call on the hottest path. The real shareable
  surface is the op envelope + completion tail, not the submit verb.
- **A/B-test knob:** Keep io_uring's batching behind a runtime `batch_submit_` flag
  (not `#ifdef`) so a single binary can flip eager per-op `io_uring_submit` vs the
  batched `submit_sqes_op` path and measure batching's value in isolation.

## 2. Synchronous-completion race protocol (`ready_` CAS) — [decided]

- **Current IOCP:** A `long ready_` + `InterlockedCompareExchange` between the
  initiating thread (`on_pending`) and the GQCS thread (`do_one`, lines 579-585),
  because GQCS on another thread can dequeue the completion before the initiator
  returns from WSARecv. `key_result_stored` re-posts stored results.
- **Current io_uring:** No equivalent — CQEs exist only after submission and CQ drain
  is serialized under `ring_mutex_`, so no initiator/harvester race on the same op.
  The `sqe_set` atomic is unrelated (cancellation-visibility ordering only).
- **Chosen for shared backend:** Keep `ready_`/CAS and `on_pending`/`on_completion`
  entirely private to the IOCP scheduler + op. The shared base contract is only "the
  backend delivers exactly one completion to a run thread"; how single delivery is
  guaranteed is backend-private.
- **Why:** Irreducibly IOCP-specific — the race is a property of the Win32 overlapped
  model. Imposing a CAS on io_uring adds an atomic RMW per op for a race that cannot
  occur.
- **A/B-test knob:** None (not a tunable). Recorded as a **deliberate non-share** so a
  future reader does not "helpfully" hoist it into the base.

## 3. Speculative synchronous I/O — [decided]

- **Current IOCP:** None (no cheap non-blocking peek on overlapped sockets).
- **Current io_uring:** `read_some`/`write_some` first try non-blocking `::readv`/
  `::sendmsg` directly (`io_uring_types.hpp:160-179`), completing inline on success,
  only submitting to the ring on EAGAIN. Adaptive heuristic in `speculative_state.hpp`
  (failure-streak counter latches off after 4 consecutive EAGAINs, re-armed by a
  readiness CQE). Mirrors the reactor speculate-then-register pattern.
- **Chosen for shared backend:** Opt-in trait `static constexpr bool
  supports_speculation` (io_uring `true`, IOCP `false`). Keep `speculative_state` and
  the speculate-then-submit body in the io_uring socket layer, guarded by the trait.
  Do not drop for symmetry.
- **Why:** Largest measured win on io_uring loopback throughput (one syscall + inline
  resume vs a full ring round-trip). IOCP genuinely cannot offer it; the trait
  compiles it out on Windows at zero cost.
- **A/B-test knob:** `supports_speculation` forced false at compile time to isolate
  speculation's contribution; plus the `speculative_state` heuristic constants
  (`max_read_failures=4`, budget init/max) kept named for independent sweeps.

## 4. Readiness / wait operations — [decided]

- **Current IOCP:** Dedicated `win_wait_reactor` WSAPoll background thread
  (`win_wait_reactor.hpp`, ~414 lines) for readiness-only waits, datagram/acceptor
  readiness, and error notification; posts a synthetic completion back through
  `on_completion` so the public path stays uniform. Lazily constructed.
- **Current io_uring:** Native `IORING_OP_POLL_ADD` via `uring_wait_op`
  (`io_uring_socket_ops.hpp:511-589`), one-shot poll SQE on the standard submit path.
- **Chosen for shared backend:** Shared `wait(op, wait_type)` interface. io_uring preps
  a poll SQE; IOCP routes via `Traits::needs_wait_reactor=true` to the wait reactor
  (or a zero-byte WSARecv for `wait_type::read`). `win_wait_reactor` is not shared.
- **Why:** The wait reactor exists solely to synthesize a readiness primitive IOCP
  lacks; io_uring has it natively. Only the *interface* (completion routed through the
  same coroutine resume) can be shared, not the implementation.
- **A/B-test knob:** IOCP `wait_type::read` via zero-byte-WSARecv vs wait-reactor.
  io_uring datagram-readiness via `POLL_ADD` vs speculative `recvmsg(MSG_PEEK)`.

## 5. Accept model — [decided]

- **Current IOCP:** `AcceptEx`, one op per accept; fits one-completion-per-op cleanly.
- **Current io_uring:** `IORING_OP_MULTISHOT_ACCEPT` — one SQE yields many CQEs
  (`IORING_CQE_F_MORE`) — driven by a CRTP `io_uring_multishot_acceptor_base`
  (`io_uring_multishot_acceptor.hpp`) that parks accepted fds and queues waiters,
  synthesizing a `uring_accept_op` per connection and re-arming on terminal CQE.
  Inflight decremented only on the non-`F_MORE` CQE.
- **Chosen for shared backend:** Share only the `accept()` public envelope + accepted-
  socket construction (`complete_accept_op<Traits,Socket>`). The multishot state
  machine stays io_uring-only; AcceptEx stays IOCP-only. `accept_policy` abstracts the
  *entry*; both backends ultimately `post` a per-connection accept op resolved via the
  same `adopt` path.
- **Why:** Multishot's one-SQE-many-CQE model violates the shared op base's one-
  completion-per-op invariant; the persistent multi-accept op is an internal generator,
  not a user op. Unifying would either cripple io_uring to one-shot (losing the
  amortization that is the whole point) or burden IOCP with a parked-fd machine it
  never needs. **This is where reuse is weakest — acceptor is only partially shared.**
- **A/B-test knob:** io_uring multishot vs one-shot accept behind a flag (a one-shot
  `uring_accept_op` + re-arm path already exists) — measure multishot benefit vs its
  parked-fd/waiter bookkeeping under low-concurrency accept loads.

## 6. Cross-thread wakeup — [decided]

- **Current IOCP:** `PostQueuedCompletionStatus` (no fd; kernel object); used by
  `post`, `stop`, timer wakeup. Coalesced via `stop_event_posted_`.
- **Current io_uring:** eventfd registered via multishot `IORING_OP_POLL_ADD` at
  `lazy_init_ring`; `interrupt_reactor()` does `write(eventfd, 1)`; wakeup CQE has
  `user_data == nullptr`.
- **Chosen for shared backend:** Single `interrupt_proactor()` / `wake()` hook. IOCP =
  PQCS(`key_wake_dispatch`); io_uring = eventfd write.
- **Why:** Both reduce to "make the blocking wait return on another thread"; the
  reactor base already proves the abstraction (`interrupt_reactor`).
- **Subtleties to preserve (recorded):** io_uring must re-arm the eventfd poll on a
  non-`F_MORE` wakeup CQE (`io_uring_scheduler.hpp:1094-1108`); the wakeup CQE is
  excluded from inflight accounting; `interrupt_reactor` is a no-op in single-threaded
  mode and uses unconditional (non-coalesced) writes in MT mode to avoid a lost-wakeup
  deadlock. IOCP coalesces via `stop_event_posted_`. Keep coalescing policy backend-
  private behind the hook.
- **A/B-test knob:** n/a (correctness primitive); the idle-wait caps are tunable — see #8.

## 7. Locking — [decided]

- **Current IOCP:** Only a `dispatch_mutex_` (`win_mutex`, CRITICAL_SECTION) for the
  `completed_ops_` fallback queue; the completion port is kernel-synchronized.
- **Current io_uring:** Two locks — `dispatch_mutex_` (completed_ops_/cond_/task_running_)
  plus `ring_mutex_` (serializes all userspace SQ/CQ access; liburing head/tail
  bookkeeping is not thread-safe). Lock order `ring_mutex_ → dispatch_mutex_`.
- **Chosen for shared backend:** `dispatch_mutex_` in the shared base; `ring_mutex_`
  only in `io_uring_scheduler`. Unify the base dispatch mutex on
  `conditionally_enabled_mutex` (single-threaded disables locking).
- **Why:** `ring_mutex_` protects a resource that does not exist on IOCP; a base member
  would be dead on Windows. `dispatch_mutex_` is genuinely common.
- **Friction to resolve (recorded):** IOCP currently uses `win_mutex` (CRITICAL_SECTION,
  recursive) vs `conditionally_enabled_mutex` (std::mutex, non-recursive). Base member
  unifies on `conditionally_enabled_mutex`; confirm IOCP does not need recursion for
  `completed_ops_` (it does not appear to).
- **A/B-test knob:** io_uring-local: could a lock-free SPSC SQ work under
  SINGLE_ISSUER + single-threaded? Single-threaded mode is the existing zero-lock
  baseline.

## 8. Timer integration — [decided]

- **Current IOCP:** `win_timers` (NT waitable timer or timer thread) posts
  `key_wake_dispatch` via PQCS to wake GQCS; `update_timeout` pushes the nearest
  expiry; GQCS timeout clamped to `gqcs_timeout_ms_`. Feeds a shared `timer_service`.
- **Current io_uring:** Deadline from `timer_svc_->nearest_expiry()` converted to a
  `__kernel_timespec` for `io_uring_wait_cqe_timeout`; `on_earliest_changed` calls
  `interrupt_reactor`. 1s idle cap. Feeds the shared `timer_service`.
- **Chosen for shared backend:** Shared `do_one` computes a neutral `long timeout_us`
  from `timer_service` (mirroring `reactor_scheduler` passing `timeout_us` to
  `run_task`). Each backend's hook converts: IOCP → ms clamp vs `gqcs_timeout_ms_`;
  io_uring → `__kernel_timespec`. `win_timers` stays IOCP-private (needed only because
  GQCS's own timeout is the wait mechanism; io_uring takes the deadline directly).
- **Why:** Deadline computation from `timer_service` is identical and belongs in the
  base; only unit conversion/cap differs.
- **A/B-test knob:** The idle-wait caps (io_uring 1s, IOCP `gqcs_timeout_ms_` 500ms) —
  both lost-wakeup safety nets, lowerable once wakeup correctness is trusted.

## 9. liburing dependency — [decided] KEEP

- **Current IOCP:** Hand-written boilerplate (WSAStartup RAII, CRITICAL_SECTION wrapper,
  GQCS loop), no external library — the Win32 IOCP API needs no wrapper lib.
- **Current io_uring:** liburing (PUBLIC link). Surface (~24 functions) to reimplement
  if ever dropped: `io_uring_queue_init_params`, `io_uring_queue_exit`,
  `io_uring_get_sqe`, `io_uring_submit`, `io_uring_submit_and_get_events`,
  `io_uring_wait_cqe_timeout`, `io_uring_cqe_get_data`, `io_uring_for_each_cqe`,
  `io_uring_cq_advance`, `io_uring_sqe_set_data`, and the prep helpers
  `io_uring_prep_recv/readv/send/sendmsg/connect/writev/recvmsg/poll_add/
  poll_multishot/multishot_accept/cancel/cancel_fd`; plus the SQ/CQ mmap setup and
  SQE/CQE struct layout liburing owns.
- **Chosen for shared backend:** KEEP liburing for this refactor.
- **Why:** It sits entirely below the shared layer (invisible to the dedup; none of the
  shared seams touch the ring ABI). Its job is the riskiest part to reimplement (mmap
  setup, memory-barrier-correct head/tail updates, ABI tracking). Reimplementing now
  adds risk/code to a refactor whose point is to remove code.
- **A/B-test knob:** The function surface above is the precise contract for a future,
  separate "drop liburing" pass with its own benchmarks and ABI-compat testing. Keep
  all liburing calls funneled through the io_uring scheduler/op-prep layer (they
  already are) so the swap stays mechanically bounded.

## 10. Inline budget / scheduler context stack — [decided]

- **Current IOCP:** `thread_context_guard` + `scheduler_context` stack — tracks the
  running-scheduler pointer only (for `running_in_this_thread`). No inline budget (no
  speculation).
- **Current io_uring:** Thread-local `io_uring_scheduler_frame` stack with
  `inline_budget`/`inline_budget_max` (fixed init=2, max=16, **no adaptive ramp** —
  deliberately deferred). Budget consumed by the speculative path.
- **Current reactor (reuse target):** `reactor_scheduler_context` — richest: running
  pointer, private work queue + `private_outstanding_work`, inline budget,
  `inline_budget_max`, and an `unassisted` flag driving an **adaptive** budget ramp.
- **Chosen for shared backend:** The shared `proactor_scheduler` base owns the running-
  context stack + inline-budget machinery, modeled on `reactor_scheduler_context`
  (including adaptive ramp + configurable init/max/unassisted). Budget consumed only
  where speculation exists (io_uring); IOCP keeps the stack purely for
  `running_in_this_thread`, budget never consumed (gated by `supports_speculation`).
- **Why:** The running-pointer stack is byte-for-byte the same idea in all three
  backends. Adopting the reactor's complete adaptive implementation both unifies and
  upgrades io_uring (gaining the ramp it deferred).
- **Differences to flag / measure:**
  - io_uring moves fixed-budget → adaptive: **behavior change, must benchmark.**
  - The reactor's private-queue contention optimization is **NOT** adopted in the
    initial dedup (it interacts with the leader/follower `task_running_` model
    differently than the reactor's task-sentinel model) — recorded as a candidate
    future optimization.
  - IOCP gains a budget field it never consumes — harmless, gated by the trait.
- **A/B-test knob:** Budget init/max/unassisted runtime-configurable (as
  `reactor_scheduler::configure_reactor` does); "adaptive vs fixed budget" behind a
  flag so io_uring's fixed→adaptive move is measurable in isolation.

---

## Net assessment

The proactors can share the *outer scheduler shell* and the *op envelope + completion
tail* (a large fraction of line count), exposing ~5 hooks (`submit`,
`harvest_completions`, `wake`/`interrupt_proactor`, `wait`, `has_kernel_work`). They
**cannot** match the reactor's near-total unification (where backends differ only in
`run_task` + `interrupt_reactor`), for three structural reasons:

1. Submission models are non-isomorphic and io_uring's batching has no IOCP counterpart
   (#1) — there is no shared "do the I/O" function, only a divergent `submit` seam.
2. Each backend carries an irreducible mechanism the other cannot use: IOCP's `ready_`
   CAS (#2) and `win_wait_reactor` (#4); io_uring's multishot accept (#5) and
   speculation (#3).
3. The `do_one` synchronization substrate differs at the core: IOCP leans on the kernel
   (GQCS serializes), io_uring needs an explicit userspace leader/follower + `ring_mutex_`
   (#7).

---

## 11. Scheduler architecture — io_uring joins the reactor scheduler family — [decided]

- **Current IOCP:** standalone `win_scheduler`; N threads each call
  `GetQueuedCompletionStatus` in parallel, the kernel distributes completions, no leader,
  `PostQueuedCompletionStatus` for post, dedicated timer thread.
- **Current io_uring:** bespoke `io_uring_scheduler` re-implementing the leader/follower
  event loop (run/run_one/poll/wait_one/do_one/post/work-counting/inline-budget/context)
  with a `task_running_` flag — ~1000 lines duplicating `reactor_scheduler`.
- **Reference (Asio):** ONE generic leader/follower `scheduler` shared by epoll, kqueue,
  select, AND io_uring (each a `scheduler_task` whose `run(usec, ops)` polls + fills the op
  queue). Only IOCP is standalone (`win_iocp_io_context`, parallel GQCS). Evidence:
  `scheduler.ipp:456-526` (leader/follower + `task_operation_` sentinel),
  `io_uring_service.ipp:421-517` (io_uring as a task), `win_iocp_io_context.ipp:435-460`
  (parallel GQCS).
- **Chosen for shared backend:** Two independent dedup axes.
  - *Scheduler:* `io_uring_scheduler` derives from the backend-neutral leader/follower
    `reactor_scheduler`, implementing `run_task(lock, ctx, timeout_us)` (its submit →
    `wait_cqe_timeout` → `process_completions` leader phase) + `interrupt_reactor()`
    (eventfd write), keeping `ring_mutex_`/submit-batch/cancel/`io_uring_inflight_`
    internals. IOCP scheduler is **NOT** unified — it stays standalone parallel-GQCS.
  - *Completion ops:* `proactor_op`/completion tail + socket/service templates remain shared
    across IOCP and io_uring (Phases 1, 3).
- **Why:** corosio's two leader/follower loops are the same pattern implemented twice; the
  reactor base already has the proven, adaptive version. io_uring's userspace ring (like a
  reactor's epoll fd) needs a single poller — leader/follower fits exactly. IOCP needs none
  (kernel distributes), and funneling it through a leader would serialize harvesting it does
  in parallel today — a regression. This is precisely Asio's split.
- **Reconciliation points to preserve / benchmark:**
  - io_uring switches from its `task_running_` flag to the proven `task_op_` sentinel model.
  - The "skip the kernel entry when no io_uring work" optimization
    (`io_uring_inflight_ || sq_ready || cq_ready`) must be preserved inside `run_task`
    (early-out when no kernel work and a non-blocking poll is requested) and re-benchmarked
    against the no-I/O microbench (`io_context:single_threaded`) and loopback throughput.
  - io_uring inherits the reactor's adaptive inline budget (was fixed 2/16) — behavior
    change, benchmark.
  - `ring_mutex_` + cross-thread `io_uring_submit_op` stay internal to io_uring; the
    framework only owns the dispatch op-queue, so no conflict.
- **A/B-test knob:** the sentinel vs flag loop is no longer a knob (io_uring adopts the
  shared sentinel loop); keep the `run_task` no-kernel-work early-out and the adaptive-budget
  params as the tunables to sweep. A future cleanup may rename `reactor_scheduler` to a
  backend-neutral name (it is the generic event-loop/reactor-pattern dispatcher, not
  epoll-specific) — deferred to avoid churn.

## 12. Socket layer — io_uring sockets align with the REACTOR sockets, IOCP stays separate — [decided]

- **Finding:** the socket I/O method bodies are 3-way divergent, and NOT along the
  io_uring↔IOCP line the original plan assumed:
  - reactor (epoll/…): copy iovec → speculate `readv`/`sendmsg` → on success complete;
    **on EAGAIN park on `reactor_descriptor_state`** (op re-runs the syscall via
    `perform_io()` when the fd signals ready).
  - io_uring: copy iovec → speculate `readv`/`sendmsg` (adaptive gate) → on success
    complete; **on EAGAIN submit an SQE** (kernel performs the I/O; CQE completes it).
  - IOCP: issue overlapped `WSARecv`/`WSASend` → branch sync/pending via the `ready_`
    CAS. **No speculation.**
  io_uring's socket body is nearly identical to the *reactor's* (both speculate; differ
  only in the on-EAGAIN action). IOCP is the outlier. Same shape as the Phase 2 scheduler
  finding: io_uring is a hybrid (proactor submission + reactor-style speculation + the
  reactor scheduler) that keeps aligning with the reactor family.
- **Chosen (user-approved):** align io_uring sockets with the reactor socket layer; leave
  the IOCP socket layer separate (as Asio keeps win_iocp's sockets separate). This keeps
  Phase 3 fully local/testable (io_uring + reactors on Linux) and requires **no IOCP
  split-object collapse** — the high-risk, locally-unverifiable change is dropped.
- **Why not deep io_uring↔IOCP socket templates:** (a) the I/O bodies don't share across
  the 3 op models; (b) unlocking even the service-plumbing share for IOCP would require
  collapsing IOCP's split-object model (every IOCP socket/op/service file) which cannot be
  compiled here — risk out of proportion to the boilerplate gain.
- **Realization constraint (recorded honestly):** the reactor socket templates
  (`reactor_basic_socket`/`reactor_stream_socket`) are pervasively readiness-coupled — they
  embed `reactor_descriptor_state`, `register_op` (park-on-readiness), and the
  `perform_io()` op model. io_uring has none of those. So "align io_uring with reactor
  sockets" does NOT mean io_uring derives from `reactor_stream_socket` wholesale. It means:
  extract the **readiness-agnostic** socket machinery shared by both — fd/option/endpoint/
  bind/close accessors, and the speculative-syscall + inline-completion preamble — into a
  shared base, and keep the op model + on-EAGAIN action (park vs submit-SQE) per-backend.
  The op bases already diverge (`reactor_op_base::perform_io()` vs
  `io_uring_op::prep_func/cqe_func`); that stays.
- **A/B-test note:** none (structural). The shared base must not change reactor behavior —
  the working epoll/kqueue/select tests are the regression gate at every step.

## 13. io_uring socket-service plumbing — io_uring-internal base, not reactor reuse — [decided]

- **Finding:** io_uring's 6 socket/acceptor services hand-duplicated
  construct/destroy/shutdown/close/scheduler() + the impl map (~4×55 lines for the
  socket services alone).
- **Chosen:** a new `io_uring_socket_service_base<Derived, ServiceBase, Socket>`
  (io_uring/io_uring_socket_service_base.hpp) factoring that plumbing; the 4 socket
  services (tcp/udp/local_stream/local_datagram) derive from it and keep only their
  protocol-specific open/bind/adopt. Sockets gained a `close_socket()` method (the
  existing service-`close()` body, relocated). **Behavior-neutral** (shutdown still
  `cancel()`s; close does the same work). 125/125 tests pass.
- **Why NOT reuse `reactor_socket_service`** (the cross-backend option I initially
  leaned toward): three hard frictions made it high-churn + behavior-changing for
  marginal extra sharing — (a) reactor tracks impls via `intrusive_list` + map and its
  `shutdown()` calls `impl->close_socket()` (closes fds at shutdown), whereas io_uring
  tracks via a map and cancels (no close) at shutdown — adopting it would change
  io_uring teardown semantics (a real regression surface, per the Phase 2 spin lesson);
  (b) it needs an `Impl(Derived&)` ctor while io_uring sockets are `(service&,
  scheduler&)` and are defined before the service (forcing out-of-line ctors); (c) it
  needs the intrusive node on io_uring sockets. An io_uring-internal base keeps io_uring's
  existing conventions intact at zero behavior change. The cost is one extra ~110-line
  template parallel to the reactor's — accepted.
- **Acceptor services NOT migrated:** the 2 acceptor services (tcp, local_stream) have a
  divergent `construct()` (the multishot acceptor ctor needs the peer service, which makes
  the base's `construct()` ill-formed for the acceptor type) and a multishot-specific
  `close()` (`drain_waiters_only` + breaking the `multi_op_->impl_ptr` self-cycle). Left
  as-is; only 2 services, genuinely divergent.
- **A/B-test knob:** none (structural, behavior-neutral).

## 14. io_uring file-service plumbing — separate file-service base — [decided]

- **Finding:** `io_uring_stream_file_service` and `io_uring_random_access_file_service` were
  byte-for-byte identical apart from the impl type, `open_file`'s param type, and the
  `ServiceBase`.
- **Chosen:** `io_uring_file_service_base<Derived, ServiceBase, File>`
  (io_uring/io_uring_file_service_base.hpp); the two file services derive from it and keep
  only `open_file`. No file-impl changes needed (they already have the intrusive node +
  `close_file()` + `(scheduler&)` ctor). Behavior-neutral; 39/39 io_uring+file tests pass.
- **Why a separate base from io_uring_socket_service_base (#13):** file services match the
  *reactor* socket service's shape, not the io_uring socket service's — they track via
  intrusive list + map (sockets: map only), they **close** files on shutdown (sockets:
  cancel only), and the impl ctor is `(scheduler&)` (sockets: `(service&, scheduler&)`).
  Two small bases are cleaner than one over-parameterized one.
- **A/B-test knob:** none (structural, behavior-neutral).

## 15. Unify the op envelope across all backends — `coro_op` — [implemented, verified]

- **Finding:** `reactor_op`(+`reactor_op_base`) and `proactor_op` independently defined the
  same coroutine-op envelope (h, cont_op, ex, ec_out, bytes_out, cancelled, stop_cb +
  canceller, impl_ptr, start, request_cancel), and `reactor_op_complete.hpp` repeated
  `proactor_resume`'s tail idiom inline ×7. Two near-identical envelopes + two tail-helper
  sets.
- **Chosen:** one `coro_op` (native/detail/coro_op.hpp, formerly proactor_op) shared by
  reactor_op_base, io_uring_op, and overlapped_op; one `coro_op_complete.hpp`
  (coro_drain_if_shutdown / coro_resume). The `proactor/` directory is removed. Done in 3
  phases: A (rename/move proactor_op→coro_op), B (re-base reactor_op_base/reactor_op; add a
  default ctor to coro_op for the reactors' virtual-dispatch path; reconcile cancellation
  via `on_cancel()`→ the reactor's existing `cancel()`, with no concrete-op changes), C
  (route reactor_op_complete's 7 tails through coro_resume).
- **Result model stays per-backend** (reactor errn/perform_io; io_uring res/cqe_flags; IOCP
  dwError) — only the envelope + resume tail are shared. is_read stays a reactor virtual
  (`is_read_operation()`); coro_op's `is_read` field is used by the proactors only.
- **Verified:** full suite 125/125 on Linux at each phase. **Benchmarked perf-neutral on
  BOTH backends** (this is hot-path code): epoll mean −0.24% (balanced 42/47, low-noise rows
  flat); io_uring low-noise rows flat (a 68/23 neg-skew was run-order/thermal drift — it
  flipped to +0.40% mean when the run order was reversed). All apparent >3% rows are
  high-CV large-buffer throughput noise (both signs).
- **Process note:** `corosio_bench` defaults to the **epoll** backend; earlier benchmark
  passes (Phase 2b, the service/op dedups) therefore measured epoll only and did not
  exercise io_uring. This pass ran both via `--backend epoll|io_uring`. Re-benchmarking the
  io_uring scheduler redirect (Phase 2) explicitly on `--backend io_uring` is a recommended
  follow-up to close that gap.
- **A/B-test knob:** none (structural, behavior-neutral). Scope/plan:
  tasks/coro-op-unification-scope.md.

## Implementation progress

### Phase 1 — shared op base + completion tail

- **[implemented, verified-linux]** `proactor_op_base.hpp`: non-template `proactor_op`
  holding the shared envelope (h, cont_op, ex, ec_out, bytes_out, is_read, empty_buffer,
  cancelled, stop_cb, impl_ptr) with a unified `canceller` → virtual `on_cancel()` hook,
  shared `start()` / `request_cancel()`.
- **[implemented, verified-linux]** `proactor_op_complete.hpp`: shared
  `proactor_drain_if_shutdown(owner, op)` (stop_cb disarm + shutdown-drain suicide) and
  `proactor_resume(op)` (resume-on-executor + impl_ptr keepalive drop).
- **[implemented, verified-linux]** `io_uring_op` re-based on `proactor_op`; the cancel-SQE
  logic moved from `request_cancel()` into an `on_cancel()` override; the 5 socket
  (`io_uring_socket_ops.hpp`) and 2 datagram (`io_uring_dgram_ops.hpp`) handlers converted
  to the shared prologue/resume helpers. Builds clean; 33/33 io_uring + 99/99
  io_uring+reactor tests pass (incl. cancel, native_cancel, cancel_close, stop_token).
- **[implemented, needs-windows-verify]** `overlapped_op` re-based on `proactor_op`
  (OVERLAPPED still first base; ready_/dwError/bytes_transferred/cancel_func_ stay);
  `request_cancel`/`start`/`canceller` now inherited; `on_cancel()` override =
  request_cancel + do_cancel (CancelIoEx). `is_read_` → `is_read` renamed across the 5
  IOCP files. IOCP cannot compile on Linux — awaiting a Windows build.

### Phase 2 — io_uring joins the reactor scheduler family

- **[implemented, verified-linux]** `reactor_scheduler` generalized with a no-op
  `ensure_initialized()` hook (called at the top of run/run_one/poll/poll_one/wait_one);
  io_uring overrides it to `lazy_init_ring()`. epoll/kqueue/select unaffected.
- **[implemented, verified-linux]** `io_uring_scheduler` now derives from
  `reactor_scheduler`. Removed ~600 lines of duplicated loop/post/work/budget/context code
  (run/run_one/poll/poll_one/wait_one, do_one, post×2, work_started/finished, stop/stopped/
  restart, running_in_this_thread, reset/try_consume_inline_budget, the frame stack + run
  guard, single_threaded toggle). io_uring keeps ring_/ring_mutex_/io_uring_inflight_/
  wakeup_eventfd_/submit-batch/cancel machinery/process_completions, and implements
  `run_task()` (submit → wait_cqe_timeout → process_completions) + `interrupt_reactor()`
  (eventfd) + `configure_single_threaded` (base + ring_mutex_). The ctor seeds the
  `task_op_` sentinel. io_uring gains the reactor's adaptive inline budget for free.
- Full suite 125/125 pass on Linux (clang, liburing 2.14).
- **Regression found + fixed during 2a (root-caused via systematic debugging):** the first
  `run_task` omitted the `task_cleanup` RAII guard. `process_expired()` posts timer
  completions via `sched_->post()`, which on a run thread routes to the thread-local
  `private_queue`. With no `task_cleanup` to splice it back into `completed_ops_`, the timer
  op stranded there: `do_one`'s `more_handlers` check stayed true (it inspects
  `private_queue`), so `run_task` was called non-blocking forever (99% CPU spin on
  `timer.io_uring`) and timer-driven shutdowns in the stress tests hung. Fix: add
  `task_cleanup on_exit{this, &lock, ctx}` to `run_task`, exactly as `epoll_scheduler` does.
  Lesson for the optimization pass: any per-backend `run_task` MUST drain the private queue
  on every exit path.
- **Deferred to Phase 2b/optimization:** `submit_sqes_op` was kept (redundant-but-correct
  flushing alongside `run_task`'s sentinel-driven flush). Evaluate removing it once the
  sentinel-batching behavior is benchmarked. A/B: see entry #11 (no-kernel-work early-out;
  adaptive vs fixed budget).

#### Phase 2b benchmark — redirect is performance-neutral (verified)

Full corosio suite, clang Release+LTO, 3s × 5 iters, pinned to one CCX (cores
0-7,16-23), governor=performance. Baseline = `a8f1d188` (pre-redirect io_uring
scheduler); head = `d8dc35b5` (io_uring derives from reactor_scheduler). Report:
`bench_results/benchmark-phase2-vs-baseline.md`.

- `io_context:single_threaded` (no-I/O microbench, the metric flagged for the
  `has_kernel_work` early-out): 5.94 → 5.85 Mops/s, **-1.5%**, but base CV 0.9% /
  head CV 2.0% → within ~1σ, statistically insignificant. Early-out preserved it.
- TCP `socket_latency`/`socket_throughput`: uniformly **+0.4%..+1.2%** (low CV).
- `local_socket_latency`: **+1.5%..+6.0%** (pingpong/1024 +6.0% @ CV 0.8% is real);
  `io_context:multithreaded/8` +3.4%. Consistent with io_uring gaining the
  reactor's adaptive inline budget.
- Apparent >3% regressions all on `local_socket_throughput:unidirectional` large
  buffers (-3.0%..-12.1%) at CV 6.4-13.7% — memory-bandwidth-bound noise, not real
  (noise band dwarfs the diff).

Conclusion: ~560-line scheduler dedup at zero perf cost, slight net gain on socket
workloads. Redirect accepted. (A/B knobs from entry #11 remain available for the
optimization pass: no-kernel-work early-out, adaptive vs fixed budget, dropping
`submit_sqes_op`.)

> **CORRECTION (this claim was wrong — measured on the wrong backend).** The Phase 2b
> run above was taken with the **default** backend (epoll), so it never exercised the
> io_uring `run_task` path it claimed to validate. Re-benchmarking explicitly with
> `--backend io_uring` exposed a **severe** TCP regression: `socket_latency:pingpong/1`
> ~140K → ~17–58K ops/s (TCP-specific; AF_UNIX only −8%). Root-caused below.

#### Phase 2c — the real io_uring regression: root cause + fix (verified-linux)

Re-benchmarking with `--backend io_uring` (the gap the Phase 2b claim hid) found a TCP
single-stream latency cliff. `strace` showed ~93K `io_uring_enter({0,0})→ETIME`
non-blocking peeks per second — one wasted GETEVENTS syscall after *every* inline
`readv`/`sendmsg`, all succeeding (100% inline speculation, zero EAGAIN). Three
independent defects, all introduced by the scheduler/op dedup, compounded:

1. **Dead inline budget.** The reactor calls `reset_inline_budget()` at the top of every
   `complete_*_op` (reactor_op_complete.hpp); the io_uring completion handlers called it
   **nowhere**. The shared `reactor_scheduler_context` starts with `inline_budget == 0`, so
   `try_consume_inline_budget()` always failed → **every** speculative io_uring op bounced
   through `completed_ops_` instead of chaining inline → `run_task` (and its peek) fired on
   essentially every op. epoll never regressed precisely because its completion path *does*
   reset the budget. **Fix:** call `sched_->reset_inline_budget()` at the top of each
   io_uring completion handler (read/write/connect/wait/local_connect in
   io_uring_socket_ops.hpp, send/recv in io_uring_dgram_ops.hpp, read/write in
   io_uring_file_ops.hpp), mirroring the reactor. Restored ~80% budget-hit rate.

2. **`io_uring_inflight_` leak defeating the `has_kernel_work` early-out.** `drain_cqes_for`
   (acceptor teardown) consumed the multishot-accept terminal CQE **and** the cancel CQEs it
   reaps **without decrementing** `io_uring_inflight_`. Since `make_socket_pair`'s acceptor
   is destroyed *before* the bench loop, the counter entered the loop stuck ≥1, so
   `has_kernel_work` (`inflight != 0 || sq_ready || cq_ready`) was permanently true and the
   non-blocking early-out never fired → the peek ran on every `run_task`. **Fix:**
   `drain_cqes_for` now decrements `io_uring_inflight_` for every consumed terminal
   (`ud != nullptr && !F_MORE`) CQE, on the same terms as `process_completions`. With this,
   the loop runs at `inflight == 0` and the early-out fires.

3. **Early-out skipped expired timers (latent; unmasked by fix #2).** The `has_kernel_work`
   early-out `return`ed *before* `timer_svc_->process_expired()`. While #2 kept the early-out
   from ever firing this was invisible; once #2 let it fire, a fired corosio timer (steady-
   clock state, independent of the ring) was stranded when the ring was otherwise idle —
   hanging any timer-driven all-inline workload (the `socket_stress.sync_completion`/
   `concurrent_ops` io_uring tests hung under ASAN). **Fix:** restructure `run_task` so only
   the *kernel* pass (submit + wait_cqe + drain) is skipped by the early-out; `process_expired()`
   always runs.

Result: io_uring `socket_latency` restored to **137–141 Kops/s** across all 12 variants
(pingpong / lockless / concurrent × {1,64,1024}/{1,4,16}) — on par with the ~140K baseline
and with epoll (135–143K). epoll unaffected. Full ASAN suite passes (incl. the two stress
tests that previously hung). **A/B knob:** all three are correctness/parity fixes, not
tunables; the no-kernel-work early-out (entry #11) is still the knob to flip if measuring
its isolated effect. **Remaining candidate (low impact):** the multishot acceptor's
accept-dispatch path does not reset the inline budget (the reactor's `complete_accept_op`
does); accept is cold (once per connection) and the first read/write resets the budget, so
left as-is — noted here so it is not mistaken for an oversight.

### Phase 3 — socket layer: native_socket_base shared by reactors + io_uring (consolidate)

- **[implemented, verified-linux]** Extracted `native_socket_base<Derived, ImplBase,
  Endpoint>` (include/.../native/detail/native_socket_base.hpp): the readiness/completion-
  agnostic socket surface — `fd_` + `local_endpoint_` (mutable, for lazy resolution),
  `ImplBase` + `enable_shared_from_this<Derived>` bases, and the synchronous accessors
  `native_handle`/`is_open`/`set_option`/`get_option`/`set_socket`/`set_local_endpoint`/
  `do_bind`.
- `reactor_basic_socket` now derives from it (keeps the readiness layer: `intrusive_list`
  node for the reactor's impl list, `svc_`, `desc_state_`, `register_op`/cancel/close that
  deregister from the reactor). `using`-declarations expose the inherited `fd_`/
  `local_endpoint_` to the template's own unqualified references.
- All four io_uring socket types (`io_uring_tcp_socket`, `io_uring_udp_socket`,
  `io_uring_local_stream_socket`, `io_uring_local_datagram_socket`) now derive from
  `native_socket_base` and drop their duplicated `fd_`/`local_endpoint_` members and
  `native_handle`/`set_option`/`get_option` (+ the simple `local_endpoint()` for udp/local).
  io_uring's TCP socket keeps its lazy 3-state `local_endpoint()` override; io_uring keeps
  `family_`, `sched_`/`svc_`, the op slots, `spec_`, and (map-based) service tracking — so it
  does NOT inherit the reactor's intrusive-list node (moved down to `reactor_basic_socket`).
- Full suite 125/125 pass on Linux. No IOCP changes (no Windows build needed for Phase 3).

#### Why the socket layer stops here (the explained limit, per the user's standing ask)

The socket *I/O method bodies* (`read_some`/`write_some`/`connect`) are 3-way divergent and
do NOT share cleanly:
- **reactor**: speculate `readv`/`sendmsg` → on EAGAIN **park the op on
  `reactor_descriptor_state`**; the op re-runs the syscall via `perform_io()` on readiness.
- **io_uring**: speculate `readv`/`sendmsg` → on EAGAIN **submit an SQE**; the kernel performs
  the I/O and a CQE completes it (op carries `prep_func`/`cqe_func`, not `perform_io`).
- **IOCP**: issue overlapped `WSARecv`/`WSASend` → branch sync/pending via the `ready_` CAS;
  **no speculation**.
The op objects themselves differ (`reactor_op_base::perform_io()` vs
`io_uring_op::prep_func/cqe_func` vs `overlapped_op` OVERLAPPED/CAS), so there is no common
"do the I/O" function. The genuinely shared surface is the op envelope (Phase 1), the
scheduler loop (Phase 2, io_uring↔reactor), and the readiness-agnostic socket accessors
(this phase). Deeper sharing of the I/O bodies would mean either heavy `if constexpr`/policy
indirection or generalizing the working reactor socket templates with an on-EAGAIN hook +
op-model abstraction — judged not worth the risk/complexity for the remaining gain.
The IOCP socket layer stays separate (no split-object collapse), mirroring how Asio keeps
its win_iocp sockets separate from the POSIX socket layer.

### Phase 4 — shared result-decode helper (`decode_io_result`) [implemented, verified-linux]

Resolves the Phase-1 `decode_result` deferral below. The `(cancelled -> canceled / err ->
make_err / read && 0 bytes && !empty -> eof / else success)` priority was duplicated ~20×
across the three backends (reactor inlined it in all 7 `complete_*_op`; io_uring had
`uring_set_result` plus inlined copies in wait, both dgram handlers, and all 8 inline
speculative-success fast paths in `io_uring_types.hpp`; IOCP inlined it in `invoke_handler`).

- **Chosen seam (NOT a `Traits` hook):** a free `decode_io_result(ec_out, cancelled, err,
  is_read, bytes, empty_buffer)` in `coro_op_complete.hpp`. The only real divergence — the
  native-error encoding (reactor positive `errno`, io_uring negative `res`, IOCP `DWORD`) —
  stays backend-local: each caller converts to a `std::error_code` in one line and passes it
  in (`{}` == success). The helper owns only the (identical) priority logic. A `std::function`
  callback design was rejected — this is the hottest path in the library, so the helper is a
  plain inline function over already-normalized inputs.
- **Wired:** reactor (all 7 `complete_*_op`), io_uring (`uring_set_result`, wait, both dgram
  do_handlers, and the 8 inline speculative-success paths in `io_uring_types.hpp`), IOCP
  (`win_overlapped_op::invoke_handler`). 13 completion-handler sites + 8 io_uring inline sites.
- **Behavior-preserving, verified:** the reactor's `is_read_operation()` already folds in the
  empty-buffer case (it returns false for a zero-length read), so the reactor passes
  `empty_buffer=false` and the shared EOF test reduces to its original `is_read && bytes==0`.
  Datagrams pass `is_read=false` (a 0-byte datagram is success, not EOF), matching prior
  behavior. Full ASAN suite 125/125; `socket_latency`/`local_socket_latency` perf flat
  (io_uring 137K, epoll 135K, local 324K — unchanged).
- **Not converted (deliberate):** the degenerate "already stop-requested" bypasses in
  `io_uring_types.hpp` (`if (ec) *ec = canceled;`) — not decode-shaped, a forced-cancel fast
  path. The IOCP acceptor/signal/resolver/file-service decode sites and the posix
  resolver/signal/file completion paths (different result shapes — addrinfo/signal number)
  are left for the IOCP Windows pass; only `invoke_handler` (the shared socket completion
  point) is wired so far. **IOCP edit is unverified — needs a Windows build.**

#### Phase 1 deferrals (to Phase 3)

- **`decode_result`** — DONE in Phase 4 above (as a free helper, not a `Traits` hook).
  Original note: Phase 1 kept decode backend-local (io_uring's `uring_set_result`, IOCP's
  inline `invoke_handler` decode) and shared only the backend-agnostic prologue/resume tail.
- **io_uring file ops** (`io_uring_file_ops.hpp`): the EMBEDDED file ops
  (`uring_file_read_op` / `uring_file_write_op`) are now routed through
  `proactor_drain_if_shutdown` + `proactor_resume` (done in the follow-up dedup pass,
  behavior-identical). The HEAP-allocated ops — `uring_random_access_read_op` /
  `uring_random_access_write_op` (and the accept ops `uring_accept_op`) — intentionally
  keep their `new`/`delete` tail: the shared helpers move the `impl_ptr` keepalive, which
  has no meaning for an op that owns itself and `delete`s on completion. The shared
  `finish()` decode helper still serves the heap ops.
- **IOCP completion tail** (`invoke_handler`) NOT yet routed through `proactor_resume`:
  IOCP ops still use the split-object `internal_ptr` keepalive, not the base `impl_ptr`.
  Folds in with the IOCP single-object collapse in Phase 3.
