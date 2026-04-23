//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

/* Adaptive convergence benchmark runner.

   Instead of running each benchmark once for a fixed duration, this runner
   executes benchmarks in interleaved rounds of short "slices." Each round
   shuffles the active benchmark set to cancel correlated system noise
   (thermal state, background load, cache residency). Benchmarks
   individually converge when the 95% confidence interval on the p33
   estimator (33rd percentile) narrows below a configurable precision
   target, and a split-half stability check passes.

   Inspired by Folly's adaptive benchmark mode and grounded in:
   - Mytkowicz et al., "Producing Wrong Data..." (ASPLOS 2009)
   - Kalibera & Jones, "Rigorous Benchmarking in Reasonable Time" (ISMM 2013)
   - Chen & Revels, "Robust Benchmarking in Noisy Environments" (2016)
*/

#ifndef BOOST_COROSIO_BENCH_ADAPTIVE_HPP
#define BOOST_COROSIO_BENCH_ADAPTIVE_HPP

#include "benchmark.hpp"
#include "../../common/perf.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace bench {

struct adaptive_config
{
    double precision_pct    = 1.0;   // 95% CI of p33 must be < this % of p33
    double max_time_s       = 60.0;  // per-benchmark wall-clock cap
    int    min_samples      = 10;    // minimum before convergence checking
    double slice_duration_s = 0.1;   // duration per sample
    // Fraction of each slice treated as ramp-up (warmup). At the
    // warmup/measurement boundary the harness resets the state's
    // counters so that only steady-state work is measured.
    //
    // Only applied to benchmarks whose effective slice duration is
    // at least `long_slice_threshold_s` — short and medium slices
    // don't have meaningful ramp-up phases, and losing 25% to warmup
    // just shrinks the measurement window, which increases per-sample
    // variance and needs more samples to hit the precision target.
    // Also skipped for benchmarks flagged bench_flags::local_counters,
    // since they publish counters only at the end of the run.
    double warmup_ratio            = 0.25;
    // Threshold used both for applying warmup and for loosening the
    // precision target. Long-slice benchmarks are the noisy ones
    // (multithread, big-buffer throughput); trying to hit 1% CI on
    // them just burns time at max-time without converging. The
    // relaxed `precision_pct_long_slice` target applies for those.
    double long_slice_threshold_s  = 3.0;
    double precision_pct_long_slice = 2.0;
};

// Tracks one benchmark's adaptive state across rounds.
struct bench_slot
{
    // Identity
    std::string library;
    std::string category;
    std::string name;
    std::function<void(state&)> fn;
    std::vector<int64_t> ranges;
    bool needs_drain     = false;
    bool local_counters  = false; // publishes counters at end; skip warmup reset
    double slice_duration_s = 0.0; // 0 = use adaptive_config default

    // Adaptive state
    bool converged             = false;
    int  consecutive_failures  = 0;
    std::vector<double> sample_values; // primary metric per sample
    double elapsed_total       = 0.0;

    // Latency and counters from the most recent sample, for final output
    perf::statistics last_latency;
    std::unordered_map<std::string, double> last_counters;
    double last_elapsed = 0.0;
    int64_t last_ops    = 0;
    int64_t last_bytes  = 0;
    int64_t last_items  = 0;

    // Per-sample latency percentiles (one value per adaptive sample).
    // Captures run-to-run variability of the tail, not within-run spread.
    std::vector<double> latency_p50_samples;
    std::vector<double> latency_p99_samples;

    // Which primary metric this benchmark uses
    enum class metric_kind { bytes_per_sec, ops_per_sec, items_per_sec, none };
    metric_kind kind = metric_kind::none;
};

class adaptive_runner
{
    adaptive_config cfg_;
    std::vector<bench_slot>& slots_;
    result_collector& collector_;
    std::mt19937 rng_{std::random_device{}()};

public:
    adaptive_runner(
        adaptive_config cfg,
        std::vector<bench_slot>& slots,
        result_collector& collector)
        : cfg_(cfg)
        , slots_(slots)
        , collector_(collector)
    {
    }

