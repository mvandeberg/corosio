## IOCTX (io_context:single_threaded)

Profile of corosio (io_uring backend) vs asio (BOOST_ASIO_HAS_IO_URING) running
`--category io_context --bench single_threaded` for 10 s after a 0.5 s warmup.
Both `single_threaded` and `single_threaded_lockless` are included (prefix match
on `single_threaded`). Pinned to CCX `taskset -c 0-7,16-23`. `perf record -F 999
-g --call-graph fp -m 32` (the small mmap buffer was required because default
perf locked enough memory to exhaust the 8 MB memlock limit and starve
`io_uring_queue_init`).

**Throughput observed during profiling:**
- corosio: 2.18 Mops/s (single_threaded), 2.27 Mops/s (single_threaded_lockless)
- asio:    3.30 Mops/s (single_threaded), 3.39 Mops/s (single_threaded_lockless)
- corosio is at ~66% of asio's throughput; loses ~34% on this micro.

(The plan referenced "~93% loss"; on this run the gap is ~34%. The profile
still shows a clean dispatch-overhead signal — just calibrating expectations.)

**Top 5 hot symbols — corosio (flat, `--no-children`):**
1. `entry_SYSRETQ_unsafe_stack` (kernel) — 25.92% cycles. Per the call-graph,
   ~13.3 pp is `__vdso_clock_gettime` reached from
   `timer_service::process_expired()` inside `io_uring_scheduler::do_one`, and
   ~12.3 pp is `pthread_mutex_lock` from the same `process_expired`.
2. `__pi__text` (kernel) — 3.80%.
3. `__do_sys_io_uring_enter` (kernel) — 3.63%. Real `io_uring_enter` syscalls
   on a benchmark with **no I/O**.
4. `__vdso_clock_gettime` — 3.37%. Standalone (in addition to time spent in
   kernel SYSRET).
5. `pthread_mutex_lock` (libc) — 2.62%.

Other notable corosio entries: `io_uring_scheduler::do_one` 2.59%,
`io_cqring_wait` (kernel) 2.38%, liburing internals 2.05%,
`io_uring_scheduler::poll` 1.04%.

**Top 5 hot symbols — asio:**
1. `__irqentry_text_end` (kernel) — 5.68%.
2. `co_spawn_entry_point<...> [clone .resume]` — 4.34%. The coroutine entry
   point itself.
3. `do_anonymous_page` (kernel) — 3.44%. Page-fault path for fresh coroutine
   frames.
4. libc internal at `+0xa5cd2` — 2.62% (looks like the malloc fast path /
   `_int_free` neighbourhood; symbol is anonymous).
5. `boost::asio::co_spawn<...>` (entry shim) — 2.45%.

Other notable asio entries: `awaitable_thread::pump` 1.83%,
`awaitable_thread::~awaitable_thread` 1.71%, `cfree` 1.58%,
`thread_info_base::allocate<awaitable_frame_tag>` 1.55%,
`__handle_mm_fault` 2.35%, `clear_page_erms` 1.53%.

