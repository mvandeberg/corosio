# io_uring Backend Profiling Report — 2026-05-20

Final consolidated report for the io_uring perf-tuning investigation. Source
data: `findings.md` (per-bench sections IOCTX / FAN / ACC / HTTP / TAIL). Raw
artifacts in this directory.

## Methodology

- Compiler: clang++ (RelWithDebInfo, `-O2 -fno-omit-frame-pointer -g`, LTO enabled).
- CPU: AMD Ryzen 9 7950X, pinned to CCX0 via `taskset -c 0-7,16-23` (4 physical
  cores + SMT siblings).
- Governor: performance; `kernel.perf_event_paranoid = -1`.
- Benchmark binary: `build_profile/perf/bench/corosio_bench` (drives both
  corosio and asio sides via `--library`).
- CPU profiles: `perf record -F 999 -g --call-graph fp` for 10 s after a
  0.5 s warmup.
- Counters: `perf stat` for 5 s.
- Syscalls: `strace -c` for 3 s (use ratios within a run; cross-run absolute
  counts are inflated by ptrace and not comparable).
- Tail: `perf record -e sched:sched_switch,sched:sched_wakeup` for 5 s
  (post-processed via `perf trace` text dump — `perf script` failed on
  this perf version, see TAIL section in findings.md).
- Known caveats: the 8 MB `RLIMIT_MEMLOCK` quota forced `-m 32` / `--per-thread
  -m 1` mmap fallback on FAN / ACC / HTTP / TAIL runs because
  `io_uring_queue_init` competes with perf's mmap buffers. Sample counts
  (~10 K per side) and per-thread vs per-CPU caveats are disclosed in each
  `findings.md` section.

## Headline findings

The five benchmarks reveal **three distinct bottleneck shapes** for the
io_uring backend, plus one tail-latency artifact:

1. **Speculative non-blocking syscall that always loses the race against
   the io_uring CQE handler on saturated workloads.** This pattern accounts
   for the headline losses in two of the five benches (ACC -24 %, HTTP
   -29 %) and contributes to a third (FAN -29 %). Same shape, three call
   sites. *This is the single highest-leverage tuning target.*
2. **Per-`do_one` fixed costs that dominate when there is no I/O to
   amortize them over** (IOCTX -34 %). Two costs: an unconditional
   `process_expired()` (timer drain — ~25 pp of cycles) and an
   unconditional `submit_and_get_events()` (~8 pp of cycles). Both are
   needed for correctness on the saturated case; gating them on
   emptiness recovers the no-I/O case without regressing the saturated
   case.
3. **Per-socket-op userspace path is ~6 % wider on instructions but
   ~41 % wider on cycles** (FAN signature — IPC deficit in
   `io_uring_tcp_socket::{read,write}_some`, iovec construction, and the
   awaitable shim). Smaller than (1) but real.
4. **The p99 latency gap on `concurrent/N` is partly comparison
   artifact**: asio blocks in `io_uring_wait_cqe_timeout` per op (its
   p50 IS ~120 µs); corosio never blocks under continuous I/O because
   `completed_ops_` stays non-empty (its p50 is ~7 µs but its p99 is
   exposed unfiltered). Needs the concurrent/N scaling experiment in
   TAIL findings before any fix.

## Per-benchmark observations

### IOCTX — `io_context:single_threaded`

- **Loss vs asio:** -34 % (corosio 2.18 / 2.27 Mops/s vs asio 3.30 /
  3.39 Mops/s on the two variants).
- **Dominant cost:** `timer_service::process_expired()` driven from
  `do_one` (~25 pp of cycles split between `__vdso_clock_gettime` and
  `pthread_mutex_lock`) + an unconditional
  `io_uring_submit_and_get_events` on a benchmark with no I/O (~8 pp).
- **Cost shape:** new. Pure scheduler-fixed-cost; no analogue in the
  other benchmarks because real I/O amortizes these costs to < 1 pp.

### FAN — `fan_out:fork_join/16`

- **Loss vs asio:** -29 % (corosio 6.52 Kops/s vs asio 9.10 Kops/s).
- **Dominant cost:** per-socket-op userspace path. The IOCTX fixed
  costs collapse to < 0.1 % each; the loss moves into
  `io_uring_tcp_socket::{read,write}_some`, `buffer_param::copy_impl`,
  and the speculative `readv` / `writev` paths. IPC 0.82 vs 1.09,
  ~6 % more instructions/op but ~41 % more cycles/op.