    void run()
    {
        if (slots_.empty())
            return;

        // Measurement rounds (no warmup — p33 naturally filters cold-start
        // samples into the upper tail that doesn't affect the estimator)
        int round = 0;
        while (true)
        {
            ++round;

            // Collect active (non-converged, non-failed) slots
            std::vector<std::size_t> active;
            for (std::size_t i = 0; i < slots_.size(); ++i)
            {
                auto& s = slots_[i];
                if (!s.converged && s.consecutive_failures < 3 &&
                    s.elapsed_total < cfg_.max_time_s)
                {
                    active.push_back(i);
                }
            }

            if (active.empty())
                break;

            std::cout << "\n--- Round " << round << " ("
                      << active.size() << " active) ---\n";

            // Shuffle for interleaving
            std::shuffle(active.begin(), active.end(), rng_);

            for (std::size_t idx : active)
            {
                auto& slot = slots_[idx];

                if (slot.needs_drain)
                    perf::await_conntrack_drain();

                double value = run_one_sample(slot);

                if (value <= 0.0)
                {
                    slot.consecutive_failures++;
                    std::cout << "  " << format_name(slot)
                              << "  FAILED\n";
                    continue;
                }
                slot.consecutive_failures = 0;
                slot.sample_values.push_back(value);

                // Progress
                int n = static_cast<int>(slot.sample_values.size());
                std::cout << "  " << format_name(slot)
                          << "  #" << n << "  "
                          << format_value(slot);

                // Convergence check
                if (n >= cfg_.min_samples)
                {
                    auto [ci, cv] = compute_stats(slot);

                    if (check_convergence(slot))
                    {
                        slot.converged = true;
                        std::cout << "  -> converged (CI="
                                  << std::fixed << std::setprecision(1)
                                  << ci << "%, CV=" << cv
                                  << "%, n=" << n << ")";
                    }
                    else if (slot.elapsed_total >= cfg_.max_time_s)
                    {
                        slot.converged = true;
                        std::cout << "  -> max time (CI="
                                  << std::fixed << std::setprecision(1)
                                  << ci << "%, CV=" << cv
                                  << "%, n=" << n << ")";
                    }
                    else if (n % 10 == 0)
                    {
                        // Show progress toward convergence every 10 samples
                        std::cout << "  (CI=" << std::fixed
                                  << std::setprecision(1) << ci
                                  << "%, target=" << effective_precision(slot)
                                  << "%)";
                    }
                }

                std::cout << "\n" << std::flush;
            }
        }

        std::cout << "\n--- Finalizing results ---\n";

        // Emit results
        for (auto& slot : slots_)
            finalize(slot);
    }

private:
    double run_one_sample(bench_slot& slot)
    {
        double dur = slot.slice_duration_s > 0.0
            ? slot.slice_duration_s : cfg_.slice_duration_s;

        // Warmup applies only when:
        //   - the benchmark streams counters (not flagged local_counters)
        //   - the slice is long enough for a meaningful warmup phase
        //     (short slices have no ramp-up, and losing 25% of a 100ms
        //     sample to warmup just shrinks the measurement window).
        double warmup_s = 0.0;
        if (!slot.local_counters &&
            cfg_.warmup_ratio > 0.0 && cfg_.warmup_ratio < 1.0 &&
            dur >= cfg_.long_slice_threshold_s)
        {
            warmup_s = cfg_.warmup_ratio * dur;
        }

        state st(dur, slot.ranges);

        std::thread warmup_thread;
        if (warmup_s > 0.0)
        {
            warmup_thread = std::thread([&st, warmup_s]() {
                std::this_thread::sleep_for(
                    std::chrono::duration<double>(warmup_s));
                st.reset_counters();
            });
        }

        slot.fn(st);

        if (warmup_thread.joinable())
            warmup_thread.join();

        // When warmup is active, the counters reflect only the
        // measurement window (post-warmup). Use that window for the
        // rate denominator instead of the benchmark's own elapsed
        // (which includes warmup time).
        double elapsed = warmup_s > 0.0
            ? (dur - warmup_s)
            : st.elapsed_seconds();
        slot.elapsed_total += dur;

        // Stash last-sample data for finalize()
        slot.last_latency = st.latency();
        slot.last_counters = st.counters;
        slot.last_elapsed = elapsed;
        slot.last_ops = st.total_ops();
        slot.last_bytes = st.total_bytes();
        slot.last_items = st.total_items();

        // Capture per-sample latency percentiles so finalize() can compute
        // run-to-run variance of the tail.
        if (st.latency().count() > 0)
        {
            slot.latency_p50_samples.push_back(st.latency().p50());
            slot.latency_p99_samples.push_back(st.latency().p99());
        }

        // Extract primary metric and detect kind on first sample
        if (st.total_bytes() > 0)
        {
            if (slot.kind == bench_slot::metric_kind::none)
                slot.kind = bench_slot::metric_kind::bytes_per_sec;
            return static_cast<double>(st.total_bytes()) / elapsed;
        }
        if (st.total_ops() > 0)
        {
            if (slot.kind == bench_slot::metric_kind::none)
                slot.kind = bench_slot::metric_kind::ops_per_sec;
            return static_cast<double>(st.total_ops()) / elapsed;
        }
        if (st.total_items() > 0)
        {
            if (slot.kind == bench_slot::metric_kind::none)
                slot.kind = bench_slot::metric_kind::items_per_sec;
            return static_cast<double>(st.total_items()) / elapsed;
        }
        return 0.0;
    }

