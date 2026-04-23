//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

/* Shared adaptive slice-duration tiers for benchmark suites.

   Each helper mutates a benchmark_suite in place, applying the same
   per-entry and per-argument slice_duration overrides regardless of
   which library (corosio, asio, asio_callback) registered it.  Helpers
   are no-ops for entries that a particular library doesn't define, so
   the same helper can be called from every registration site.

   Tiers:
     - 0.1s: default (fast to converge, no ramp-up)
     - 1.0s: medium-buffer throughput, some latency benchmarks, some
             io_context microbenchmarks that are noisy at 0.1s
     - 3.0s: multithread benchmarks (thread pool ramp-up), very large
             buffer throughput, and the noisiest variants
*/

#ifndef BOOST_COROSIO_BENCH_SLICE_TIERS_HPP
#define BOOST_COROSIO_BENCH_SLICE_TIERS_HPP

#include "suite.hpp"

namespace bench {

inline void apply_socket_throughput_tiers(benchmark_suite& s)
{
    s.set_entry_slice_duration_for("bidirectional", 65536, 3.0);
    s.set_entry_slice_duration_for("bidirectional_lockless", 65536, 3.0);
    s.set_entry_slice_duration("multithread", 3.0);
}

inline void apply_local_socket_throughput_tiers(benchmark_suite& s)
{
    s.set_entry_slice_duration_for("unidirectional", 65536, 1.0);
    s.set_entry_slice_duration_for("unidirectional", 262144, 1.0);
    s.set_entry_slice_duration_for("unidirectional", 1048576, 3.0);
    s.set_entry_slice_duration_for("unidirectional_lockless", 65536, 1.0);
    s.set_entry_slice_duration_for("unidirectional_lockless", 262144, 1.0);
    s.set_entry_slice_duration_for("unidirectional_lockless", 1048576, 3.0);
    s.set_entry_slice_duration_for("bidirectional", 65536, 1.0);
}

inline void apply_local_socket_latency_tiers(benchmark_suite& s)
{
    s.set_entry_slice_duration_for("pingpong", 1024, 1.0);
    s.set_entry_slice_duration_for("pingpong_lockless", 1, 1.0);
    s.set_entry_slice_duration_for("pingpong_lockless", 64, 1.0);
}

inline void apply_http_server_tiers(benchmark_suite& s)
{
    s.set_entry_slice_duration("multithread", 3.0);
}

inline void apply_io_context_tiers(benchmark_suite& s)
{
    s.set_entry_slice_duration("multithreaded", 3.0);
    s.set_entry_slice_duration("concurrent", 3.0);
    s.set_entry_slice_duration("large_event_buffer", 1.0);
}

} // namespace bench

#endif