- **Cost shape:** new (per-op userspace), but the speculative-syscall
  shape that defines ACC and HTTP is *also* embedded here on every
  read/write — just not yet the headline cost because per-cycle
  fan-out work dominates.

### ACC — `accept_churn:concurrent/4`

- **Loss vs asio:** -24 % (corosio 29.57 Kops/s vs asio 38.91 Kops/s).
- **Dominant cost:** an unconditional speculative `::accept4()` at the
  head of every `dispatch_or_queue()` call that **returns EAGAIN 100 %
  of the time on this bench** (13,576 / 13,576). The multishot CQE
  handler drains the listen queue first via `ready_fds_`; the
  speculative call always loses the race and burns one wasted syscall
  per accept.
- **Cost shape:** first appearance of the speculative-syscall-trap
  pattern as a headline cost. Secondary cost: per-accept shared_ptr +
  unordered_map bookkeeping (~0.3 pp).

### HTTP — `http_server:concurrent/16`

- **Loss vs asio:** -29 % (corosio 102.67 Kops/s vs asio 145.16 Kops/s).
- **Dominant cost:** an unconditional speculative `::readv()` in
  `io_uring_tcp_socket::read_some` that **returns EAGAIN 100 % of the
  time on this bench** (87,552 / 87,552). The asymmetric finding: the
  speculative `::sendmsg()` on the write side succeeds 87,520 / 87,520
  (it is load-bearing); only the read-side is dead weight. corosio
  issues `io_uring_enter` ~16× as often as asio per op (2.21/op vs
  0.138/op) and ~31× the syscalls per op overall.
- **Cost shape:** ACC's pattern, scaled up. Same structural fix.

### TAIL — `socket_latency:concurrent/16` (p99 investigation)

- **W/T/L ratio:** corosio p50 +1284 % vs asio (corosio ~17× faster
  typical case) but p99 -77 % vs asio (corosio ~3.7× slower worst
  case). In absolute µs: corosio p50 7 µs / p99 485 µs; asio p50
  118 µs / p99 131 µs.
- **Sched analysis:** the bench thread never voluntarily blocks under
  continuous I/O. `completed_ops_` is non-empty across iterations, so
  `do_one`'s leader-wait branch (`io_uring_wait_cqe_timeout` at
  `io_uring_scheduler.hpp:970`) never runs. This is the **same
  architectural property that caused the timer-fairness deadlock
  fixed in commit 2c73112d** — re-emerging here as a latency artifact.
- **Dominant tail driver (hypothesis):** in-thread coroutine dispatch
  queueing under N=16 concurrency. Asio's p50 IS its block-and-wake
  overhead; corosio's p50 skips that path entirely but exposes the
  queueing delay raw on the worst-positioned op in each CQE batch.
- **Cost shape:** new — a tail-only effect invisible to CPU profiling
  and to W/T/L throughput. Needs the concurrent/N scaling experiment
  before any code fix.

## Symbols hot across multiple benchmarks

Percentages are flat self-time as reported by `perf report --no-children`
for the corosio side. `—` means not measured / not relevant on that bench.