    // Compute the p-th percentile from a sorted vector using linear
    // interpolation (same algorithm as perf::statistics::percentile).
    static double percentile(std::vector<double> const& sorted, double p)
    {
        if (sorted.empty())
            return 0.0;
        double index = p * static_cast<double>(sorted.size() - 1);
        auto lower   = static_cast<std::size_t>(std::floor(index));
        auto upper   = static_cast<std::size_t>(std::ceil(index));
        if (lower == upper)
            return sorted[lower];
        double frac = index - static_cast<double>(lower);
        return sorted[lower] * (1.0 - frac) + sorted[upper] * frac;
    }

    // p33 of the sample values
    static double p33(std::vector<double> const& values)
    {
        std::vector<double> sorted = values;
        std::sort(sorted.begin(), sorted.end());
        return percentile(sorted, 1.0 / 3.0);
    }

    // Binomial CI on the p-th percentile using normal approximation.
    // Returns (lo_value, hi_value) from the order statistics.
    static std::pair<double, double>
    percentile_ci(std::vector<double> const& values, double p)
    {
        std::vector<double> sorted = values;
        std::sort(sorted.begin(), sorted.end());

        auto n = static_cast<double>(sorted.size());
        double z = 1.96; // 95% confidence

        // Normal approximation to binomial: indices for CI endpoints
        double np   = n * p;
        double se   = std::sqrt(n * p * (1.0 - p));
        double lo_f = np - z * se;
        double hi_f = np + z * se;

        // Clamp to valid indices
        auto lo = static_cast<std::size_t>(
            std::max(0.0, std::floor(lo_f)));
        auto hi = static_cast<std::size_t>(
            std::min(static_cast<double>(sorted.size() - 1),
                     std::ceil(hi_f)));

        return {sorted[lo], sorted[hi]};
    }

    // Effective precision target for a slot: long-slice (noisy)
    // benchmarks get the relaxed target, everything else gets the
    // default. Keeps short benchmarks tight without burning time
    // on noisy ones that won't converge at 1% anyway.
    double effective_precision(bench_slot const& slot) const
    {
        double dur = slot.slice_duration_s > 0.0
            ? slot.slice_duration_s : cfg_.slice_duration_s;
        return dur >= cfg_.long_slice_threshold_s
            ? cfg_.precision_pct_long_slice
            : cfg_.precision_pct;
    }

    bool check_convergence(bench_slot const& slot) const
    {
        auto const& vals = slot.sample_values;
        auto n = static_cast<int>(vals.size());
        if (n < cfg_.min_samples)
            return false;

        double est = p33(vals);
        if (est <= 0.0)
            return false;

        // Precision check: 95% CI width on p33 < effective target
        auto [lo, hi] = percentile_ci(vals, 1.0 / 3.0);
        double ci_width_pct = (hi - lo) / est * 100.0;
        if (ci_width_pct > effective_precision(slot))
            return false;

        // Split-half stability
        if (!split_half_ok(vals))
            return false;

        return true;
    }

    static bool split_half_ok(std::vector<double> const& vals)
    {
        auto n = vals.size();
        if (n < 6)
            return true; // not enough for meaningful split

        auto half = n / 2;
        std::vector<double> first(vals.begin(), vals.begin() + half);
        std::vector<double> second(vals.begin() + half, vals.end());

        std::sort(first.begin(), first.end());
        std::sort(second.begin(), second.end());

        double p33_first  = percentile(first, 1.0 / 3.0);
        double p33_second = percentile(second, 1.0 / 3.0);
        double p33_all    = p33(vals);

        if (p33_all <= 0.0)
            return true;

        double drift = std::abs(p33_first - p33_second) / p33_all;
        return drift < 0.02; // 2% tolerance
    }

    // Returns (ci_pct, cv_pct) for display
    std::pair<double, double>
    compute_stats(bench_slot const& slot) const
    {
        auto const& vals = slot.sample_values;
        double est = p33(vals);
        if (est <= 0.0)
            return {0.0, 0.0};

        auto [lo, hi] = percentile_ci(vals, 1.0 / 3.0);
        double ci_pct = (hi - lo) / est * 100.0;

        double mean_val = std::accumulate(
            vals.begin(), vals.end(), 0.0) / vals.size();
        double sq_sum = 0.0;
        for (double v : vals)
            sq_sum += (v - mean_val) * (v - mean_val);
        double stddev = vals.size() > 1
            ? std::sqrt(sq_sum / (vals.size() - 1))
            : 0.0;
        double cv = mean_val > 0.0 ? (stddev / mean_val) * 100.0 : 0.0;

        return {ci_pct, cv};
    }

