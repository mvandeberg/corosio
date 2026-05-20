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
`dispatch_mutex_` acquisitions. Because `poll()` calls `do_one(0)` once per
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