| Symbol                                            | IOCTX        | FAN        | ACC        | HTTP                | TAIL       | Notes                                                                              |
| ------------------------------------------------- | ------------ | ---------- | ---------- | ------------------- | ---------- | ---------------------------------------------------------------------------------- |
| `entry_SYSRETQ_unsafe_stack` (kernel)             | 25.92 %      | 7.57 %     | 6.92 %     | 7.05 %              | 6.96 %     | Per-syscall return path. Magnitude tracks syscall density.                         |
| `timer_service::process_expired`                  | ~25 % (call-graph attributed to SYSRETQ) | 0.05 %     | < 0.05 %   | 0.06 %              | < 0.1 %    | IOCTX-only headline. Correctness-required since 2c73112d; gate on `empty()`.       |
| `io_uring_submit_and_get_events` (kernel side)    | ~8 %         | 0.05 %     | < 0.05 %   | tracks via `do_one` | < 0.1 %    | IOCTX-only headline. Per-`do_one` syscall with no I/O to drain.                    |
| `io_uring_scheduler::do_one`                      | 2.59 %       | 0.50 %     | 0.13 %     | 0.49 %              | 0.06 %     | Dispatch step. Hot only in IOCTX where it isn't amortized.                         |
| `__vdso_clock_gettime` (standalone)               | 3.37 %       | 0.31 %     | not in top | not in top          | not in top | Reached from `process_expired`.                                                    |
| `pthread_mutex_lock`                              | 2.62 %       | 0.35 %     | not in top | not in top          | not in top | Timer-service mutex.                                                               |
| Speculative `::accept4`                           | —            | —          | **100 % EAGAIN** (13576/13576) | —          | —          | ACC headline. `io_uring_multishot_acceptor.hpp:236`. EAGAIN 100 % on saturated accept loop. |
| Speculative `::readv`                             | —            | embedded   | embedded   | **100 % EAGAIN** (87552/87552) | ~2 % (kernel readv path) | HTTP headline. `io_uring_types.hpp:149`. EAGAIN 100 % on saturated reader.         |
| Speculative `::sendmsg`                           | —            | embedded   | embedded   | **load-bearing** (0 errors / 87520) | embedded   | DO NOT remove. Succeeds on write side because send buffer is empty.                |
| `io_uring_tcp_socket::read_some`                  | —            | 0.38 %     | not in top | 0.37 %              | 0.52 %     | Contains the speculative `::readv` block.                                          |
| `io_uring_tcp_socket::write_some`                 | —            | 0.19 %     | not in top | 0.26 %              | 0.77 %     | Contains the speculative `::sendmsg` block (load-bearing).                         |
| `buffer_param::copy_impl` (iovec construction)    | —            | 0.18+0.05 %| not in top | not in top          | not in top | Per-op userspace cost; FAN-specific framing of the IPC deficit.                    |
| `io_uring_scheduler::process_completions`         | not in top   | 0.16 %     | 0.13 %     | not in top          | not in top | CQE drain; modest cost everywhere.                                                 |
| `__shared_ptr<io_uring_tcp_socket>` ctor          | —            | —          | 0.18 %     | —                   | —          | Per-accept; ACC-specific bookkeeping.                                              |
| Per-cycle TCP kernel (`tcp_ack`, `tcp_sendmsg_locked`, `__tcp_transmit_skb`) | — | 1.5-2.7 % each | 1.0-1.4 % each | 1.7-2.3 % each | 2.1-3.1 % each | Floor cost both libraries pay on real I/O; not corosio-only.                       |

**Cross-cutting reading.** `entry_SYSRETQ_unsafe_stack` is high (7+ %)
on every bench that issues real syscalls and is 25.9 % on IOCTX where
the timer drain forces a `clock_gettime` plus mutex per `do_one`. A
symbol that high in five flat-profiles is a "you do too many syscalls"
signature. Each headline bottleneck below resolves to a specific
contributor to that symbol.

## Prioritized tuning targets

In order of leverage (highest first). For each target: which benches
benefit, the exact source location, the proposed fix shape, and
correctness gates / risks.

### 1. Gate the speculative `::readv` in `io_uring_tcp_socket::read_some`

- **Leverage:** HTTP (-29 %) primary; FAN (-29 %) secondary; TAIL minor.
- **Root cause:** `include/boost/corosio/native/detail/io_uring/io_uring_types.hpp:147-160`
  (the `read_some` body) unconditionally calls
  `::readv(fd_, iovecs, iovec_count)` whenever
  `spec_.may_speculate_read()` returns true. On saturated read
  workloads (HTTP server, TCP echo) the kernel CQE handler always wins
  the race, so the syscall returns EAGAIN 100 % of the time and burns
  one wasted syscall round-trip per read. HTTP measured: 87,552 /
  87,552 EAGAIN.
- **Proposed fix shape:** gate the speculative attempt behind "there
  is no read SQE currently in flight for this fd AND the spec-state
  isn't already cooled down by `on_read_exhausted()`". The existing
  `spec_state` machinery already adapts on failure
  (`spec_.on_read_exhausted()` at line 158) but does not check whether
  a CQE is imminent. A simpler interim experiment: build with
  `may_speculate_read()` permanently false and measure — that
  establishes the upper bound on this fix.
- **Correctness gate:** the speculative `::sendmsg` in `write_some`
  (line 228 onward) IS load-bearing on HTTP (87,520 / 87,520 success).
  **Only the read side is dead weight. Do not symmetrize the change.**
- **Expected impact:** HTTP `entry_SYSRETQ_unsafe_stack` 7.05 % → ~3 %,
  throughput 102 → ~125-135 Kop/s. *Hypothesis only — needs A/B test
  before claiming a fix.*

