# Queue-Aware Inline Budget — Design

## Background

corosio's reactor scheduler has an "inline budget" mechanism that decides, at each
synchronously-completing I/O syscall, whether to:

- **Chain**: directly resume the awaiting coroutine on the current stack (fast path)
- **Post**: enqueue the continuation and return through the scheduler (slow path)

The mechanism is controlled by three runtime options on `io_context_options`:

- `inline_budget_initial` — chain length at startup
- `inline_budget_max` — adaptive ceiling, doubles each time budget is fully consumed
- `unassisted_budget` — clamp applied when no other thread absorbs queued work

Defaults today: `(2, 16, 4)`.

## Problem

Empirical results from the throughput benchmark (evpp) and the corosio benchmark
suite establish two clear regimes:

1. **Workloads with a single coroutine per scheduler thread** (single-conn ping-pong,
   HTTP `single_conn`, `accept_churn:sequential`): chained dispatch wins — there's
   nobody to yield to, and post-then-resume just adds queue-trip overhead.

2. **Workloads with many coroutines per scheduler thread** (`concurrent/16`,
   `multithread/8` w/ many sessions, evpp 16-thread × small-block configs):
   post-everything wins — yielding lets sibling coroutines run, which in turn
   gives the kernel softirq breathing room and allows cross-thread work-stealing.

The wrong choice loses 10-50% throughput, and the optimum flips depending on
*coroutine concurrency*, not OS thread count. Static defaults can't satisfy both.

A measured experiment substituting `(0, 0, 0)` (post-everything) as the default
recovered the multi-thread wins but lost ~25 single-conn benchmarks vs the old
default. The win/tie/loss summary at corosio bench level dropped from 86 wins to
10 wins against asio.

## Hypothesis

The right adaptive signal is **whether anyone else is waiting in the local
scheduler queue**:

- If `private_queue.empty()` → I'm the only coroutine making progress on this
  thread. Yielding accomplishes nothing useful. Chain.
- If `!private_queue.empty()` → other coroutines are waiting for cycles.
  Yielding lets them interleave with the kernel work I'm about to trigger.
  Post.

This signal:

- Is essentially free to read (TLS pointer + one-byte queue-empty check; both
  cache-hot at this point in the hot path).
- Auto-adapts to coroutine concurrency without static knobs or runtime tracking.
- Captures the single-thread-high-fanout case correctly (where thread-count
  heuristics fail).
- Is a strict refinement, not a replacement, of the existing budget logic — the
  budget cap still bounds chain length when chaining is appropriate.

## Design

### Code change (minimum viable)