**Corosio-only symbols in the top 25** (functions hot in corosio that do NOT
appear in asio's top 25):
- `boost::corosio::detail::io_uring_scheduler::do_one(long)` — 2.59%. The
  scheduler's dispatch step that runs `io_uring_submit_and_get_events`,
  `process_completions`, and `timer_svc_->process_expired()` *on every
  invocation*, then dispatches one ready op (`io_uring_scheduler.hpp:832`).
- `__do_sys_io_uring_enter` / `io_cqring_wait` / liburing internals — together
  ~8% kernel-side. There is no I/O in this benchmark, yet every `do_one(0)`
  unconditionally calls `io_uring_submit_and_get_events` on the ring
  (`io_uring_scheduler.hpp:852`).
- `__vdso_clock_gettime` (standalone) — 3.37%. Reached from
  `timer_service::process_expired` (`timer_service.hpp:679`,
  `clock_type::now()`) which corosio runs once per `do_one`.
- `pthread_mutex_lock` — 2.62%. The timer-service mutex
  (`timer_service.hpp:678`) acquired once per `do_one`, even though no timers
  are ever registered in this micro.
- `boost::corosio::detail::io_uring_scheduler::poll()` — 1.04%. Top-level loop
  that calls `do_one(0)` until empty.
- `boost::corosio::detail::io_uring_scheduler::post(coroutine_handle)` — 0.58%.
  Heap-allocates a `post_handler` op per spawned coroutine
  (`io_uring_scheduler.hpp:602`).
- `boost::capy::recycling_memory_resource::deallocate_slow` — 0.81%,
  `allocate_fast` — 0.59%, `allocate_slow` — 0.52%. The capy thread-local
  recycling resource backing coroutine-frame allocation. (Asio has an
  analogous `thread_info_base::allocate` that *is* in its top 25.)
- `run_async_trampoline / run_async_wrapper` (capy) — 0.53% + 0.47%. The
  `capy::run_async(ex)(...)` entry point that wraps the user task.

**Counter deltas (from `IOCTX_stat.txt`, 5 s of each `--bench
single_threaded` run × 2 benchmarks = ~22 M items corosio / ~33 M items asio):**
- IPC: corosio 1.354, asio 2.316 (corosio is **−41.6%**).
- Branch-miss rate: corosio 3.02%, asio 2.28%.
- L1d cache-miss rate (of cache-references): corosio 1.02%, asio 5.11%.
  Asio takes more cache misses *because it is page-faulting in fresh coroutine
  frames* (visible as `do_anonymous_page`, `clear_page_erms`,
  `__handle_mm_fault`, `lru_add` in asio's top symbols).
- dTLB load misses: corosio 5.67 M, asio 14.49 M (same reason as above).
- Context switches: corosio 147, asio 122 (essentially equal — single-threaded
  bench, no real preemption).
- LLC-load-misses: `<not supported>` on this host.
- Per-item: corosio executes **fewer** instructions (3676 vs 4191) but spends
  **more** cycles (2715 vs 1809). Corosio's deficit is not "running more code";
  it is "code that retires fewer instructions per cycle" — the classic
  signature of syscalls, locked atomics, and memory stalls dominating cycles.

**Working hypothesis:**
The per-dispatch cost in corosio's `io_uring` backend is dominated by three
fixed costs that fire on *every* `do_one(0)` regardless of whether they are
needed: (a) an unconditional `io_uring_submit_and_get_events` syscall
(`io_uring_scheduler.hpp:852`), (b) a `pthread_mutex_lock` plus
`clock_gettime` inside `timer_service::process_expired`
(`timer_service.hpp:678-679`), and (c) the surrounding `ring_mutex_` /
`dispatch_mutex_` acquisitions.

> **Context for (b) — do not naively delete the timer drain.** The
> unconditional `timer_svc_->process_expired()` at the top of `do_one`
> was added in commit 2c73112d as a correctness fix: without it,
> continuous loopback I/O keeps `completed_ops_` non-empty across
> iterations, the leader-wait branch never runs, and stopper-timer
> shutdowns deadlock (regression tests
> `socket_stress.concurrent_ops.io_uring` /
> `socket_stress.sync_completion.io_uring`). The cost measured here is
> O(1) when no timer is due (heap-top compare + one mutex acquire), so
> the optimization is to *skip* the call when the timer service is
> empty — not to remove it. `timer_service` exposes
> `empty()` (line 161, atomic, lock-free) and `nearest_expiry()` (line
> 168, atomic) for exactly this purpose. The same pattern applies to
> (a): `submit_and_get_events` is needed to drain kernel CQEs that
> arrive during continuous I/O, but on `do_one(0)` with no in-flight
> SQEs and no pending CQEs, the syscall is pure overhead. Because `poll()` calls `do_one(0)` once per
dispatched handler, in the synthetic loop these fixed costs are amortized over
a single op — exactly the worst case. Asio's `single_threaded` (and especially
`single_threaded_lockless` with `BOOST_ASIO_CONCURRENCY_HINT_UNSAFE`) does no
analogous per-handler kernel call or timer sweep; its dominant cost is the
coroutine-frame allocation page-fault traffic, which is *amortized work*
rather than per-handler overhead.

Hypothesis only for the lockless variant — would need confirmation by an
experiment that disables the timer sweep / ring pump on `do_one(0)` (or
batches them every N dispatches) and re-runs the same micro. The lockless
variant has the same call sequence in the current implementation, which is
consistent with it gaining only ~4% over the locked variant (vs asio's
lockless variant gaining ~3%).

---

**Source citations (for traceability):**
- corosio scheduler `do_one`: `include/boost/corosio/native/detail/io_uring/io_uring_scheduler.hpp:832-883`
- corosio scheduler `poll`: `include/boost/corosio/native/detail/io_uring/io_uring_scheduler.hpp:800-816`
- corosio scheduler `post(coroutine_handle)`: `include/boost/corosio/native/detail/io_uring/io_uring_scheduler.hpp:602-624`
- corosio timer-service `process_expired`: `include/boost/corosio/detail/timer_service.hpp:672-705`
- corosio bench source: `perf/bench/corosio/io_context_bench.cpp:43-69` (`bench_single_threaded_post`), `:261-290` (`bench_single_threaded_lockless`)
- asio bench source: `perf/bench/asio/coroutine/io_context_bench.cpp:42-66`, `:191-215`

**Artifacts in this directory:**
- `IOCTX_corosio.{data,folded,svg}` - corosio profile
- `IOCTX_asio.{data,folded,svg}` - asio profile
- `IOCTX_diff.svg` - differential flamegraph (red = corosio hotter)
- `IOCTX_stat.txt` - perf stat microarchitectural counters
- `IOCTX_top_symbols.txt` - flat top symbols (raw)
- `IOCTX_{corosio,asio}_stdout.txt` - captured benchmark stdout

---

## FAN (fan_out:fork_join/16)

Profile of corosio (io_uring backend) vs asio (BOOST_ASIO_HAS_IO_URING) running
`--category fan_out --bench fork_join/16` for 10 s after a 0.5 s warmup. CCX-pinned
(`taskset -c 0-7,16-23`). `perf record -F 999 -g --call-graph fp -m 32 --per-thread`.

> Note on `--per-thread`: the IOCTX captures used per-CPU recording, but FAN
> `fork_join/16` constructs 16 connected TCP socket pairs and asio's io_uring
> backend allocates per-socket ring memory at registration time. Per-CPU perf
> mmap buffers consumed enough of the 8 MB RLIMIT_MEMLOCK that
> `io_uring_queue_init` returned `ENOMEM` even with `-m 1`. `--per-thread`
> attaches the ring buffer to the bench thread only and resolved the
> allocation pressure without sudo/prlimit. Sample counts (~10500 per side)
> are comparable to the IOCTX runs.

> Note on bench asymmetry: corosio's `fork_join` calls
> `set_option(no_delay(true))` on both ends of each socket pair
> (`perf/bench/corosio/fan_out_bench.cpp:100`); asio's does not
> (`perf/bench/asio/coroutine/fan_out_bench.cpp:87-92`). Both run over the
> loopback interface with 64-byte messages, so TCP_NODELAY only matters if
> Nagle would actually defer; with synchronous request/response and no
> pipelining the effect is small but is a real difference between the two
> benches.

**Throughput observed during profiling:**
- corosio: 6.52 Kops/s (fork_join/16, 65204 ops in 10 s)
- asio:    9.10 Kops/s (fork_join/16, 90973 ops in 10 s)
- Loss vs asio: **−28.7%** (corosio at ~71.7% of asio)

For context: corosio mean latency 153 us, asio mean latency 110 us. Each "op"
is a full fan-out of 16 client writes + 16 server reads + 16 server writes +
16 client reads + a zero-duration timer round-trip on the parent.

**Top 5 hot symbols — corosio (flat, `--no-children`):**
1. `entry_SYSRETQ_unsafe_stack` (kernel) — 7.57%. Catch-all kernel-syscall
   return symbol; the call-graph attributes ~1.5 pp to write paths via
   `io_uring_tcp_socket::write_some` and the rest to recv/io_uring syscalls.
2. `tcp_ack` (kernel) — 2.67%.
3. `srso_alias_return_thunk` (kernel) — 1.85%. SRSO mitigation thunk; charged
   to many kernel call sites.
4. `tcp_sendmsg_locked` (kernel) — 1.77%.
5. `__tcp_transmit_skb` (kernel) — 1.76%.

The single hottest *userspace* corosio symbol is `capy::write` (the resume
shim) at 0.53%; the scheduler `do_one` is at 0.50%. Kernel TCP processing
dominates this benchmark — see "Comparison to IOCTX" below.

**Top 5 hot symbols — asio:**
1. `tcp_ack` (kernel) — 3.50%.
2. `__tcp_transmit_skb` (kernel) — 2.60%.
3. `tcp_sendmsg_locked` (kernel) — 2.25%.
4. `srso_alias_safe_ret` (kernel) — 2.04%.
5. `srso_alias_return_thunk` (kernel) — 1.92%.

Asio's hottest userspace symbol is `io_uring_service::io_queue::perform_io`
at 0.33%.

**Corosio-only symbols in the top 25 userspace** (with one-sentence "what does
this function do", citing file:line):
- `capy::write<...native_tcp_socket<io_uring_t>...>.resume` — 0.53%. Resume
  point of the `capy::write` composed operation that loops `write_some`
  until the full buffer is sent
  (`capy::write` is defined in capy's headers; see `boost/capy/write.hpp`).
- `io_uring_scheduler::do_one(long)` — 0.50%. The scheduler's per-iteration
  dispatch step
  (`include/boost/corosio/native/detail/io_uring/io_uring_scheduler.hpp:832`).
  Now visible at half its IOCTX share (was 2.59% there) because real I/O
  work amortizes the fixed cost.
- `native_write_awaitable::await_suspend` — 0.39%. Thin awaitable that
  forwards to `io_uring_tcp_socket::write_some`
  (`include/boost/corosio/native/native_tcp_socket.hpp:151-157`).
- `io_uring_tcp_socket::read_some` — 0.38%. The read implementation including
  the speculative `readv` fast path added in 65043641
  (`include/boost/corosio/native/detail/io_uring/io_uring_types.hpp:128-180`).
- `io_uring_tcp_socket::write_some` — 0.19%. The write counterpart with the
  speculative `writev` fast path
  (`include/boost/corosio/native/detail/io_uring/io_uring_types.hpp:209` ff.).
- `buffer_param::copy_impl<ConstBufferSequence,...>` — 0.18% + 0.05% (two
  instantiations). The type-erased buffer-sequence iterator that fills an
  `iovec[]` for each socket op
  (`include/boost/corosio/detail/buffer_param.hpp:333-365`).
- `io_uring_scheduler::process_completions()` — 0.16%. Drains the CQ ring
  into the scheduler's ready queue
  (`include/boost/corosio/native/detail/io_uring/io_uring_scheduler.hpp`).
- `uring_read_op::do_handler` — 0.18%. Completion-handler invoked when the
  read CQE arrives; restores the suspended coroutine and resumes it (same
  header).
- `io_uring_scheduler::run()` — 0.27%. Top-level event loop calling `do_one`
  until stopped.

**Counter deltas (from `FAN_stat.txt`, 5 s each):**
- IPC: corosio **0.820**, asio **1.091** (corosio **−25%**).
- Branch-miss rate (of branches): corosio **9.27%**, asio **8.04%**.
- Cache-miss rate (of cache-references): corosio **0.102%**, asio **0.140%**.
  (Asio takes proportionally more L1d misses — different from IOCTX where
  the gap was much larger; here both libraries are doing real kernel-side
  work that shares the kernel's working set.)
- dTLB load misses: corosio 1.42 M, asio 0.43 M (corosio sees **3.3×** more).
- Context switches: corosio 149, asio 202. CPU-migrations: corosio 97, asio
  165. Both benches are single-bench-thread + 16 sockets, so context switches
  reflect kernel softirq / network-stack scheduling; asio's higher count
  tracks its higher op rate.
- LLC-load-misses: `<not supported>` on this host.
- Per-op (5 s window, corosio 32637 ops, asio 45786 ops):
  - corosio: **939 K cycles/op, 770 K instructions/op**
  - asio:    **668 K cycles/op, 729 K instructions/op**
  - Corosio runs only ~6% more instructions per op but spends **41% more
    cycles per op** — the same IPC-deficit signature as IOCTX, only this
    time the lost cycles are spent in kernel TCP processing rather than the
    scheduler. Corosio's user/sys breakdown is 0.79 s user / 3.43 s sys; asio
    is 0.27 s user / 3.58 s sys (similar sys, much more user time for
    corosio — again, larger per-op userspace path).

**Comparison to IOCTX:**

- The IOCTX-flagged corosio-only symbols are **dramatically smaller** in FAN.
  `process_expired` was implicated as ~13 pp of IOCTX (via
  `entry_SYSRETQ_unsafe_stack` → `__vdso_clock_gettime` and `pthread_mutex_lock`
  from `timer_service::process_expired`); in FAN, `process_expired` itself is
  **0.05%**, `__vdso_clock_gettime` standalone is **0.31%**, and
  `pthread_mutex_lock` is **0.35%**. Likewise `io_uring_submit_and_get_events`
  is **0.05%** (was ~8% in IOCTX). The per-`do_one` fixed costs are now
  amortized across genuine per-iteration work and are no longer the bottleneck.
- New userspace symbols hot in FAN but absent from IOCTX top-25:
  `capy::write.resume`, `native_write_awaitable::await_suspend`,
  `io_uring_tcp_socket::{read_some,write_some}`, `buffer_param::copy_impl`,
  `uring_read_op::do_handler`, `io_uring_scheduler::process_completions`. These
  are all per-socket-op userspace work, not coroutine-spawn cost.
- **No coroutine-spawn allocator/page-fault hot spots appear in FAN.** IOCTX
  showed asio dominated by `do_anonymous_page` / `clear_page_erms` /
  `__handle_mm_fault` / `thread_info_base::allocate` from fresh awaitable
  frames; none of that is in FAN's top 25 for either side. The 16 child
  coroutines per iteration evidently reuse their frames via asio's
  `awaitable_thread` pool and capy's `recycling_memory_resource`, so the
  spawn path is **not** an additional cost over IOCTX — the loss is the same
  signature as IOCTX (IPC deficit) but moved from scheduler fixed costs into
  per-socket-op userspace work.

**Working hypothesis:**

The 28.7% FAN loss is dominated by per-socket-op userspace overhead — the
`io_uring_tcp_socket::{read_some,write_some}` path (speculative `readv`/`writev`
fast path, iovec construction via `buffer_param::copy_impl`,
`native_{read,write}_awaitable::await_suspend` shim, and the surrounding
`capy::{read,write}` composed-op resume) — rather than the per-dispatch fixed
costs identified in IOCTX. Corosio's per-op userspace path is wider than asio's
io_uring_socket_service path despite executing only ~6% more retired
instructions; the cycle deficit (corosio 939 K cyc/op vs asio 668 K cyc/op)
points to lower IPC in that path (0.82 vs 1.09) — likely from branch mispredicts
in the buffer-iteration / iovec-construction code (corosio branch-miss rate
9.27% vs asio 8.04%) and from the speculative-readv path's branchy short-read
handling. Hypothesis only — would need confirmation by either (a) profiling a
variant of the corosio bench with the speculative fast path disabled, or
(b) `perf record -e branch-misses` to attribute mispredicts directly to
specific source lines in `io_uring_types.hpp`.

**Important context (same as IOCTX):**
Any high cost in `timer_service::process_expired()` is *correctness overhead*
from commit 2c73112d, not inherent — see the IOCTX section's blockquote for why.
In FAN this cost is already small (0.05%) because each `do_one` does real
I/O work, so the optimization (skipping when `timer_service::empty()` is true)
will not move the FAN number measurably. The FAN loss lives elsewhere.

---

**Source citations (FAN-specific, for traceability):**
- corosio bench source: `perf/bench/corosio/fan_out_bench.cpp:75-145`
  (`bench_fork_join`)
- asio bench source: `perf/bench/asio/coroutine/fan_out_bench.cpp:75-160`
  (`bench_fork_join`)
- corosio `io_uring_tcp_socket::read_some` (with speculative readv):
  `include/boost/corosio/native/detail/io_uring/io_uring_types.hpp:128-205`
- corosio `io_uring_tcp_socket::write_some` (with speculative writev):
  `include/boost/corosio/native/detail/io_uring/io_uring_types.hpp:209` ff.
- corosio `native_write_awaitable::await_suspend`:
  `include/boost/corosio/native/native_tcp_socket.hpp:151-157`
- corosio `buffer_param::copy_impl`:
  `include/boost/corosio/detail/buffer_param.hpp:333-365`

**FAN artifacts in this directory:**
- `FAN_corosio.{data,folded,svg}` - corosio profile
- `FAN_asio.{data,folded,svg}` - asio profile
- `FAN_diff.svg` - differential flamegraph (red = corosio hotter)
- `FAN_stat.txt` - perf stat microarchitectural counters
- `FAN_top_symbols.txt` - flat top symbols (raw)
- `FAN_{corosio,asio}_stdout.txt` - captured benchmark stdout

## ACC (accept_churn:concurrent/4)

Profile of corosio (io_uring) vs asio (io_uring via `BOOST_ASIO_HAS_IO_URING`)
running `--category accept_churn --bench concurrent/4` for 10 s after 0.5 s
warmup. CCX-pinned (`taskset -c 0-7,16-23`). Recording used
`perf record -F 999 -g --call-graph fp --per-thread -m 1` — the standard
`-m 32` form failed with `io_uring_queue_init_params: Cannot allocate memory`
because the bench process opens enough io_uring rings (acceptor + per-socket
on both sides plus internal asio rings) to exhaust the unprivileged memlock
quota when perf's shared mmap buffers are also charged against it. Per-thread
mmap buffers sidestep that. Caveat: per-thread sampling loses some kernel-side
samples, so the kernel breakdowns below are a slight under-count but cross-side
ratios remain meaningful (both runs used the same mode).

**Throughput observed during profiling:**
- corosio: 29.57 Kops/s (295667 ops over 10 s, 4 concurrent loops)
- asio:    38.91 Kops/s (389112 ops over 10 s, 4 concurrent loops)
- Loss vs asio: **24.0 %**

(`perf stat` 5 s run gave the same shape: 29.62 Kops vs 39.20 Kops, 24.4 %
loss — consistent.)

**Top 5 hot symbols — corosio (overall, kernel-dominated for I/O bench):**
1. `entry_SYSRETQ_unsafe_stack` (kernel) — 6.92 %
2. `srso_alias_safe_ret` (kernel SRSO mitigation) — 2.83 %
3. `srso_alias_return_thunk` (kernel SRSO mitigation) — 2.04 %
4. `nf_conntrack_in` (kernel) — 1.16 %
5. `__tcp_transmit_skb` (kernel) — 1.15 %

**Top 5 USERSPACE hot symbols — corosio (from `ACC_top_user_symbols.txt`):**
1. `std::__introsort_loop<...>` (latency-stats post-processing, not the hot loop) — 0.50 %
2. `bench_concurrent_churn<io_uring_t>::...::operator()(int).resume` (the coroutine frame itself) — 0.29 %
3. `io_uring_tcp_socket::set_option(int,int,void const*,unsigned long)` — 0.20 %
4. `std::__shared_ptr<io_uring_tcp_socket,...>::__shared_ptr<...>(...)` (socket shared_ptr ctor) — 0.18 %
5. `io_uring_scheduler::process_completions()` — 0.13 %

**Top 5 hot symbols — asio (overall):**
1. `entry_SYSRETQ_unsafe_stack` (kernel) — 3.18 %
2. `srso_alias_safe_ret` (kernel) — 2.43 %
3. `srso_alias_return_thunk` (kernel) — 1.82 %
4. `nf_conntrack_in` (kernel) — 1.66 %
5. `__tcp_transmit_skb` (kernel) — 1.42 %

**Top 5 USERSPACE hot symbols — asio:**
1. `std::__introsort_loop<...>` (same stats post-processing) — 0.64 %
2. `bench_concurrent_churn(...)::$_1::operator()(int) [.resume]` — 0.21 %
3. `boost::asio::detail::scheduler::do_run_one(...)` — 0.16 %
4. `boost::asio::detail::io_uring_service::io_queue::perform_io(int)` — 0.14 %
5. `boost::asio::detail::io_uring_service::run(long, op_queue<...>&)` — 0.12 %

**Corosio-only userspace symbols in the top 25** (with one-sentence description,
file:line where load-bearing):
- `io_uring_tcp_socket::set_option(int,int,void const*,unsigned long)` — 0.20 %
  — Path used by `configure_churn_socket` to apply send_buffer / recv_buffer /
  linger. Both libraries do this (asio's `set_option<integer<1,8>>` totals
  0.08+0.07+0.05 = 0.20 % across three template instantiations), so the
  per-side cost is comparable; the *number of calls* differs because corosio
  hits this from a slightly different cycle structure
  (`include/boost/corosio/native/detail/io_uring/io_uring_types.hpp`).
- `std::__shared_ptr<io_uring_tcp_socket,...>::__shared_ptr(...)` (and the
  matching `io_uring_tcp_service::destroy` 0.09 % +
  `_Hashtable<...>::_M_erase` 0.07 %) — corosio allocates each accepted socket
  in a `shared_ptr` and tracks it in an unordered_map for the lifetime of the
  service; asio uses an intrusive object pool. This is a corosio-specific
  per-accept cost, totaling ~0.34 % across the three symbols, with no
  asio analogue in the top 25.
- `io_uring_multishot_acceptor_base<...>::dispatch_or_queue(...)` — 0.07 %
  — Per-accept dispatch through the multishot-accept state machine
  (`include/boost/corosio/native/detail/io_uring/io_uring_multishot_acceptor.hpp`).
  No corresponding asio symbol because asio uses one-shot accepts that
  re-arm via the generic io_uring_service path (already counted above).
- `io_uring_scheduler::cancel_and_flush(int)` — 0.08 % — Called on socket
  close to drain any in-flight ops for the fd. asio's equivalent
  (`do_cancel_ops`) shows 0.05 %.
- `boost::capy::execution_context::find_impl(std::type_index)` — 0.08 %
  — Service lookup via type_index on every awaitable construction (one map
  probe per `co_await`). asio's `service_registry::do_use_service` is the
  analogous symbol at 0.08 % — comparable.

The userspace deltas are tiny in absolute terms (sub-1 % each); the headline
delta lives in the kernel and in the syscall mix, not in the corosio
acceptor C++.

**Counter deltas (from `ACC_stat.txt`, 5 s runs):**
- IPC: corosio **0.734**, asio **0.846** (delta -13.2 %)
- Branch-miss rate: corosio **10.72 %**, asio **9.92 %** (delta +0.80 pp)
- Cache-miss rate: corosio **1.72 %** of refs, asio **1.35 %** (delta +0.37 pp)
- Context switches: corosio 923, asio 898 (negligible)
- Per-op cost: corosio **150,664 instr / 205,128 cyc / op**;
  asio **131,374 instr / 155,377 cyc / op**
  — corosio executes **+14.7 % more instructions per op** and burns
  **+32.0 % more cycles per op**, i.e. instruction overhead alone does not
  explain the cycle gap; the lower IPC accounts for the rest.

The user/sys split is striking: corosio spent **0.693 s user / 3.479 s sys**
in the 5 s window, asio **0.405 s user / 3.443 s sys**. System time is nearly
identical — both libraries pay essentially the same kernel cost — but corosio
spends **1.71x** the user time, consistent with the per-op instruction delta
above.

**Syscall mix (from `ACC_strace.txt`, 3 s runs):**

Order-of-magnitude differences. corosio issues 12 distinct net-relevant
syscalls per loop, asio issues 4.

| syscall          | corosio calls | asio calls    | notes                                    |
| ---------------- | ------------- | ------------- | ---------------------------------------- |
| `io_uring_enter` | 54,312        | 24,354        | corosio submits **2.23x** as many enters |
| `accept4`        | 13,576        | 0             | corosio falls back to syscall accept     |
| `read`           | 27,149        | 0             |                                          |
| `readv`          | 13,570        | 0             | speculative readv from io_uring_types    |
| `sendmsg`        | 13,570        | 0             | speculative writev / sendmsg fast path   |
| `write`          | 27,165        | 0             |                                          |
| `getsockname`    | 27,146        | 0             | per-accept local-endpoint lookup         |
| `close`          | 27,171        | 64,946        | asio: 2x because two sides closed sync   |
| `setsockopt`     | 40,736        | 97,411        | both ~3x per cycle (configure_churn)     |
| `socket`         | 13,584        | 32,473        | asio creates more client sockets per cycle|
| `futex`          | 13            | 4             |                                          |

Total syscalls: **258,141 (corosio)** vs **219,338 (asio)** — corosio issues
+17.7 % more syscalls overall. But the more interesting ratio is
syscalls-per-accept:

- corosio: 258,141 / 13,576 accepts = **19.0 syscalls per accepted connection**
- asio:    219,338 / (24,354 enters serving accepts+I/O) — asio has zero
  `accept4` calls, so accepts ride entirely on `IORING_OP_ACCEPT` inside
  `io_uring_enter`. Using the socket count as a proxy
  (asio creates 32,473 client-side sockets, i.e. 32,473 cycles), asio runs
  at **6.75 syscalls per cycle**.

**The accept-path-specific finding:** corosio's "multishot acceptor" fires
an **unconditional speculative `accept4(2)` at the head of every
`dispatch_or_queue()` call** before consulting its buffered-CQE queue. See
`include/boost/corosio/native/detail/io_uring/io_uring_multishot_acceptor.hpp:236`:

```cpp
void dispatch_or_queue(...) {
    int accepted_fd = ::accept4(fd_, ..., SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (accepted_fd >= 0) { /* post completion synthetically */ return; }
    if (errno != EAGAIN && errno != EWOULDBLOCK) { /* error path */ return; }
    // EAGAIN: fall through to ready_fds_ (populated by multishot CQE handler)
}
```

The strace evidence shows this syscall **fails every single time** on this
bench: 13,576 `accept4` calls, **13,576 errors** (EAGAIN). The reason is
that the multishot path is *faster*, not slower: by the time
`dispatch_or_queue()` runs in user code, the multishot CQE handler
(`on_accept_cqe`) has already drained the kernel's listen queue and parked
the new fd in `ready_fds_`. The speculative `accept4()` then finds the
listen queue empty and returns EAGAIN; the code falls through to the
`ready_fds_` path and consumes the buffered fd.

Net effect: **one wasted syscall per accept** that asio does not pay. The
multishot SQE is NOT dead code — it's carrying all the actual accept
traffic via `ready_fds_`. The speculative `accept4()` is the dead code:
intended as a fast path to avoid io_uring round-trip latency, it
consistently loses the race against the multishot and pays a pure-overhead
syscall on every invocation.

This is the strongest single-source explanation of the 24 % throughput gap.

`epoll_wait` is absent from both sides — neither library falls back to epoll
on this bench. No "extra" syscall appears in corosio that has no asio
counterpart for *legitimate I/O reasons* — it's all the speculative-readv /
speculative-sendmsg fast paths from FAN re-appearing here on the
post-accept read/write that the bench performs per cycle.

**Comparison to IOCTX and FAN:**

The IOCTX scheduler-cost signature (`timer_service::process_expired`,
`submit_and_get_events`) has **stayed quiet** here, as in FAN — both at
~0.05 % or below. ACC does real I/O per `do_one`.

FAN's per-socket-op symbols (`write_some`, `read_some`,
`native_write_awaitable::await_suspend`, `buffer_param::copy_impl`) **do
appear** in ACC, but at much lower percentages (0.06 %, 0.06 %, 0.05 %, not
visible respectively) because each ACC cycle does only one tiny read and
one tiny write, dwarfed by the per-cycle accept + connect + setsockopt x3
+ close work.

Symbols HOT in corosio's ACC profile that were NOT hot in IOCTX or FAN:
1. **`io_uring_tcp_socket::set_option`** (0.20 %) — ACC-specific because the
   bench calls `configure_churn_socket` on every accepted client (3 setsockopts
   per accept). Did not appear in FAN.
2. **`std::__shared_ptr<io_uring_tcp_socket,...>::__shared_ptr` ctor +
   `io_uring_tcp_service::destroy` + `_Hashtable::_M_erase`** combined ~0.34 %
   — per-accept socket lifecycle (allocate-into-shared_ptr, register in
   service map, erase from service map on destroy). This whole shape was
   absent from IOCTX (no I/O) and FAN (long-lived sockets, register-once)
   but is per-cycle work in ACC.

**Working hypothesis:**

The 24 % loss on `accept_churn:concurrent/4` is dominated by **wasted
syscall overhead from the speculative `accept4()` fast path in
`io_uring_multishot_acceptor.hpp:236` that always loses the race against
the multishot CQE handler on this bench**. Specifically:

1. The multishot SQE consumes new connections from the kernel's listen
   queue and parks them in `ready_fds_` via `on_accept_cqe`. When user
   code eventually calls `accept()`, the listen queue is already empty.
2. `dispatch_or_queue()` then unconditionally calls `::accept4()`, which
   returns EAGAIN (13,576 / 13,576 = 100 % failure rate on this run).
   The code falls through to `ready_fds_` and successfully consumes the
   buffered fd.
3. The net effect is one wasted EAGAIN syscall per accept that asio's
   io_uring acceptor does not pay. Combined with the speculative-read /
   speculative-write per-cycle syscalls also confirmed in FAN, corosio
   averages 19.0 syscalls per cycle vs asio's 6.75.
4. The per-accept shared_ptr / service-map bookkeeping
   (`__shared_ptr<io_uring_tcp_socket>` ctor + `_M_erase` + `destroy`)
   adds ~0.3 % of userspace samples that asio's intrusive-pool design
   avoids — a secondary but real cost.
5. The lower IPC (0.73 vs 0.85) is consistent with branchy syscall
   front-ends and dispatch code being hot rather than tight I/O loops.

**Recommended experiment** (highest leverage): either remove the
speculative `accept4()` entirely (the multishot path already carries the
work), or gate it behind `ready_fds_.empty()` so it only fires when the
buffered queue actually IS empty (which on this bench is never).
Hypothesis only — confirm by (a) building a variant with the speculative
removed/gated and re-running ACC, expecting the 19 syscalls/cycle ratio to
drop toward asio's 6.75, or (b) `strace -e accept4 -k` to verify all
13,576 calls trace to `dispatch_or_queue` (not e.g. a test-harness path).

A subtlety worth checking before deleting: on workloads where user-level
accepts arrive faster than multishot CQEs (unsaturated server), the
speculative path may still be load-bearing. The `ready_fds_.empty()` gate
preserves that case while killing the wasted-syscall cost on saturated
workloads.

**Important context (carried from IOCTX):**
High cost in `timer_service::process_expired()` (none observed here) would
be correctness overhead from commit 2c73112d; optimization is to skip when
`timer_service::empty()`. Not relevant to the ACC bottleneck.

---

**ACC artifacts in this directory:**
- `ACC_corosio.{data,folded,svg}` - corosio profile (per-thread mode)
- `ACC_asio.{data,folded,svg}` - asio profile (per-thread mode)
- `ACC_diff.svg` - differential flamegraph (red = corosio hotter)
- `ACC_stat.txt` - perf stat microarchitectural counters
- `ACC_top_symbols.txt` - flat top symbols (overall, kernel-dominated)
- `ACC_top_user_symbols.txt` - userspace-only top symbols (DSO filter on corosio_bench)
- `ACC_strace.txt` - syscall counts and time
- `ACC_{corosio,asio}_stdout.txt` - captured benchmark stdout

## HTTP

**Profile command (10 s record, 0.5 s warmup):**

```
taskset -c 0-7,16-23 perf record -F 999 -g --call-graph fp \
    --per-thread -m 1 -o HTTP_<side>.data -- \
    corosio_bench --library <side> [--backend io_uring] \
      --category http_server --bench concurrent/16 \
      --duration 10 --warmup 0.5
```

CCX pin: `0-7,16-23`. perf record form: **`--per-thread -m 1`** — the default
`-m 32` aggregate ring exhausted the 8 MB memlock quota on the corosio run
(asio's io_uring registrations + perf mmap = `Cannot allocate memory`).
Per-thread fallback succeeded for both sides.

Bench shape: 16 pre-paired client/server socket coroutines on a single
`io_context`, each doing `write(HTTP request) → read_until("\r\n\r\n")` on
the client side and the mirror on the server. **No accept loop** — sockets
are pre-paired via `make_socket_pair`, so the ACC speculative-accept4 trap
is structurally absent here.

### Throughput

| Side    | Ops (10 s) | Throughput   | Mean latency |
| ------- | ---------- | ------------ | ------------ |
| corosio | 1,026,712  | 102.67 Kop/s | 155.8 µs     |
| asio    | 1,451,600  | 145.16 Kop/s | 110.2 µs     |

**Loss vs asio: 29.3 %** (slightly under the task's 32 % estimate; the
shape is the same).

### Top 5 overall symbols (flat, kernel-dominated)

**corosio**

| %    | Symbol                       | Object        |
| ---- | ---------------------------- | ------------- |
| 7.05 | entry_SYSRETQ_unsafe_stack   | kernel        |
| 2.27 | tcp_ack                      | kernel        |
| 1.80 | tcp_sendmsg_locked           | kernel        |
| 1.79 | srso_alias_safe_ret          | kernel        |
| 1.72 | __tcp_transmit_skb           | kernel        |

**asio**

| %    | Symbol                       | Object        |
| ---- | ---------------------------- | ------------- |
| 3.66 | tcp_ack                      | kernel        |
| 2.60 | __tcp_transmit_skb           | kernel        |
| 2.37 | tcp_sendmsg_locked           | kernel        |
| 2.27 | srso_alias_safe_ret          | kernel        |
| 1.97 | srso_alias_return_thunk      | kernel        |

Key contrast at the top: corosio's **#1 is `entry_SYSRETQ_unsafe_stack` at
7.05 %** (the userspace-to-kernel-and-back ret path) — almost 2× the next
kernel symbol. asio has no such standout: its hottest symbol is `tcp_ack`
at 3.66 %. `entry_SYSRETQ_unsafe_stack` accumulates per-syscall fixed
cost — its prominence is a smoking gun for "corosio does more syscalls per
op." The `process_completions → write_some → write` call-graph
contribution (1.81 % of the 7.05 % SYSRETQ slice) traces to the
speculative-sendmsg fast path on the response-write step.

### Top 5 USERSPACE symbols (`--dsos corosio_bench`)

**corosio**

| %    | Symbol                                                                 |
| ---- | ---------------------------------------------------------------------- |
| 1.44 | std::__introsort_loop (latency-percentile post-process — bench harness)|
| 0.49 | io_uring_scheduler::do_one                                             |
| 0.49 | capy::read_until_match_impl::resume                                    |
| 0.37 | io_uring_tcp_socket::read_some                                         |
| 0.36 | uring_read_op::do_handler                                              |

**asio**

| %    | Symbol                                                                 |
| ---- | ---------------------------------------------------------------------- |
| 1.71 | asio::detail::read_until_delim_string_op_v2::operator()                |
| 1.63 | std::__introsort_loop (bench harness)                                  |
| 0.34 | asio::detail::io_uring_socket_service_base::async_send                 |
| 0.31 | asio::detail::scheduler::do_run_one                                    |
| 0.27 | asio::detail::io_uring_service::io_queue::perform_io                   |

Userspace footprint is **roughly comparable in absolute %** between the two
implementations — neither library is dominated by C++ wrapper overhead at
this throughput. The throughput gap lives in kernelspace / syscall fixed
cost, not in a fat userspace path.

### Corosio-only userspace symbols (no asio analogue)

| Symbol                                                                 | File:line                                                                     |
| ---------------------------------------------------------------------- | ----------------------------------------------------------------------------- |
| `io_uring_tcp_socket::read_some` (speculative `::readv`)               | `native/detail/io_uring/io_uring_types.hpp:149`                               |
| `io_uring_tcp_socket::write_some` (speculative `::sendmsg`)            | `native/detail/io_uring/io_uring_types.hpp:233`                               |
| `uring_read_op::do_handler`                                            | `native/detail/io_uring/io_uring_socket_ops.hpp:133`                          |
| `native_write_awaitable::await_suspend`                                | `native/native_tcp_socket.hpp` (template trampoline)                          |
| `io_uring_scheduler::process_completions`                              | `native/detail/io_uring/io_uring_scheduler.hpp` (CQE drain)                   |
| `timer_service::process_expired` (0.06 %, NOT hot here)                | `detail/timer_service.hpp`                                                    |

The first two are the **FAN per-socket-op userspace signature** reappearing
here — but at modest absolute % (0.37 / 0.26). The real cost is what they
*cause* in kernelspace (see syscall mix below).

### perf stat counter deltas (5 s run, same args)

|                        | corosio       | asio          | Δ                 |
| ---------------------- | ------------- | ------------- | ----------------- |
| ops                    | 506,656       | 726,272       | corosio −30.2 %   |
| cycles                 | 30.92 B       | 30.97 B       | ≈ equal           |
| instructions           | 25.19 B       | 35.66 B       | corosio −29.4 %   |
| **IPC**                | **0.815**     | **1.152**     | corosio **−29 %** |
| inst / op              | 49,710        | 49,099        | ≈ equal (+1.2 %)  |
| cycles / op            | 61,019        | 42,645        | corosio +43 %     |
| branches               | 5.68 B        | 7.96 B        |                   |
| branch-misses          | 522 M (9.2 %) | 599 M (7.5 %) | corosio +22 % rate|
| cache-references       | 3.74 B        | 2.65 B        | corosio +41 %     |
| cache-misses           | 7.88 M (0.21 %)| 4.09 M (0.15 %)| corosio +93 %    |
| dTLB-load-misses       | 1,911,886     | 396,928       | corosio **+382 %**|
| context-switches       | 145           | 214           | corosio −32 %     |
| cpu-migrations         | 93            | 166           | corosio −44 %     |

**Key reading:** the two sides execute almost identical *cycle* budgets and
almost identical *instructions per op* — corosio simply gets 0.815 IPC
where asio gets 1.152, paying 43 % more cycles per op. Lower IPC is driven
by (a) a higher branch-miss rate (+22 %), (b) higher cache pressure
(cache-refs +41 %, miss rate +40 %), and (c) a striking **4.8× dTLB
load-miss count** — the speculative readv path repeatedly touches per-fd
data the kernel has not warmed, and the SYSRETQ churn flushes TLB locality.

### Syscall mix (3 s strace -c, in-bench overhead inflates timing)

**corosio (39,808 ops in 3 s under strace)**

| syscall         | calls   | errors             | per op                      |
| --------------- | ------- | ------------------ | --------------------------- |
| sendmsg         | 87,520  | 0                  | 2.20 (≈ client+server send) |
| io_uring_enter  | 87,876  | 0                  | 2.21                        |
| **readv**       | **87,552** | **87,552 (100 % EAGAIN)** | **2.20 wasted/op**   |
| futex           | 12      | 0                  | (test harness)              |
| accept4         | 32      | 32 (EAGAIN)        | (one-time setup)            |
| setsockopt      | 160     | 0                  | (one-time setup)            |

**asio (350,576 ops in 3 s under strace)**

| syscall         | calls   | errors | per op |
| --------------- | ------- | ------ | ------ |
| io_uring_enter  | 48,254  | 0      | 0.138  |
| setsockopt      | 160     | 0      | (one-time setup) |
| accept          | 32      | 0      | (one-time setup) |
| (no `readv`, no `sendmsg` — pure io_uring) | | | |

This is the single sharpest finding of the run:

> **corosio fires `::readv()` on every read attempt, and EVERY ONE
> RETURNS EAGAIN.** 87,552 calls, 87,552 errors — a 100 % failure rate,
> identical pathology to the speculative-accept4 trap from ACC, but on
> the read side of every HTTP cycle. Speculative-sendmsg, by contrast,
> succeeds (0 errors / 87,520 calls) because the kernel send buffer is
> empty when the response is written — that path is load-bearing and
> earning its keep. The asymmetry is consistent with the HTTP shape:
> when the server enters `read_until`, the request bytes have not yet
> arrived (we just finished writing the prior response), so the
> speculative `readv` always loses the race against the io_uring CQE
> that will eventually deliver them.

corosio also calls `io_uring_enter` ~16× as often as asio (per op):
2.21/op vs 0.138/op. asio batches CQEs aggressively (one enter per
~7 ops on average); corosio's enter-per-op ratio combined with the
wasted readv gives ~4.4 syscall round-trips per HTTP request cycle vs
asio's 0.14 — a **~31× syscall-per-op ratio**.

### Comparison to IOCTX / FAN / ACC

1. **IOCTX scheduler-fixed-cost signature (`timer_service::process_expired`,
   `submit_and_get_events`)** — **not present.** `process_expired` sits at
   0.06 % userspace. HTTP is dominated by real I/O work, not scheduler
   bookkeeping, as expected on a 100 Kop/s workload.
2. **FAN per-socket-op userspace signature (`read_some`, `write_some`,
   `buffer_param::copy_impl`, speculative `readv`/`sendmsg`)** — **present
   and dominant in the syscall mix** but only modest in userspace samples
   (0.37 / 0.26 / not visible / N/A). The userspace `%` is small because
   each call is short, but the *kernel-side* cost it generates is huge:
   the speculative `::readv` is what produces the 7.05 %
   `entry_SYSRETQ_unsafe_stack` peak.
3. **ACC speculative-accept4 trap** — **structurally absent.** This bench
   uses `make_socket_pair`, no per-cycle accept. The 32 accept4 EAGAIN
   calls visible in strace are one-time setup noise from `socket_pair`
   construction.
4. **NEW symbols hot in HTTP that did NOT appear in IOCTX/FAN/ACC:**
   - **`entry_SYSRETQ_unsafe_stack` at 7.05 %** — kernel return path.
     Was not the #1 in any prior bench because no prior bench combined
     "many syscalls" with "many ops/sec" at this ratio. This is FAN's
     speculative-readv pathology, scaled up by the request-rate of a
     realistic HTTP workload.
   - **`tcp_ack` at 2.27 %, `tcp_sendmsg_locked` at 1.80 %,
     `tcp_recvmsg_locked` at 1.48 %, `nf_conntrack_in/_tcp_packet`
     ~2.1 % combined** — these are the real packet-handling cost.
     They appear in *both* sides; they did not appear in IOCTX (no I/O)
     or FAN (loopback fan-out, smaller per-cycle packets). They are
     part of the floor cost both libraries pay, not a corosio-only
     regression.

So HTTP is **principally the FAN bottleneck (per-socket speculative-readv
pathology), magnified by 16× concurrency on a single context.** It is NOT
a new fourth bottleneck shape — it is the FAN shape at full saturation
plus a per-op CQE-draining overhead (high `io_uring_enter` ratio) that FAN
masked because FAN's per-cycle work was syscall-cheap on the read side.

### Working hypothesis

The 29 % loss on `http_server:concurrent/16` decomposes as follows.
**Confidence high on (1) and (2); medium on (3).**

1. **Speculative `::readv` fails on every HTTP read** (87,552 / 87,552
   EAGAIN, `io_uring_types.hpp:149`). Each request arrives via the
   io_uring CQE path, not the speculative fast path — yet the
   speculative attempt still costs one syscall round-trip per read.
   This is the *same architectural shape* as the speculative-accept4
   trap from ACC: a fast-path syscall that wins on the unsaturated case
   but loses 100 % of the time once the workload is real-traffic
   saturated. Removing/gating it (e.g. only attempt when no read CQE
   is in flight) is the highest-leverage single change.
2. **`io_uring_enter` ratio is ~16× asio's** (2.21/op vs 0.138/op).
   asio amortizes one enter across ~7 completions; corosio enters
   nearly per op. This is `io_uring_scheduler::do_one` /
   `submit_and_get_events` working a smaller batch — likely because
   the inline budget exhausts faster on a per-cycle-fully-completing
   bench, or because corosio submits and waits separately where asio
   submits-and-waits in one entry.
3. **dTLB-load-misses are 4.8× asio's.** This is consistent with the
   speculative-readv path touching cold per-fd state (the `spec_state`,
   the `iovec` stack array, the `shared_from_this()` refcount) on
   every read, where asio's flatter completion path stays in already-
   warm cache lines. Removing the speculative path should also drop
   the TLB-miss count substantially.

These three feed each other. Lower IPC (0.815 vs 1.152) is the *symptom*
— branchy syscall front-ends and dispatch code dominate the per-op cycle
budget. Per-op *instruction* count is essentially identical (49.7 K vs
49.1 K) so the gap is not "we do more work" — it's "we do the same work
less efficiently because we keep the pipeline guessing wrong."

**Recommended experiment** (highest leverage, in order):

a. Build with the speculative `::readv` gated behind "no read SQE in
   flight" (or removed entirely on this bench's pattern) and re-run
   HTTP. Expected: readv error count → 0, `entry_SYSRETQ_unsafe_stack`
   drops from 7 % toward ~3 %, throughput moves from 102 → ~125–135
   Kop/s. Hypothesis only; verify.
b. Trace `io_uring_enter` call-graph (`perf record -e
   raw_syscalls:sys_enter -k`) to confirm whether each enter is
   carrying ~1 SQE or larger batches — if 1 SQE/enter, the batching
   path needs investigation.
c. Build with `spec_state::may_speculate_read()` permanently false on
   the read path and re-run as a control. If throughput matches (a),
   confirms the gate logic is correct; if (b) goes higher, there is
   additional batching cost beyond the speculative trap.

### Important context (carried from IOCTX)

> High cost in `timer_service::process_expired()` would be correctness
> overhead from commit 2c73112d; optimization is to skip when
> `timer_service::empty()`. On HTTP it sits at 0.06 % — not material to
> the bottleneck. Carrying forward only for completeness; not actionable
> for HTTP.

---

**HTTP artifacts in this directory:**
- `HTTP_corosio.{data,folded,svg}` - corosio profile (per-thread mode)
- `HTTP_asio.{data,folded,svg}` - asio profile (per-thread mode)
- `HTTP_diff.svg` - differential flamegraph (red = corosio hotter)
- `HTTP_stat.txt` - perf stat microarchitectural counters
- `HTTP_top_symbols.txt` - flat top symbols (overall, kernel-dominated)
- `HTTP_top_user_symbols.txt` - userspace-only top symbols (DSO filter on corosio_bench)
- `HTTP_strace.txt` - syscall counts and time
- `HTTP_{corosio,asio}_stdout.txt` - captured benchmark stdout

## TAIL (socket_latency:concurrent/16 — p99 investigation)

Profile of corosio (io_uring) only, focused on the p99 tail. Prior W/T/L report:
p50 is +1284 % vs asio (corosio is much faster typical case) but p99 is −77 % (much
worse worst case). CPU profiling alone cannot explain a tail issue, so this
section uses sched event tracing (`sched_switch`, `sched_wakeup`) to find the
worst wakeup-to-run gaps.

CCX-pinned (`taskset -c 0-7,16-23`). Recording:
- CPU profile: `perf record -F 999 -g --call-graph fp --per-thread -m 1` (10 s) —
  the `-m 32` mmap size exhausted the user's 8 MiB memlock allowance and caused
  the bench's `io_uring_queue_init_params` to fail (`Cannot allocate memory`).
  Per-thread mode with `-m 1` succeeded.
- Sched events: a workaround was needed (see below). The bench was launched in
  the background first to claim its full memlock for io_uring; then
  `perf record -e sched:sched_switch,sched:sched_wakeup -a -m 1` attached system-wide
  for 5 s. However, **`perf script`, `perf report`, and `perf sched latency` all
  fail with "incompatible file format" on this system (perf 6.19.11) for any
  tracepoint capture**, regardless of record flags. Tested workaround: a fresh
  `perf trace -e sched:sched_switch,sched:sched_wakeup -a -T` capture run during
  a second bench instance produced a text dump (93 893 lines) that the analysis
  script could parse. The binary `.data` file is saved but unreadable; the text
  file is the source of truth.

**Throughput observed during profiling:**
- corosio: 138.71 Kops/s (10 s run), 140.04 Kops/s (15 s parallel-trace run)
- p50 latency: 7 073 ns (CPU run) / 7 053 ns (trace run)
- p99 latency: 490 424 ns (CPU run) / 485 033 ns (trace run)
- p99.9 latency: 596 865 ns (CPU run) / 578 813 ns (trace run)
- max latency: 4 154 µs (CPU run) / 7 597 µs (trace run)
- Ratio to asio: corosio p50 is ~17× faster than asio (118 075 ns); corosio p99
  is ~3.7× *slower* than asio (130 799 ns). This confirms the W/T/L direction
  (sign matches; magnitude within run-to-run noise).

**Top 5 hot symbols — corosio CPU profile (context):**

Userspace+kernel flat self time (no callchain accumulation):

| % | symbol | DSO |
|---|---|---|
| 6.96 | `entry_SYSRETQ_unsafe_stack` | kernel |
| 3.09 | `tcp_ack` | kernel |
| 2.61 | `__tcp_transmit_skb` | kernel |
| 2.17 | `tcp_sendmsg_locked` | kernel |
| 1.99 | `std::__introsort_loop` (bench's latency vector sort) | corosio_bench |

The userspace corosio symbols are deep in the noise (`io_uring_tcp_socket::write_some`
0.77 %, `read_some` 0.52 %, `io_uring_scheduler::do_one` 0.06 %). The CPU profile
looks like a textbook TCP loopback benchmark — kernel networking dominates,
userspace is fine. No "spec-readv at 7 %" shape from the HTTP investigation
appears here, because `concurrent/16` doesn't run the HTTP handler and the
single-packet ping-pong pattern has no speculative-readv opportunity.

**Bench-thread sched analysis from `TAIL_corosio_topgaps.txt`:**

The bench is single-threaded (one comm `corosio_bench`, tid 993177). For this
5-second run, it never voluntarily blocks:

- `sched_wakeup` events targeting bench: **0**
- `sched_switch` events with `prev_pid=bench` (bench going OFF-CPU): 207
- All 207 transitions are `prev_state=256` (TASK_RUNNING — CFS preemption
  while still runnable, not voluntary sleep) or runqueue migration

> **Why doesn't the bench ever block?** corosio's io_uring scheduler does
> have a blocking primitive — `do_one(-1)` calls
> `io_uring_wait_cqe_timeout(&ring_, &cqe, NULL)` at
> `io_uring_scheduler.hpp:970` — but that branch is only entered when
> `completed_ops_.empty()`. Under continuous I/O at concurrent/16, the
> dispatch loop never sees an empty `completed_ops_`: each speculative
> read/write/op-push keeps the queue non-empty across iterations. This is
> the same architectural property that caused the timer-fairness deadlock
> fixed in commit 2c73112d (see the IOCTX section's blockquote on `do_one`).
> Asio's io_uring backend takes the equivalent blocking path more often —
> it doesn't pre-populate a sync-completion queue from speculative syscalls
> — which is the structural reason its p50 is higher (~118 µs vs corosio's
> ~7 µs) and its CPU samples show it as a "sleeping" workload to perf.
>
> So "the bench never sleeps" here is a *consequence* of the same
> always-non-empty-`completed_ops_` pattern seen in HTTP, not evidence that
> corosio is fundamentally a busy-poll architecture. It is functionally
> busy-looping *in this regime* only.

Top 5 worst preemption gaps (bench is OFF the CPU but still runnable):

| off-CPU (µs) | preempted by | resume_ts (µs since epoch) |
|--------------|--------------|---------------------------|
| 256.0 | migration/4 | 3826876818977 |
| 20.0 | migration/4 | 3826878287674 |
| 20.0 | kworker/6:1 | 3826876593729 |
| 19.0 | kworker/2:0 | 3826878640668 |
| 19.0 | migration/4 | 3826877078942 |

Distribution of bench off-CPU periods (n=206): p50=6 µs, p90=10 µs, p99=20 µs,
max=256 µs.

**perf sched latency summary:**

`perf sched latency` failed with "incompatible file format" on this system
(see `TAIL_corosio_sched_latency.txt`). Same root cause as `perf script`
failing. Equivalent information is in the topgaps text and bench off-CPU
distribution above.

**Working theory for the p99 outliers:**

**Scheduler preemption is NOT the dominant cause of the p99 tail.** The bench
thread spends a total of <2 ms off the CPU across the entire 5-second sample
(206 events × ~6 µs mean ≈ 1.3 ms), and its longest single off-CPU period
(256 µs) is roughly half of the reported p99 (485–490 µs) and a third of the
reported p99.9 (579–597 µs). One 256 µs preemption could explain at most a
single tail observation, not the ~14 000 ops per second that fall above p99.

If the bench never sleeps and is rarely preempted, where does ~480 µs of tail
latency come from on every 100th operation? Hypotheses, in rough order of
plausibility:

- **In-process queueing under 16-way concurrency** (most likely). The bench
  runs 16 ping-pong pairs on a single thread serviced by one io_uring ring.
  When 16 CQEs complete in the same batch, the scheduler dispatches them in
  some order — the last coroutine to resume sees the latency of "wait for 15
  other coroutines' send+recv steps to finish on this thread before I can run".
  At ~7 µs per typical op and 16 in flight, the worst-case in-process queueing
  delay is naturally ~16 × 7 µs ≈ 112 µs, but the actual p99 of 485 µs is ~4×
  that — suggesting a CQE-batch processing pattern that locally amplifies the
  fan-out. *Hypothesis only — would need confirmation by measuring per-op
  "queued behind N other coroutines" depth, or by re-running at concurrent/4
  and concurrent/64 to see the p99 ratio scale linearly with N.*
- **CQE-batch buildup spikes** — `do_one` may drain a long CQE list while
  holding the dispatch path, leading to processing-time clumps. The CPU
  profile attributes only 0.06 % to `do_one`, so this is dispatch ordering,
  not raw cycles. *Hypothesis only — would need confirmation by tracing CQE
  count per `io_uring_enter` return.*
- **TCP loopback retransmit / Nagle / delayed ACK pause** — the kernel
  networking stack accounts for ~80 % of CPU samples. A `tcp_schedule_loss_probe`
  symbol shows up at 0.75 %; if the kernel ever decides to defer-send under
  contention, that's a ~200–500 µs window. *Hypothesis only — would need
  confirmation by `tcp_v4_rcv` callchain inspection or `ss -ti` during the
  bench to see retransmits.*
- **Allocator pauses** — the bench's own latency-histogram sort
  (`std::__introsort_loop` at 1.99 %) is end-of-run noise. There is no
  free/alloc on the hot per-op path visible in the profile. Considered unlikely.
- **Kernel scheduler preemption** — directly contradicted by the off-CPU
  data; max 256 µs and only one event of that magnitude. Considered ruled out.

The most-likely single cause is **in-thread coroutine dispatch ordering under
N=16 concurrency**: at the steady-state ops/sec (≈140 K) the typical op takes
~7 µs but the worst-position-in-the-fan-out op gets queued behind ~60 µs of
other coroutines' work + ring submission + kernel TCP send + ring completion.
Notably, asio's p99 (131 µs) is *also* much higher than its p50 (118 µs) — the
absolute p99–p50 gap is similar in microseconds (~13 µs for asio, ~480 µs for
corosio). The p50 ratio looks alarming because asio's typical op carries
io_uring_wait_cqe_timeout block-and-wake overhead (~120 µs per op) while
corosio's typical op skips that path entirely (because `completed_ops_` is
never empty under continuous I/O — see the blockquote above). Asio "blocks
until I have work" on every op, amortizing the queueing delay across all
ops; corosio dispatches directly out of `completed_ops_` and leaves typical
ops at ~7 µs but exposes the queueing delay raw on the worst-positioned op
of each CQE batch.

*Hypothesis — would need confirmation by the concurrent/N scaling experiment
below.* If the p99 grows roughly linearly with N, the cause is in-thread
fan-out queueing. If it stays flat or sub-linear, the cause is something
else (single-event timing artifacts, kernel TCP, etc.).

**Comparison to IOCTX/FAN/ACC/HTTP:**

- The IOCTX/FAN findings on `entry_SYSRETQ_unsafe_stack` (7 %) and kernel TCP
  dominance reproduce here — same kernel-stack pattern.
- HTTP's "speculative readv fails 100 %" shape does **not** appear here. This
  bench is a 16-byte ping-pong; the first speculative readv after a write would
  always race with the kernel and there's no opportunity for it to succeed —
  but the relative cost (the symbol `[k] do_readv` 0.57 %, `[k] vfs_readv`
  0.95 %, libc `readv` 0.19 %) is far smaller than HTTP's. Total `readv`-path
  cost is ~2 % vs HTTP's ~7 % — meaningful but not headline.
- ACC's "speculative accept4" shape is absent (no accept loop on this bench).
- This is a **new tail-latency shape** — IOCTX/FAN/ACC/HTTP all explained
  *throughput / median* gaps via CPU-time hotspots; this section explains
  the p99 tail via *coroutine dispatch ordering*, an effect that's invisible
  to CPU profiling and to the W/T/L throughput report.

**Recommended next investigation steps:**

In order of expected information value:

1. Re-run `socket_latency` at `concurrent/1`, `concurrent/4`, `concurrent/16`,
   `concurrent/64` and plot p99 vs N. If p99 ≈ N × p50, the tail is fan-out
   queueing and the fix is to either spread connections over multiple
   io_contexts or to reorder CQE dispatch (e.g., LIFO).
2. Add an in-bench measurement of "how many CQEs were ready in the batch
   that contained my completion" — directly counts queueing depth.
3. Run `socket_latency:pingpong/64` (one connection, deeper pipeline) — if
   p99 stays close to p50 there, the tail is concurrency-induced, confirming
   (1).
4. Try `iouring_setup_attach_wq` or running 16 pairs across 2 io_contexts —
   does halving the fan-out halve the p99?
5. Capture `bpftrace -e 'kprobe:io_uring_enter { @[args.nr] = count(); }'`
   to see CQE batch sizes; large batches → confirms the (CQE-buildup) sub-hypothesis.

**No code change is recommended at this point.** The p99 gap is real but the
root cause needs the experiments above before any fix should be designed.
A "fix" that lowers p99 at the cost of p50 would lose the +1284 % p50 win.

---

**TAIL artifacts in this directory:**
- `TAIL_corosio_cpu.{data,folded,svg}` — corosio CPU profile (per-thread mode)
- `TAIL_corosio_cpu_stdout.txt` — bench output (throughput, latency percentiles)
- `TAIL_corosio_sched.data` — sched-event binary capture (unreadable by perf
  script on this system; kept for completeness / future perf version)
- `TAIL_corosio_sched.txt` — `perf trace` text dump of sched events (93 893 lines)
- `TAIL_corosio_sched_stdout.txt` — bench stdout during sched capture
- `TAIL_corosio_topgaps.txt` — top wakeup-to-switch gaps and bench off-CPU
  distribution (Python-processed from the text dump)
- `TAIL_corosio_sched_latency.txt` — documentation of the `perf sched latency`
  failure on this perf version
