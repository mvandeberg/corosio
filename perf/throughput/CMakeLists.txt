#
# Copyright (c) 2026 Steve Gerbino
# Copyright (c) 2026 Michael Vandeberg
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#
# TCP throughput benchmark: separate server/client binaries that talk over
# a TCP socket pair, derived from Christopher M. Kohlhoff's classic asio
# bench (chenshuo packaged it as the "tpc" benchmark in his recipes,
# https://github.com/chenshuo/recipes). One binary pair per implementation:
#
#   * asio_callback - the chenshuo/recipes callback variant, header-only
#                     Boost.Asio, modernized for Boost.Asio >= 1.66
#   * asio_coro     - modern Boost.Asio with awaitable + co_spawn
#   * corosio       - corosio native_io_context port mirroring the asio_coro flow
#
# The server reads bytes and echoes them back; the client opens N sessions
# in parallel, runs for a fixed wall-clock window, and reports MiB/s. Drive
# a configuration matrix from your own harness.
#
# Sets COROSIO_BENCH_LTO_SUPPORTED locally so the binaries opt into LTO
# wherever the toolchain supports it (mirroring perf/bench).
#

include(CheckIPOSupported)
check_ipo_supported(
    RESULT COROSIO_BENCH_LTO_SUPPORTED
    OUTPUT COROSIO_BENCH_LTO_ERROR
    LANGUAGES CXX)
if (MINGW)
    set(COROSIO_BENCH_LTO_SUPPORTED FALSE)
endif ()

add_subdirectory(asio_callback)
add_subdirectory(asio_coro)
add_subdirectory(corosio)