In `reactor_scheduler::try_consume_inline_budget`
([reactor_scheduler.hpp:459](include/boost/corosio/native/detail/reactor/reactor_scheduler.hpp#L459)):

```cpp
inline bool
reactor_scheduler::try_consume_inline_budget() const noexcept
{
    if (budget_disabled_)
        return false;
    if (auto* ctx = reactor_find_context(this))
    {
        // Chain only when the local queue is empty — i.e., no other
        // coroutine on this thread is waiting for cycles. If anyone is
        // waiting, post so they can interleave.
        if (ctx->inline_budget > 0 && ctx->private_queue.empty())
        {
            --ctx->inline_budget;
            return true;
        }
    }
    return false;
}
```

That's the only required change. `reset_inline_budget` and the
`unassisted_budget`/`inline_budget_max` ramp-up logic remain as-is.

### Default value adjustments

With the queue-aware check in place:

- `inline_budget_initial = 2` — same as today, fine
- `inline_budget_max = 16` — caps how aggressively we chain when genuinely alone;
  same as today, fine
- `unassisted_budget = 4` — **becomes mostly redundant** because the queue check
  already handles the "I'm alone with backlog" case. Recommend keeping at 4 as
  defense-in-depth (it still bounds chain length when the queue check thinks
  we're alone but the chunk-level unassisted detection thinks otherwise).
  Consider removing in a follow-up if benchmarks show no behavioral difference.

### Configuration interaction

The change should be transparent to existing config:

- Users who set `(0, 0, 0)` explicitly still get post-everything (the
  short-circuit via `budget_disabled_` fires first).
- Users who set a non-zero budget get the queue-aware behavior automatically.
- Documentation should explain that `inline_budget_max` is now interpreted as
  "max chain length when no other coroutines are waiting on this thread."

## Implementation steps

1. **Make the code change** in `reactor_scheduler::try_consume_inline_budget`.
   Single file. No header dependencies change.

2. **Update the docstring** on `try_consume_inline_budget` to describe the
   queue-empty condition.

3. **Update `io_context_options::inline_budget_max` docstring** in
   `io_context.hpp` to reflect the new semantics: "max chain length when the
   thread's local queue is empty."

4. **Update the configuration guide** at
   `doc/asciidoc/modules/ROOT/pages/4.guide/4c2.configuration.adoc` to describe
   the queue-aware behavior. The "when to tune" guidance becomes simpler — the
   defaults Just Work for both single-coroutine and multi-coroutine regimes.

5. **Build and run unit tests** to ensure no regressions in correctness.

## Test plan

### Phase 1: Functional correctness

Run the corosio test suite (`corosio_tests`) under the standard build:

```bash
cd build_cmake && cmake --build . --target boost_corosio_tests && ctest
```

Ensure all existing tests pass. The change preserves all observable semantics
(timing, ordering of ops within a coroutine), so this should be a no-op for
correctness.

### Phase 2: Single-thread baseline

Verify the single-conn case is unchanged from default budget. Run the corosio
benchmark suite:

```bash
cd build_bench && cmake --build . --target corosio_bench
./bin/corosio_bench --benchmark_filter="socket_latency:pingpong/.*"
```

Compare `pingpong/1` and `pingpong/64` ops/sec against the develop branch
(default `(2, 16, 4)`). Should be **identical within ±1%**. Any larger
regression indicates the queue check is incorrectly firing for single-coroutine
loops.

### Phase 3: Multi-coroutine single-thread (the key validator)

Run the `concurrent/N` family for the cases head's `(0, 0, 0)` improved over
develop:

```bash
./bin/corosio_bench --benchmark_filter="socket_latency:concurrent/(4|16)"
./bin/corosio_bench --benchmark_filter="local_socket_latency:concurrent/(4|16)"
```

Expected outcome: throughput within 1-3% of head's `(0, 0, 0)` results — the
queue-aware check should pick up that 4 or 16 sibling coroutines are waiting
and post just like 0/0/0 does.

### Phase 4: Multi-thread

Run the multi-thread benchmarks:

```bash
./bin/corosio_bench --benchmark_filter="socket_throughput:multithread/.*"
./bin/corosio_bench --benchmark_filter="http_server:multithread/.*"
```

Expected outcome: throughput close to head's `(0, 0, 0)` results. At
multi-thread, there's almost always pending work in *some* thread's queue, so
the queue-aware check effectively selects post-everything behavior.

### Phase 5: HTTP server (composed-op)

```bash
./bin/corosio_bench --benchmark_filter="http_server:single_conn.*"
./bin/corosio_bench --benchmark_filter="http_server:concurrent/1"
```

Expected outcome: matches develop's `(2, 16, 4)` results for `single_conn`
(only one coroutine, queue empty, chain helps composed `read_until`/`write`).
HTTP server multi-thread should match head's results.

### Phase 6: Full corosio bench win/tie/loss

Run the full benchmark matrix, generate the win/tie/loss report against asio:

```bash
./bin/corosio_bench --benchmark_format=json --benchmark_out=run.json
# Then run the existing analysis script that generated asio-win-loss.md
```

**Acceptance criterion**: total wins ≥ 86 (matches develop) **AND** zero
unique regressions vs develop in single-conn / single-coroutine benchmarks.
Stretch goal: total wins > 86 (gain over develop on multi-coroutine cases that
develop also lost).

### Phase 7: evpp throughput matrix

Run the evpp benchmark matrix (24 configs × 3 impls):

```bash
cd /home/michael/git/evpp/build_clang
bash /tmp/run_matrix_extended.sh > /tmp/matrix_queue_aware.txt
```

Compare to:

- `/tmp/matrix_extended.txt` — corosio at `(0, 1, 0)` (effective post-everything
  via the now-fixed bug)
- `/tmp/matrix_default_budget.txt` — corosio at default `(2, 16, 4)`

**Acceptance criterion**: at every config, corosio with the queue-aware default
performs within noise (say, ±3%) of the better of the two reference runs at
that config. Specifically:

- Single-thread configs (1024-16384 / N / 1): match `(2, 16, 4)`
- 4-thread × small/medium: match the post-everything run
- 16-thread × small/medium: match the post-everything run (these were +42-52%
  over asio_coro)

## Success criteria summary

1. **No correctness regressions**: full test suite passes.

2. **No performance regressions vs develop on single-coroutine workloads**:
   `pingpong/1`, `concurrent/1`, `single_conn`, `accept_churn:sequential` all
   within 1-2% of develop.

3. **Captures multi-coroutine wins**: `concurrent/{4,16}`, `multithread/{4,8}`,
   `http_server:multithread/{4,8}` all within 1-3% of head's
   `(0, 0, 0)` results.

4. **Net wins increase**: corosio bench win/tie/loss table shows ≥ 86 wins
   against asio (matches develop's count) without regressing any of develop's
   wins. Stretch: > 86 wins, gaining on the head-improvement cases that develop
   currently loses.

5. **No measurable hot-path overhead**: per-syscall cost increase from the
   added queue-empty check should be < 5 ns. Verify with
   `perf stat -e cycles:u,instructions:u` on the throughput benchmark at
   1024/100/1; total cycles per byte should be within 1% of the develop
   baseline.

## Risks and open questions

### Risk 1: AF_UNIX regressions may have a separate cause

The corosio bench shows `local_socket_throughput:bidirectional/{1024,4096,16384}`
regressed in head. Some of this regression may be from the
`send`/`recv` single-buffer optimization (`bbc0e4a2`) being a poor fit for
AF_UNIX rather than the budget change.

**Mitigation**: After implementing this change, run the AF_UNIX throughput
benchmarks specifically. If they don't fully recover, investigate whether the
send/recv change should be conditional on socket family (`AF_INET` only) — that
investigation is independent of this design.

### Risk 2: Cross-thread queue contention

`private_queue` is per-thread. The queue-empty check looks only at this thread's
local queue. In multi-thread mode, this thread's queue might be empty while
other threads have heavy backlog. The current behavior (chain when local queue
empty) is arguably correct — chained dispatch on this thread doesn't slow
others down. But it could miss opportunities to share work across threads.

**Mitigation**: Existing `unassisted_budget` clamp acts as defense-in-depth.
If multi-thread + idle local queue + active other threads turns out to be a
common pathology, a follow-up could check the global `completed_ops_` queue
(at the cost of one cache-line touch per syscall completion).

### Risk 3: `unassisted` flag becomes dead code

After this change, the `unassisted` clamp in `reset_inline_budget` may rarely
fire (because the queue-empty check in `try_consume_inline_budget` makes it
moot in most cases). It's still correct, but may be a code smell.

**Mitigation**: leave it for now. Revisit after measurement; if it never
materially affects throughput, simplify in a follow-up.

### Open question: cost of the queue-empty check

`private_queue` is `op_queue` — verify that `empty()` is a single-cycle
operation (typically `head_ == nullptr`, no lock needed since per-thread).
Confirm by reading the `op_queue` implementation. If it requires more (e.g., a
size counter that needs synchronization), reconsider.

### Open question: should chain decisions also consider non-local pending work?

In single_threaded mode, only `private_queue` matters. In multi-thread mode,
work on other threads' private queues isn't visible. The asymmetry is likely
fine for now (corosio's design is "each thread schedules its own queue") but
worth noting in a comment.

## Out of scope

- **io_uring backend**: this design is for the epoll/kqueue/select reactor.
  When io_uring lands, the chain-vs-post decision becomes "submit-with-link
  vs submit-and-yield" and the relevant signal may be different. Cross that
  bridge when we get there.

- **Adaptive `inline_budget_max`**: the existing doubling ramp-up stays as-is.
  A more sophisticated adaptation (e.g., decay-on-idle) is possible but not
  required for this change.

- **Removing `unassisted_budget`**: covered as Risk 3 mitigation; defer.

## Estimated effort

- Implementation: 30 minutes (single-line change + docstring updates).
- Testing (full corosio bench + evpp matrix): 1-2 hours wall-clock with
  multiple reps for tight signal at multi-thread configs.
- Total: half a day, including writing the win/tie/loss diff.