### 2. Gate or remove the speculative `::accept4` in the multishot acceptor

- **Leverage:** ACC (-24 %).
- **Root cause:**
  `include/boost/corosio/native/detail/io_uring/io_uring_multishot_acceptor.hpp:236`
  — `dispatch_or_queue` unconditionally calls
  `::accept4(fd_, ..., SOCK_NONBLOCK | SOCK_CLOEXEC)` before consulting
  `ready_fds_`. On saturated accept loops, the multishot CQE handler
  drains the listen queue first and parks fds in `ready_fds_`; the
  speculative `accept4` then finds the queue empty and returns EAGAIN.
  13,576 / 13,576 EAGAIN on this bench.
- **Proposed fix shape:** gate behind `ready_fds_.empty()`. On
  unsaturated servers where user-level accepts arrive faster than CQEs,
  the speculative path may still be load-bearing; the `empty()` gate
  preserves that case while removing the wasted-syscall cost on
  saturated workloads. Alternative: remove the speculative path
  entirely — the multishot SQE is doing the real work already — and
  measure whether the unsaturated case regresses.
- **Correctness gate:** the multishot SQE handler (`on_accept_cqe`)
  must remain — it is not the dead code. The speculative `::accept4`
  is the dead code.
- **Expected impact:** ACC throughput 29.57 → ~37-39 Kop/s,
  syscalls/cycle 19.0 → ~6.75 (asio parity). *Hypothesis only — needs
  A/B test before claiming a fix.*

### 3. Gate `timer_service::process_expired()` on `timer_service::empty()` in `do_one`

- **Leverage:** IOCTX (-34 %); essentially zero on FAN / ACC / HTTP / TAIL.
- **Root cause:**
  `include/boost/corosio/native/detail/io_uring/io_uring_scheduler.hpp:863`
  (and `:999`) call `timer_svc_->process_expired()` on every `do_one`,
  which acquires the timer-service mutex
  (`include/boost/corosio/detail/timer_service.hpp:678`) and calls
  `clock_type::now()` (`:679`) even when the heap is empty. On a no-I/O
  microbenchmark this is ~25 pp of cycles.
- **Proposed fix shape:**
  `if (!timer_svc_->empty()) timer_svc_->process_expired();` —
  `empty()` is at `include/boost/corosio/detail/timer_service.hpp:161`,
  atomic and lock-free. This skips the `clock_gettime` + mutex on the
  hot path when no timers are registered, while still draining whenever
  there is real timer work.
- **Critical correctness gate:** the unconditional call at
  `io_uring_scheduler.hpp:863` was added in commit 2c73112d to fix a
  shutdown deadlock. **DO NOT remove `process_expired` from `do_one`
  entirely** — that reintroduces the 2c73112d regression in
  `socket_stress.concurrent_ops.io_uring` and
  `socket_stress.sync_completion.io_uring`. The fix is to *skip when
  the timer service is empty*, not to remove the call.
- **Expected impact:** IOCTX IPC 1.354 → toward asio's 2.316,
  throughput 2.18 → toward 3.30 Mops/s. *Hypothesis only — needs A/B
  test.*

### 4. Gate `io_uring_submit_and_get_events` on `do_one(0)` when no SQE is in flight and no CQE is pending

- **Leverage:** IOCTX (-34 %) secondary, after (3). Possibly minor
  improvement on TAIL p50 stability.
- **Root cause:**
  `include/boost/corosio/native/detail/io_uring/io_uring_scheduler.hpp:852`
  — `::io_uring_submit_and_get_events(&ring_)` runs unconditionally
  inside `do_one(0)`. On a no-I/O bench this is a syscall with no work
  to do — ~8 pp of IOCTX cycles.
- **Proposed fix shape:** track in-flight SQE count + pending-CQE hint;
  if both are zero on `do_one(0)`, skip the submit/get call. The
  inline-budget machinery already tracks completion-side state; SQE
  side may need a small counter.
- **Correctness gate:** under continuous I/O the call must run, or
  CQEs accumulate in the ring without being drained into
  `completed_ops_`. The gate must err on the side of calling. Likely
  harder to gate safely than (3); save until after (3) is validated.
- **Expected impact:** IOCTX residual cycles after (3) closes most of
  the gap. *Hypothesis only — and lower confidence than (3).*

### 5. (Tier 2) FAN per-socket-op userspace path

- **Leverage:** FAN (-29 %) residual after (1) lands; possibly HTTP
  marginal.
