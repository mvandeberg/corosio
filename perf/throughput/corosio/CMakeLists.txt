#
# Copyright (c) 2026 Steve Gerbino
# Copyright (c) 2026 Michael Vandeberg
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#
# corosio port of the chenshuo throughput benchmark, using the native epoll
# variants for fair comparison against asio's template-based concrete socket
# types.
#

add_executable(corosio_throughput_corosio_server server.cpp)
add_executable(corosio_throughput_corosio_client client.cpp)

foreach (target corosio_throughput_corosio_server corosio_throughput_corosio_client)
    target_link_libraries(${target} PRIVATE Boost::corosio Threads::Threads)
    target_compile_features(${target} PRIVATE cxx_std_20)
    set_property(TARGET ${target} PROPERTY FOLDER "perf/throughput")
    if (COROSIO_BENCH_LTO_SUPPORTED)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif ()
endforeach ()
