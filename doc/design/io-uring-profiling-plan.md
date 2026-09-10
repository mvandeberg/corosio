# io_uring Backend Profiling Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Identify the dominant bottlenecks in the corosio io_uring backend that cause it to underperform asio io_uring on single-`io_context`-concurrent workloads, and produce a written report with prioritized perf-tuning targets. Side-task: characterize the p99 tail-latency regression seen at higher concurrencies.

**Architecture:** Linux `perf` for CPU profiles, microarchitectural counters, and syscall stats; `strace -c` for syscall mix; FlameGraphs for visualization; matched corosio-vs-asio runs of representative benchmarks; CCX-pinned (`taskset -c 0-7,16-23`) under `performance` governor; dedicated `build_profile/` configured with `-O2 -fno-omit-frame-pointer -g` for accurate stack unwinding without breaking optimization. All profile artifacts land in `perf_results/<date>/` (gitignored) so runs are reproducible and comparable.

**Tech Stack:** Linux `perf`, `strace`, FlameGraph (Brendan Gregg's perl scripts), `c++filt`, `addr2line`. Optional: `bpftrace`.

---

## Reference data driving the plan

From the 2026-05-20 baseline-only run (`bench_results/io_uring_20260520/`),
corosio loses to asio io_uring on these loss-clusters:

- **Single-io_context dispatch micros:** `io_context:single_threaded` -93%,
  `io_context:interleaved` -92%, `io_context:concurrent/4` -78%.
- **Task spawning:** `fan_out:fork_join/N` -19% to -33% across N=1..64.
- **Single-ctx parallel I/O:** `http_server:concurrent/N` -12% to -32%
  (corosio plateaus at ~100K ops/s, asio reaches ~150K).
- **Accept-loop hot path:** `accept_churn:*` -15% to -38%.
- **Tail latency under contention:** `socket_latency:concurrent/16` p99 -77%
  *while* p50 is +1284%. Same shape for `local_socket_latency`.

Multithread benches (multiple `io_context`s) are already +50% to +200%, so
the scaling story is good — this plan only targets the single-context path.

---

## Target benchmarks (cover the failure modes)

| Tag | Benchmark | What it exposes |
|-----|-----------|-----------------|
| `IOCTX` | `io_context:single_threaded` | Raw scheduler dispatch cost, no I/O |
| `FAN` | `fan_out:fork_join/16` | Coroutine spawn / completion dispatch |
| `ACC` | `accept_churn:concurrent/4` | Accept path + completion delivery |
| `HTTP` | `http_server:concurrent/16` | Realistic single-ctx parallel I/O |
| `TAIL` | `socket_latency:concurrent/16` | p50/p99 split (tail-latency probe) |

For each tag we run **both** the corosio and asio variants under identical
conditions and diff the profiles.

---

## File / artifact layout

All artifacts live under `perf_results/<YYYY-MM-DD>/`:

- `<TAG>_corosio.data`, `<TAG>_asio.data` — raw perf data
- `<TAG>_corosio.folded`, `<TAG>_asio.folded` — collapsed stacks
- `<TAG>_corosio.svg`, `<TAG>_asio.svg` — FlameGraphs
- `<TAG>_diff.svg` — differential FlameGraph (asio vs corosio)
- `<TAG>_stat.txt` — `perf stat` microarch counters
- `<TAG>_strace.txt` — `strace -c` syscall counts
- `findings.md` — running notes (one section per tag)
- `report.md` — final consolidated report

`perf_results/` is added to `.gitignore` (raw data files are large and host-specific).

---

## Task 0: Environment baseline & gitignore

**Files:**
- Modify: `.gitignore`

- [ ] **Step 1: Confirm host invariants are still in place**

```bash
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor   # must be "performance"
cat /proc/sys/kernel/perf_event_paranoid                    # must be ≤ 1 for user CPU profiling
lscpu --extended=CPU,CORE,SOCKET,NODE,CACHE                 # confirm CCX0 = 0-7,16-23
```

If governor is not `performance`, stop and set it (`sudo cpupower frequency-set -g performance`). If `perf_event_paranoid` is 2 or higher, either lower it via `sudo sysctl -w kernel.perf_event_paranoid=1` or accept that kernel-symbol resolution and certain counters will be unavailable.

- [ ] **Step 2: Add perf_results to gitignore**

Append to `.gitignore`:
```
perf_results/
```

- [ ] **Step 3: Commit**

```bash
git add .gitignore
git commit -m "perf: ignore perf_results/ profiling output"
```

---

## Task 1: Install FlameGraph

**Files:**
- Create: `tools/flamegraph/` (git submodule or unpacked clone — see Step 2)

- [ ] **Step 1: Check if FlameGraph is already on PATH or under tools/**

```bash
ls /home/michael/git/boost/libs/corosio/tools/flamegraph/ 2>&1 | head
command -v flamegraph.pl stackcollapse-perf.pl
```

If both scripts exist, skip to Step 4.

- [ ] **Step 2: Clone FlameGraph under tools/**

```bash
cd /home/michael/git/boost/libs/corosio
git clone --depth 1 https://github.com/brendangregg/FlameGraph.git tools/flamegraph
```

`tools/` is already used for ad-hoc scripts and is gitignored at top-level via the existing `tools/` entry — verify with `git check-ignore -v tools/flamegraph` and if it ISN'T ignored, add `tools/flamegraph/` to `.gitignore` explicitly.

- [ ] **Step 3: Make the two scripts executable**

```bash
chmod +x tools/flamegraph/flamegraph.pl tools/flamegraph/stackcollapse-perf.pl tools/flamegraph/difffolded.pl
```

- [ ] **Step 4: Confirm they work**

```bash
tools/flamegraph/flamegraph.pl --help 2>&1 | head -3
tools/flamegraph/stackcollapse-perf.pl --help 2>&1 | head -3
tools/flamegraph/difffolded.pl --help 2>&1 | head -3
```

Each should print usage. No commit needed (artifact is gitignored).

---

## Task 2: Build profiling binary

**Files:**
- Create: `build_profile/` (build dir)

The profile binary must keep optimizations (we're measuring the production hot path) but preserve frame pointers and debug info so `perf record -g --call-graph fp` produces correctly-attributed stacks. `-fno-omit-frame-pointer` adds < 2% overhead but enables `perf`'s cheap frame-pointer unwinder; DWARF unwinding (`--call-graph dwarf`) works without it but is much slower and balloons perf.data.

- [ ] **Step 1: Configure**

```bash
REPO=/home/michael/git/boost/libs/corosio
rm -rf "$REPO/build_profile"
cmake -B "$REPO/build_profile" -S "$REPO" \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g -fno-omit-frame-pointer -DNDEBUG" \
  -DBOOST_COROSIO_BUILD_PERF=ON \
  -DBOOST_COROSIO_BUILD_TESTS=OFF \
  -DBOOST_COROSIO_BUILD_EXAMPLES=OFF
```

Expected: `Building with liburing 2.14 — io_uring backend enabled`, `LTO enabled for benchmarks`.

LTO is fine — clang LTO preserves debug info and frame pointers correctly.

- [ ] **Step 2: Build only `corosio_bench`**

```bash
cmake --build "$REPO/build_profile" --target corosio_bench -j$(nproc)
```

- [ ] **Step 3: Verify the binary has frame pointers and asio io_uring**

```bash
# Frame pointer check (look for "pushq %rbp" prologues)
objdump -d "$REPO/build_profile/perf/bench/corosio_bench" \
    | grep -A1 -m5 "<_ZN5boost7corosio" | grep -c "push.*rbp"

# liburing linked
ldd "$REPO/build_profile/perf/bench/corosio_bench" | grep -i uring
```

Expect: > 0 frame-pointer prologues, and `liburing.so.2` linked.

If asio is NOT picking up io_uring (no `liburing` link), re-check that `perf/bench/CMakeLists.txt` still has the `BOOST_ASIO_HAS_IO_URING` block from the previous benchmarking work.

---

## Task 3: Profile IOCTX (`io_context:single_threaded`)

This is the synthetic-dispatch micro where corosio loses ~93%. It runs a tight loop posting completions on a single io_context. No I/O, no kernel involvement, no contention. **If something here is slow, it's pure scheduler overhead** — the most actionable signal in the suite.

**Note on `--bench` prefix matching:** `--bench single_threaded` matches BOTH `single_threaded` and `single_threaded_lockless` (the filter is prefix-based on the benchmark name). For profiling that's fine — both stress the same dispatch path, just one with the lockless single-threaded mode. If you need to isolate one, pass `--bench single_threaded_lockless` (which uniquely matches the lockless variant) and report findings against that variant explicitly.

**Files:**
- Output: `perf_results/<DATE>/IOCTX_corosio.{data,folded,svg}`
- Output: `perf_results/<DATE>/IOCTX_asio.{data,folded,svg}`
- Output: `perf_results/<DATE>/IOCTX_stat.txt`
- Output: `perf_results/<DATE>/IOCTX_diff.svg`

- [ ] **Step 1: Make the output directory**

```bash
REPO=/home/michael/git/boost/libs/corosio
DATE=$(date +%Y-%m-%d)
PR="$REPO/perf_results/$DATE"
mkdir -p "$PR"
echo "Writing artifacts to $PR"
```

- [ ] **Step 2: Record corosio profile**

Use a longer duration than benchmarking (10s) so the profile has enough samples but does not include warmup. Frequency 999 Hz avoids aliasing with the kernel's 1000 Hz tick.

```bash
BIN="$REPO/build_profile/perf/bench/corosio_bench"
taskset -c 0-7,16-23 perf record -F 999 -g --call-graph fp \
    -o "$PR/IOCTX_corosio.data" -- \
    "$BIN" --library corosio --backend io_uring \
           --enable-microbenchmarks \
           --category io_context --bench single_threaded \
           --duration 10 --warmup 0.5
```

Expected: completes in ~11s. `perf.data` file ~50-200 MB.

- [ ] **Step 3: Record asio coroutine profile**

```bash
taskset -c 0-7,16-23 perf record -F 999 -g --call-graph fp \
    -o "$PR/IOCTX_asio.data" -- \
    "$BIN" --library asio \
           --enable-microbenchmarks \
           --category io_context --bench single_threaded \
           --duration 10 --warmup 0.5
```

Note: `--backend` is a corosio-only flag (asio backend is fixed at compile time). asio will use io_uring because of the `BOOST_ASIO_HAS_IO_URING` define we added to the bench build.

- [ ] **Step 4: Collapse stacks and generate flamegraphs**

```bash
for side in corosio asio; do
    perf script -i "$PR/IOCTX_${side}.data" \
        | tools/flamegraph/stackcollapse-perf.pl \
        > "$PR/IOCTX_${side}.folded"
    tools/flamegraph/flamegraph.pl --title "IOCTX $side" \
        "$PR/IOCTX_${side}.folded" > "$PR/IOCTX_${side}.svg"
done
```

- [ ] **Step 5: Generate differential flamegraph (red = corosio hotter)**

```bash
tools/flamegraph/difffolded.pl \
    "$PR/IOCTX_asio.folded" "$PR/IOCTX_corosio.folded" \
    | tools/flamegraph/flamegraph.pl --negate \
        --title "IOCTX corosio vs asio (red = corosio spends more time)" \
    > "$PR/IOCTX_diff.svg"
```

- [ ] **Step 6: Get microarchitectural counters with `perf stat`**

```bash
for side in corosio asio; do
    BIN_ARGS="--library $side --enable-microbenchmarks --category io_context --bench single_threaded --duration 5 --warmup 0.5"
    [ "$side" = "corosio" ] && BIN_ARGS="$BIN_ARGS --backend io_uring"
    echo "=== $side ===" >> "$PR/IOCTX_stat.txt"
    taskset -c 0-7,16-23 perf stat -e cycles,instructions,branches,branch-misses,cache-references,cache-misses,dTLB-load-misses,LLC-load-misses,context-switches,cpu-migrations \
        "$BIN" $BIN_ARGS 2>> "$PR/IOCTX_stat.txt"
done
```

- [ ] **Step 7: Inspect top symbols (text reports for findings.md)**

```bash
for side in corosio asio; do
    echo "=== top 25 hot symbols: $side ==="
    perf report --no-children -n --stdio -i "$PR/IOCTX_${side}.data" \
        | grep -A 30 "^# Overhead" | head -40
done | tee "$PR/IOCTX_top_symbols.txt"
```

- [ ] **Step 8: Write findings**

Open `$PR/findings.md` (create if missing) and append a `## IOCTX` section with:

1. **Throughput observed during profiling** (read from the benchmark stdout — the binary prints final ops/s).
2. **Top 5 hot symbols in corosio** (function name + % cycles).
3. **Top 5 hot symbols in asio** (same).
4. **Symbols present in corosio but absent in asio's top 25** — these are the corosio-specific cost.
5. **Notable counter deltas from `IOCTX_stat.txt`** — particularly IPC, branch-misprediction rate, L1d/LLC miss rate, context-switch count.
6. **One-sentence hypothesis** of where the cost lives.

Stop here for now — the consolidated report in Task 8 cross-references all five tags' findings. Don't change code yet.

- [ ] **Step 9: Commit findings**

```bash
git add doc/design/io-uring-profiling-plan.md  # if not already committed
git add -f perf_results/$DATE/findings.md       # findings are tracked
git commit -m "perf: IOCTX profile findings"
```

(Note the `-f` — `perf_results/` is gitignored as a whole; we explicitly track only `findings.md` and `report.md`.)

---

## Task 4: Profile FAN (`fan_out:fork_join/16`)

Spawning 16 child coroutines per parent op, joining, repeat. Same `io_context`, no I/O. Stresses the **task-spawn + completion-dispatch chain** without any kernel involvement. If `fork_join/16` looks the same as IOCTX, the bottleneck is purely scheduler-side; if it differs, the coroutine-spawn path adds its own cost.

**Files:**
- Output: `perf_results/<DATE>/FAN_corosio.{data,folded,svg}`
- Output: `perf_results/<DATE>/FAN_asio.{data,folded,svg}`
- Output: `perf_results/<DATE>/FAN_stat.txt`
- Output: `perf_results/<DATE>/FAN_diff.svg`
- Output: `perf_results/<DATE>/findings.md` (append `## FAN` section)

- [ ] **Step 1: Record corosio**

```bash
DATE=$(date +%Y-%m-%d)
PR="/home/michael/git/boost/libs/corosio/perf_results/$DATE"
BIN="/home/michael/git/boost/libs/corosio/build_profile/perf/bench/corosio_bench"
taskset -c 0-7,16-23 perf record -F 999 -g --call-graph fp \
    -o "$PR/FAN_corosio.data" -- \
    "$BIN" --library corosio --backend io_uring \
           --category fan_out --bench fork_join/16 \
           --duration 10 --warmup 0.5
```

- [ ] **Step 2: Record asio**

```bash
taskset -c 0-7,16-23 perf record -F 999 -g --call-graph fp \
    -o "$PR/FAN_asio.data" -- \
    "$BIN" --library asio \
           --category fan_out --bench fork_join/16 \
           --duration 10 --warmup 0.5
```

- [ ] **Step 3: Collapse, generate flamegraphs, diff**

```bash
for side in corosio asio; do
    perf script -i "$PR/FAN_${side}.data" \
        | tools/flamegraph/stackcollapse-perf.pl > "$PR/FAN_${side}.folded"
    tools/flamegraph/flamegraph.pl --title "FAN $side" \
        "$PR/FAN_${side}.folded" > "$PR/FAN_${side}.svg"
done
tools/flamegraph/difffolded.pl "$PR/FAN_asio.folded" "$PR/FAN_corosio.folded" \
    | tools/flamegraph/flamegraph.pl --negate \
        --title "FAN corosio vs asio (red = corosio spends more time)" \
    > "$PR/FAN_diff.svg"
```

- [ ] **Step 4: `perf stat` counters**

```bash
for side in corosio asio; do
    BIN_ARGS="--library $side --category fan_out --bench fork_join/16 --duration 5 --warmup 0.5"
    [ "$side" = "corosio" ] && BIN_ARGS="$BIN_ARGS --backend io_uring"
    echo "=== $side ===" >> "$PR/FAN_stat.txt"
    taskset -c 0-7,16-23 perf stat -e cycles,instructions,branches,branch-misses,cache-references,cache-misses,dTLB-load-misses,LLC-load-misses,context-switches \
        "$BIN" $BIN_ARGS 2>> "$PR/FAN_stat.txt"
done
```

- [ ] **Step 5: Top symbols**

```bash
for side in corosio asio; do
    echo "=== $side ==="
    perf report --no-children -n --stdio -i "$PR/FAN_${side}.data" \
        | grep -A 30 "^# Overhead" | head -40
done | tee "$PR/FAN_top_symbols.txt"
```

- [ ] **Step 6: Write `## FAN` section in `$PR/findings.md`**

Same six-bullet structure as IOCTX:
1. Throughput observed during profiling.
2. Top 5 corosio symbols.
3. Top 5 asio symbols.
4. Corosio-only symbols (in top 25).
5. Counter deltas vs IOCTX (interesting: does FAN look like IOCTX scaled up, or does coroutine-spawn introduce new costs?).
6. Hypothesis.

- [ ] **Step 7: Commit**

```bash
git add -f "$PR/findings.md"
git commit -m "perf: FAN profile findings"
```

---

## Task 5: Profile ACC (`accept_churn:concurrent/4`)

Accept-loop with 4 concurrent accept ops. Exercises `IORING_OP_ACCEPT` multishot and acceptor-state machinery. Loss of 25% suggests a real accept-path overhead, not just scheduler dispatch.

**Files:** mirror Task 4, substituting `ACC` and `accept_churn:concurrent/4`. `--enable-microbenchmarks` not required (accept_churn is in the default set).

- [ ] **Step 1: Record corosio**

```bash
DATE=$(date +%Y-%m-%d)
PR="/home/michael/git/boost/libs/corosio/perf_results/$DATE"
BIN="/home/michael/git/boost/libs/corosio/build_profile/perf/bench/corosio_bench"
taskset -c 0-7,16-23 perf record -F 999 -g --call-graph fp \
    -o "$PR/ACC_corosio.data" -- \
    "$BIN" --library corosio --backend io_uring \
           --category accept_churn --bench concurrent/4 \
           --duration 10 --warmup 0.5
```

- [ ] **Step 2: Record asio**

```bash
taskset -c 0-7,16-23 perf record -F 999 -g --call-graph fp \
    -o "$PR/ACC_asio.data" -- \
    "$BIN" --library asio \
           --category accept_churn --bench concurrent/4 \
           --duration 10 --warmup 0.5
```

- [ ] **Step 3: Collapse + flamegraphs + diff**

```bash
for side in corosio asio; do
    perf script -i "$PR/ACC_${side}.data" \
        | tools/flamegraph/stackcollapse-perf.pl > "$PR/ACC_${side}.folded"
    tools/flamegraph/flamegraph.pl --title "ACC $side" \
        "$PR/ACC_${side}.folded" > "$PR/ACC_${side}.svg"
done
tools/flamegraph/difffolded.pl "$PR/ACC_asio.folded" "$PR/ACC_corosio.folded" \
    | tools/flamegraph/flamegraph.pl --negate \
        --title "ACC corosio vs asio (red = corosio spends more time)" \
    > "$PR/ACC_diff.svg"
```

- [ ] **Step 4: `perf stat` + top symbols + syscall counts**

```bash
# perf stat
for side in corosio asio; do
    BIN_ARGS="--library $side --category accept_churn --bench concurrent/4 --duration 5 --warmup 0.5"
    [ "$side" = "corosio" ] && BIN_ARGS="$BIN_ARGS --backend io_uring"
    echo "=== $side ===" >> "$PR/ACC_stat.txt"
    taskset -c 0-7,16-23 perf stat -e cycles,instructions,branches,branch-misses,cache-references,cache-misses,context-switches \
        "$BIN" $BIN_ARGS 2>> "$PR/ACC_stat.txt"
done

# top symbols
for side in corosio asio; do
    echo "=== $side ==="
    perf report --no-children -n --stdio -i "$PR/ACC_${side}.data" \
        | grep -A 30 "^# Overhead" | head -40
done | tee "$PR/ACC_top_symbols.txt"

# strace: count and time each syscall to catch syscall-pattern differences
for side in corosio asio; do
    BIN_ARGS="--library $side --category accept_churn --bench concurrent/4 --duration 3 --warmup 0.3"
    [ "$side" = "corosio" ] && BIN_ARGS="$BIN_ARGS --backend io_uring"
    echo "=== $side ===" >> "$PR/ACC_strace.txt"
    taskset -c 0-7,16-23 strace -c -f -o /tmp/strace_$$.txt -- \
        "$BIN" $BIN_ARGS >/dev/null 2>&1
    cat /tmp/strace_$$.txt >> "$PR/ACC_strace.txt"
    rm /tmp/strace_$$.txt
done
```

Why strace here: accept-heavy workloads have a clear syscall expectation
(`io_uring_enter` for batches, occasionally `accept4` for fallbacks).
Counts and per-syscall time exposes whether corosio issues extra
`io_uring_enter` calls per accept (which would mean SQE batching is broken).

- [ ] **Step 5: Write `## ACC` section in `$PR/findings.md`**

Same six-bullet structure plus a seventh bullet:
7. **Syscall-pattern diff:** any syscall corosio issues more often than asio,
   or any syscall present in one side and absent in the other.

- [ ] **Step 6: Commit**

```bash
git add -f "$PR/findings.md"
git commit -m "perf: ACC profile findings"
```

---

## Task 6: Profile HTTP (`http_server:concurrent/16`)

Realistic single-context parallel I/O. -32% on this bench is the most
user-visible loss. Mixes accept, read, parse, write — confirms whether
IOCTX/FAN findings translate to real workloads.

**Files:** mirror Task 4, substituting `HTTP` and `http_server:concurrent/16`. Include `strace -c` (Step 4) because syscall mix matters in realistic I/O.

- [ ] **Step 1: Record corosio**

```bash
DATE=$(date +%Y-%m-%d)
PR="/home/michael/git/boost/libs/corosio/perf_results/$DATE"
BIN="/home/michael/git/boost/libs/corosio/build_profile/perf/bench/corosio_bench"
taskset -c 0-7,16-23 perf record -F 999 -g --call-graph fp \
    -o "$PR/HTTP_corosio.data" -- \
    "$BIN" --library corosio --backend io_uring \
           --category http_server --bench concurrent/16 \
           --duration 10 --warmup 0.5
```

- [ ] **Step 2: Record asio**

```bash
taskset -c 0-7,16-23 perf record -F 999 -g --call-graph fp \
    -o "$PR/HTTP_asio.data" -- \
    "$BIN" --library asio \
           --category http_server --bench concurrent/16 \
           --duration 10 --warmup 0.5
```

- [ ] **Step 3: Collapse + flamegraphs + diff**

```bash
for side in corosio asio; do
    perf script -i "$PR/HTTP_${side}.data" \
        | tools/flamegraph/stackcollapse-perf.pl > "$PR/HTTP_${side}.folded"
    tools/flamegraph/flamegraph.pl --title "HTTP $side" \
        "$PR/HTTP_${side}.folded" > "$PR/HTTP_${side}.svg"
done
tools/flamegraph/difffolded.pl "$PR/HTTP_asio.folded" "$PR/HTTP_corosio.folded" \
    | tools/flamegraph/flamegraph.pl --negate \
        --title "HTTP corosio vs asio (red = corosio spends more time)" \
    > "$PR/HTTP_diff.svg"
```

- [ ] **Step 4: perf stat + top symbols + strace**

```bash
# perf stat
for side in corosio asio; do
    BIN_ARGS="--library $side --category http_server --bench concurrent/16 --duration 5 --warmup 0.5"
    [ "$side" = "corosio" ] && BIN_ARGS="$BIN_ARGS --backend io_uring"
    echo "=== $side ===" >> "$PR/HTTP_stat.txt"
    taskset -c 0-7,16-23 perf stat -e cycles,instructions,branches,branch-misses,cache-references,cache-misses,context-switches \
        "$BIN" $BIN_ARGS 2>> "$PR/HTTP_stat.txt"
done

# top symbols
for side in corosio asio; do
    echo "=== $side ==="
    perf report --no-children -n --stdio -i "$PR/HTTP_${side}.data" \
        | grep -A 30 "^# Overhead" | head -40
done | tee "$PR/HTTP_top_symbols.txt"

# strace
for side in corosio asio; do
    BIN_ARGS="--library $side --category http_server --bench concurrent/16 --duration 3 --warmup 0.3"
    [ "$side" = "corosio" ] && BIN_ARGS="$BIN_ARGS --backend io_uring"
    echo "=== $side ===" >> "$PR/HTTP_strace.txt"
    taskset -c 0-7,16-23 strace -c -f -o /tmp/strace_$$.txt -- \
        "$BIN" $BIN_ARGS >/dev/null 2>&1
    cat /tmp/strace_$$.txt >> "$PR/HTTP_strace.txt"
    rm /tmp/strace_$$.txt
done
```

- [ ] **Step 5: Write `## HTTP` section in `$PR/findings.md`**

Same seven-bullet structure as ACC. Note explicitly whether the corosio-specific symbols overlap with IOCTX/FAN (suggesting the same root cause) or are new (HTTP-specific path that needs separate attention).

- [ ] **Step 6: Commit**

```bash
git add -f "$PR/findings.md"
git commit -m "perf: HTTP profile findings"
```

---

## Task 7: Profile TAIL (`socket_latency:concurrent/16` for p99)

The p99 loss at concurrent/16 is bimodal: p50 is +1284% (corosio is wildly
faster typical-case) but p99 is -77%. CPU profiling alone won't show this —
need to capture tail samples. Approach: use `perf record -e cs_etm` is too
exotic; instead capture **scheduling events** (`-e sched:sched_switch -e
sched:sched_wakeup`) on the corosio run only, and inspect the longest
between-wakeup gaps for the worker thread.

**Files:**
- Output: `perf_results/<DATE>/TAIL_corosio_sched.data`
- Output: `perf_results/<DATE>/TAIL_corosio_sched.txt`
- Output: `perf_results/<DATE>/TAIL_corosio_cpu.{data,folded,svg}` (CPU profile for context)

- [ ] **Step 1: CPU profile of the corosio side (context for the tail data)**

```bash
DATE=$(date +%Y-%m-%d)
PR="/home/michael/git/boost/libs/corosio/perf_results/$DATE"
BIN="/home/michael/git/boost/libs/corosio/build_profile/perf/bench/corosio_bench"
taskset -c 0-7,16-23 perf record -F 999 -g --call-graph fp \
    -o "$PR/TAIL_corosio_cpu.data" -- \
    "$BIN" --library corosio --backend io_uring \
           --category socket_latency --bench concurrent/16 \
           --duration 10 --warmup 0.5

perf script -i "$PR/TAIL_corosio_cpu.data" \
    | tools/flamegraph/stackcollapse-perf.pl > "$PR/TAIL_corosio_cpu.folded"
tools/flamegraph/flamegraph.pl --title "TAIL corosio CPU" \
    "$PR/TAIL_corosio_cpu.folded" > "$PR/TAIL_corosio_cpu.svg"
```

- [ ] **Step 2: Capture scheduling events (needs CAP_PERFMON or root)**

```bash
# May require sudo on this host; if it fails, try `sudo perf record ...` and
# re-run subsequent perf commands under sudo too (or chown the data file).
taskset -c 0-7,16-23 perf record -e 'sched:sched_switch,sched:sched_wakeup' \
    -o "$PR/TAIL_corosio_sched.data" -- \
    "$BIN" --library corosio --backend io_uring \
           --category socket_latency --bench concurrent/16 \
           --duration 5 --warmup 0.5
```

If `perf record` rejects the events with `Permission denied`, run with
sudo and `chown $USER:$USER "$PR/TAIL_corosio_sched.data"` afterwards.

- [ ] **Step 3: Find the longest sched-switch gaps (= longest blocked intervals)**

```bash
perf script -i "$PR/TAIL_corosio_sched.data" \
    --fields comm,pid,tid,time,event,trace \
    > "$PR/TAIL_corosio_sched.txt"

# Quick eyeball: 20 longest off-CPU intervals for the bench process.
# Pass $PR through the env so the heredoc body doesn't have to expand it.
SCHED_TXT="$PR/TAIL_corosio_sched.txt" TOPGAPS="$PR/TAIL_corosio_topgaps.txt" \
python3 - <<'PY'
import os, re
events = []
with open(os.environ["SCHED_TXT"]) as f:
    for line in f:
        m = re.match(r"\s*(\S+)\s+(\d+)\/(\d+)\s+\[\d+\]\s+(\d+\.\d+):\s+(\S+):", line)
        if not m: continue
        comm, pid, tid, ts, ev = m.groups()
        events.append((float(ts), comm, int(tid), ev))
# Per-tid gap between sched_wakeup (became runnable) and sched_switch (started running)
ready = {}
gaps = []
for ts, comm, tid, ev in events:
    if "wakeup" in ev:
        ready[tid] = (ts, comm)
    elif "switch" in ev and tid in ready:
        gap = ts - ready[tid][0]
        gaps.append((gap*1e6, ready[tid][1], tid, ready[tid][0]))
        del ready[tid]
gaps.sort(reverse=True)
with open(os.environ["TOPGAPS"], "w") as out:
    out.write("# Top 20 wakeup-to-switch gaps (microseconds):\n")
    out.write(f"{'gap_us':>10}  {'comm':<16}  {'tid':>8}  {'wakeup_ts':>15}\n")
    for g, comm, tid, ts in gaps[:20]:
        out.write(f"{g:10.1f}  {comm:<16}  {tid:>8}  {ts:15.6f}\n")
PY
cat "$PR/TAIL_corosio_topgaps.txt"
```

Expected: rows in microseconds. A handful of multi-millisecond gaps would
explain the p99 outliers — that's a thread that *was* woken but didn't run
for that long.

- [ ] **Step 4: Optional — perf sched latency report**

```bash
perf sched latency -i "$PR/TAIL_corosio_sched.data" \
    --sort max 2>&1 | head -40 | tee "$PR/TAIL_corosio_sched_latency.txt"
```

This gives a per-task summary including `max delay` (worst wakeup-to-run
latency) which is the most direct p99-tail measurement we can get from
perf alone.

- [ ] **Step 5: Write `## TAIL` section in `$PR/findings.md`**

1. **Throughput + p50/p99 observed during profiling.**
2. **Top 5 corosio hot symbols** (from the CPU flamegraph in Step 1) — establishes whether the tail samples land in the same hot path as the median case.
3. **The 5 worst sched gaps** from `TAIL_corosio_topgaps.txt`. Each row should ideally point to a comm/tid we can map back to a thread role (worker thread, helper, etc.).
4. **`perf sched latency` max delay per task** — distill the per-thread max.
5. **Working theory for the p99 outliers** — e.g. "occasional 5ms wakeup gap on the bench worker thread, scheduled out by kernel `<kfunc>` ; suggests we're not setting affinity inside the corosio worker pool" or "long critical section under `dispatch_mutex` holds the leader, blocking all followers; budget exhaustion path is the trigger". Be specific.

- [ ] **Step 6: Commit**

```bash
git add -f "$PR/findings.md"
git commit -m "perf: TAIL profile findings"
```

---

## Task 8: Consolidated report with prioritized recommendations

**Files:**
- Create: `perf_results/<DATE>/report.md`

- [ ] **Step 1: Cross-reference findings**

Read every `## TAG` section from `$PR/findings.md` and look for **symbols that appear in multiple sections**. A function that's hot in IOCTX, FAN, and HTTP is the highest-leverage tuning target — fixing it improves all three. Note these in a table.

- [ ] **Step 2: Write the report skeleton**

Create `$PR/report.md`:

```markdown
# io_uring Backend Profiling Report — <DATE>

## Methodology

- Compiler: clang++ (RelWithDebInfo, -O2 -fno-omit-frame-pointer -g)
- CPU: AMD Ryzen 9 7950X, pinned to CCX0 (cores 0-7 + SMT 16-23)
- Governor: performance
- Benchmark binary: `build_profile/perf/bench/corosio_bench`
- Profile: `perf record -F 999 -g --call-graph fp`, 10s per run
- Counters: `perf stat` (5s warmup'd run)
- Syscalls: `strace -c`
- Tail: `perf record -e sched:*` + `perf sched latency`

## Headline findings

(2-3 sentences max — the elevator pitch for "where is corosio losing".)

## Per-benchmark observations

### IOCTX (io_context:single_threaded)
- Loss: -93%
- Hot path in corosio: ...
- Hot path in asio: ...
- Diff: ...

### FAN (fan_out:fork_join/16)
... (same shape) ...

### ACC (accept_churn:concurrent/4)
... (same shape, with syscall delta) ...

### HTTP (http_server:concurrent/16)
... (same shape, with syscall delta) ...

### TAIL (socket_latency:concurrent/16 p99)
- Loss: p99 -77% (p50 +1284%)
- Worst scheduling gap: ... ms
- Working theory: ...

## Symbols hot across multiple benchmarks

| Symbol | IOCTX | FAN | ACC | HTTP | TAIL |
|--------|-------|-----|-----|------|------|
| `boost::corosio::detail::...::foo` | 18% | 22% | 9% | 11% | 14% |
| ... | ... | ... | ... | ... | ... |

## Prioritized tuning targets

1. **<target>** — biggest payoff, applies to N benchmarks, root cause:
   <one sentence>. Likely fix shape: <one sentence>.
2. **<target>** — applies to N benchmarks, root cause: <one sentence>.
3. ...

## Out of scope (do not chase)

- Multithread benches — already +50% to +200%
- Bidirectional throughput regressions — small, likely buffer-sizing details that should fall out of the dispatch fixes.
```

Fill in every section with actual numbers and symbol names from the findings — no `<placeholder>` tokens in the final document.

- [ ] **Step 3: Reality-check the recommendations**

For each prioritized target in the report:
- Name the file(s) and line(s) where the hot symbol lives (use `addr2line` if needed).
- Sanity-check that the proposed fix shape doesn't conflict with an invariant elsewhere (e.g., "remove the dispatch mutex" — would break thread safety in multi-context mode).
- If a target is speculative, mark it explicitly: "Hypothesis only — needs A/B test before claiming a fix."

- [ ] **Step 4: Commit the report**

```bash
git add -f "$PR/report.md"
git add -f "$PR/findings.md"
git commit -m "perf: consolidated profiling report"
```

- [ ] **Step 5: Surface the report to the user**

Print the path to `$PR/report.md` and the headline findings. Pause before proposing any code changes — the report is the artifact, not the fix. The next session (or the next plan) will pick concrete targets from this report and implement.

---

## Notes for the implementing engineer

- **One benchmark at a time.** Profiles run for 10s plus collapse plus flamegraph; sequencing matters because perf record competes with the bench for CPU. Never run two `perf record` commands in parallel.
- **Don't trust a single `perf stat` run.** If counters look strange (negative IPC, zero context-switches), re-run. PMU events on shared hosts are sometimes unavailable.
- **Demangle when reading reports.** `perf report` already pipes through `c++filt`, but if you see mangled names (`_ZN5boost...`), demangle manually: `echo '_ZN...' | c++filt`.
- **The IOCTX micro might be misleading.** If asio's `io_context:single_threaded` is essentially a `for` loop calling a handler, and corosio's exercises the full async-completion machinery, the -93% comparison may be apples-to-oranges. Read both implementations before treating IOCTX as a real bottleneck signal. The check: `perf/bench/asio/coroutine/io_context_bench.cpp` and `perf/bench/corosio/io_context_bench.cpp`.
- **Symbol attribution gotchas.** Coroutine frames produce synthetic symbol names that are not always demangled cleanly. If you see `operator()` dominating with no apparent owner, run `perf script` and inspect the full stack — the parent frame identifies the coroutine type.