- **Root cause:** combined ~1.5 pp of userspace samples across
  `io_uring_tcp_socket::{read_some,write_some}`,
  `buffer_param::copy_impl`, `native_write_awaitable::await_suspend`,
  `uring_read_op::do_handler`. IPC 0.82 vs asio 1.09; branch-miss rate
  9.27 % vs 8.04 %. Cycle cost +41 % per op for only +6 % instructions
  — branch-mispredict-dominated.
- **Proposed fix shape:** profile with `perf record -e branch-misses`
  to attribute mispredicts directly to source lines in
  `io_uring_types.hpp`; the speculative-readv short-read handling and
  the `buffer_param::copy_impl` inner loop are the leading candidates.
  Defer until after (1) measures — (1) removes most of the readv path,
  which may also remove most of the FAN mispredicts.
- *Hypothesis only — would need branch-miss profiling to localize.*

### 6. (Tier 2) TAIL p99 — concurrent/N scaling experiment, NOT a code change

- **Leverage:** p99 latency on `socket_latency:concurrent/N`.
- **Root cause (hypothesis):** in-thread coroutine dispatch ordering
  under N=16 fan-out, exposed by corosio never blocking under
  continuous I/O because `completed_ops_` stays non-empty (same
  architectural property as the 2c73112d deadlock).
- **Proposed action:** run the experiments in TAIL findings §
  "Recommended next investigation steps" — at minimum, plot p99 vs N
  across concurrent/{1, 4, 16, 64}. If p99 grows ~linearly with N, fix
  is CQE-dispatch reordering or spreading connections across multiple
  io_contexts. If p99 is sub-linear, root cause is elsewhere.
- **Do not pursue a code change yet.** A "fix" that lowers p99 at the
  cost of p50 would lose the +1284 % p50 win. Asio's better p99 is
  partly comparison artifact: asio's p50 IS its block-and-wake
  overhead (~120 µs), which smooths the tail at the cost of the
  median.

## Out of scope (do not chase)

- **Multithread benchmarks** (`*multithread/N>=2`): corosio already
  wins by +50 % to +200 %. The W/T/L tally that motivated this
  investigation is concentrated on the single-context cases.
- **`concurrent/N` p99 latency comparison vs asio as a regression to
  fix in code:** partly comparison artifact. Run the scaling
  experiment in (6) first.
- **Bidirectional throughput minor regressions (<10 %):** small; likely
  buffer-sizing details that will fall out of dispatch fixes (1) and
  (4) rather than warranting their own investigation.

## Verification experiments (for the engineer)

Concrete A/B tests to run before and after each proposed fix.

1. **For the speculative-readv gating (target 1):**
   - Run `http_server:concurrent/16` before and after. Expect ~29 %
     throughput gain (from -29 % loss to roughly parity).
   - Verify `strace -c` shows `readv` count drops from ~87 K to ~0 in
     a 3 s window.
   - Verify `entry_SYSRETQ_unsafe_stack` drops from 7.05 % toward
     ~3 % in a 10 s perf record.
2. **For the speculative-accept4 gating (target 2):**
   - Run `accept_churn:concurrent/4` before and after. Expect ~24 %
     throughput gain.
   - Verify `strace -c` shows `accept4` calls drop from ~13.6 K (all
     EAGAIN) to ~0.
3. **For the timer-drain gating (target 3):**
   - Run `io_context:single_threaded` and `single_threaded_lockless`
     before and after. Expect IPC to recover from 1.354 toward asio's
     2.316.
   - **Regression gate:** re-run `socket_stress.concurrent_ops.io_uring`
     and `socket_stress.sync_completion.io_uring` — these were the
     tests broken before 2c73112d. They must still pass.
4. **For all four together:**
   - Run the full bench suite. Verify the asio W/T/L tally improves
     from 94 / 13 / 149 toward parity. The multithread wins should
     remain intact (they are not on the affected code paths).

## Reading order for the perf-tuner

The per-bench `findings.md` sections are the source of truth. This
report is the synthesis. If acting on a recommendation:

1. Read this report's prioritized-target entry for context and the
   correctness gate.
2. Read the corresponding `findings.md` section in full to understand
   the underlying evidence (call-graphs, counter deltas, strace
   tables).
3. Inspect the cited file:line in source — the citations were verified
   against the current tree but the speculative-syscall blocks are
   under active change on this branch.
4. Run the verification experiment under "Verification experiments"
   above before claiming the fix landed.