    void finalize(bench_slot& slot)
    {
        if (slot.sample_values.empty())
            return;

        double est = p33(slot.sample_values);
        auto [ci, cv] = compute_stats(slot);
        int n = static_cast<int>(slot.sample_values.size());

        double mean_val = std::accumulate(
            slot.sample_values.begin(), slot.sample_values.end(), 0.0)
            / slot.sample_values.size();

        benchmark_result result(slot.library, slot.category, slot.name);

        // Primary metric = p33 (for compatibility with existing scripts)
        char const* metric_name = nullptr;
        switch (slot.kind)
        {
        case bench_slot::metric_kind::bytes_per_sec:
            metric_name = "bytes_per_sec";
            break;
        case bench_slot::metric_kind::ops_per_sec:
            metric_name = "ops_per_sec";
            break;
        case bench_slot::metric_kind::items_per_sec:
            metric_name = "items_per_sec";
            break;
        default:
            return;
        }

        result.add(metric_name, est);
        result.add(std::string(metric_name) + "_mean", mean_val);
        result.add(std::string(metric_name) + "_cv_pct", cv);
        result.add(std::string(metric_name) + "_ci95_pct", ci);
        result.add("n_samples", static_cast<double>(n));
        result.add("converged", slot.converged ? 1.0 : 0.0);

        result.add_sample_array(
            std::string(metric_name) + "_samples",
            slot.sample_values);

        // Latency stats from last sample
        if (slot.last_latency.count() > 0)
            result.add_latency_stats("latency", slot.last_latency);

        // Per-sample percentile CV — captures run-to-run tail stability.
        // Different from latency_p50_ns / latency_p99_ns above, which come
        // from the final sample's within-run distribution.
        auto emit_percentile_samples = [&](char const* key,
                                           std::vector<double> const& samples)
        {
            if (samples.empty())
                return;
            double mean_v = std::accumulate(
                samples.begin(), samples.end(), 0.0) / samples.size();
            double sq_sum = 0.0;
            for (double v : samples)
                sq_sum += (v - mean_v) * (v - mean_v);
            double stddev = samples.size() > 1
                ? std::sqrt(sq_sum / (samples.size() - 1))
                : 0.0;
            double cv = mean_v > 0.0 ? (stddev / mean_v) * 100.0 : 0.0;
            result.add(std::string(key) + "_mean_ns", mean_v);
            result.add(std::string(key) + "_cv_pct", cv);
            result.add_sample_array(
                std::string(key) + "_samples", samples);
        };
        emit_percentile_samples("latency_p50", slot.latency_p50_samples);
        emit_percentile_samples("latency_p99", slot.latency_p99_samples);

        // Throughput-specific fields from last sample
        if (slot.last_bytes > 0)
        {
            result.add("bytes", static_cast<double>(slot.last_bytes));
            result.add("elapsed_s", slot.last_elapsed);
        }
        if (slot.last_ops > 0)
        {
            result.add("ops", static_cast<double>(slot.last_ops));
            result.add("elapsed_s", slot.last_elapsed);
        }
        if (slot.last_items > 0)
        {
            result.add("items", static_cast<double>(slot.last_items));
            result.add("elapsed_s", slot.last_elapsed);
        }

        // Custom counters from last sample
        for (auto const& [k, v] : slot.last_counters)
            result.add(k, v);

        collector_.add(std::move(result));

        std::cout << "  " << format_name(slot)
                  << ": p33=" << format_metric(est, slot.kind)
                  << ", mean=" << format_metric(mean_val, slot.kind)
                  << ", CV=" << std::fixed << std::setprecision(1)
                  << cv << "%, CI=" << ci << "%, n=" << n
                  << (slot.converged ? "" : " (NOT CONVERGED)")
                  << "\n";
    }

    static std::string format_name(bench_slot const& slot)
    {
        std::string s;
        if (!slot.library.empty())
            s += "(" + slot.library + ") ";
        s += "[" + slot.category + "] " + slot.name;
        return s;
    }

    std::string format_value(bench_slot const& slot) const
    {
        if (slot.sample_values.empty())
            return "";
        return format_metric(slot.sample_values.back(), slot.kind);
    }

    static std::string format_metric(
        double value, bench_slot::metric_kind kind)
    {
        switch (kind)
        {
        case bench_slot::metric_kind::bytes_per_sec:
            return perf::format_throughput(value);
        case bench_slot::metric_kind::ops_per_sec:
        case bench_slot::metric_kind::items_per_sec:
            return perf::format_rate(value);
        default:
            return std::to_string(value);
        }
    }
};

} // namespace bench

#endif
