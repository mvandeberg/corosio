#
# Copyright (c) 2026 Steve Gerbino
# Copyright (c) 2026 Michael Vandeberg
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
# Official repository: https://github.com/cppalliance/corosio
#
# Christopher M. Kohlhoff's classic Boost.Asio callback throughput benchmark
# (the "asio_from_chenshuo" variant chenshuo packaged in his recipes).
# Modernized for Boost.Asio >= 1.66; builds header-only against Boost.Asio
# with no compiled-Boost dependency.
#

if (NOT TARGET Boost::asio)
    return()
endif ()

add_executable(corosio_throughput_asio_callback_server server.cpp)
add_executable(corosio_throughput_asio_callback_client client.cpp)

foreach (target corosio_throughput_asio_callback_server corosio_throughput_asio_callback_client)
    target_link_libraries(${target} PRIVATE Boost::asio Threads::Threads)
    target_compile_definitions(${target} PRIVATE
        BOOST_ASIO_HEADER_ONLY=1
        BOOST_ERROR_CODE_HEADER_ONLY=1
        BOOST_BIND_GLOBAL_PLACEHOLDERS=1)
    target_compile_features(${target} PRIVATE cxx_std_17)
    set_property(TARGET ${target} PROPERTY FOLDER "perf/throughput")
    if (COROSIO_BENCH_LTO_SUPPORTED)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif ()
endforeach ()
