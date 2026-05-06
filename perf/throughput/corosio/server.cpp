//
// server.cpp (corosio port of Christopher M. Kohlhoff's asio throughput
//             benchmark, mirroring perf/throughput/asio_callback/server.cpp)
//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Uses the native epoll variants (devirtualized hot path) for fair comparison
// against asio's template-based concrete socket types.
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/backend.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/corosio/ipv4_address.hpp>
#include <boost/corosio/native/native_io_context.hpp>
#include <boost/corosio/native/native_socket_option.hpp>
#include <boost/corosio/native/native_tcp_acceptor.hpp>
#include <boost/corosio/native/native_tcp_socket.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/ex/run_async.hpp>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <boost/capy/buffers.hpp>
#include <boost/capy/write.hpp>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

using io_context_t   = corosio::native_io_context<corosio::epoll>;
using tcp_socket_t   = corosio::native_tcp_socket<corosio::epoll>;
using tcp_acceptor_t = corosio::native_tcp_acceptor<corosio::epoll>;
template<class T = void>
using ttask = capy::task<T>;

ttask<>
session(tcp_socket_t sock, std::size_t block_size)
{
    try
    {
        sock.set_option(corosio::native_socket_option::no_delay(true));
    }
    catch (std::exception&)
    {
        sock.close();
        co_return;
    }

    std::unique_ptr<char[]> buf(new char[block_size]);

    for (;;)
    {
        auto [ec, n] = co_await sock.read_some(
            capy::mutable_buffer(buf.get(), block_size));
        if (ec)
            break;

        auto [wec, wn] = co_await capy::write(
            sock, capy::const_buffer(buf.get(), n));
        if (wec)
            break;
    }

    sock.close();
}

ttask<>
accept_loop(
    io_context_t& ioc,
    tcp_acceptor_t& acc,
    std::size_t block_size)
{
    for (;;)
    {
        tcp_socket_t peer(ioc);
        auto [ec] = co_await acc.accept(peer);
        if (ec)
            break;

        capy::run_async(ioc.get_executor())(
            session(std::move(peer), block_size));
    }
}

int main(int argc, char* argv[])
{
    if (argc != 5 && argc != 8)
    {
        std::cerr << "Usage: server <address> <port> <threads> <blocksize> "
                     "[<inline_initial> <inline_max> <unassisted>]\n";
        return 1;
    }

    corosio::ipv4_address address{std::string_view(argv[1])};
    std::uint16_t port = static_cast<std::uint16_t>(std::atoi(argv[2]));
    int thread_count = std::atoi(argv[3]);
    if (thread_count < 1)
        thread_count = 1;
    std::size_t block_size = static_cast<std::size_t>(std::atoi(argv[4]));
    unsigned inline_initial = (argc == 8) ? std::atoi(argv[5]) : 0;
    unsigned inline_max     = (argc == 8) ? std::atoi(argv[6]) : 1;
    unsigned unassisted     = (argc == 8) ? std::atoi(argv[7]) : 0;

    // Match asio's concurrency_hint=1 lockless single-thread mode: corosio
    // exposes this as io_context_options::single_threaded, which is NOT
    // enabled by the concurrency_hint constructor alone.
    corosio::io_context_options opts;
    opts.single_threaded = (thread_count == 1);
    if (argc == 8) {
        opts.inline_budget_initial = inline_initial;
        opts.inline_budget_max = inline_max;
        opts.unassisted_budget = unassisted;
    }
    io_context_t ioc(opts, static_cast<unsigned>(thread_count));
    tcp_acceptor_t acc(ioc);
    acc.open(corosio::tcp::v4());
    acc.set_option(corosio::native_socket_option::reuse_address(true));
    if (auto ec = acc.bind(corosio::endpoint(address, port)))
    {
        std::cerr << "bind failed: " << ec.message() << "\n";
        return 1;
    }
    if (auto ec = acc.listen())
    {
        std::cerr << "listen failed: " << ec.message() << "\n";
        return 1;
    }

    capy::run_async(ioc.get_executor())(accept_loop(ioc, acc, block_size));

    std::vector<std::thread> threads;
    threads.reserve(thread_count - 1);
    for (int i = 1; i < thread_count; ++i)
        threads.emplace_back([&ioc] { ioc.run(); });

    ioc.run();

    for (auto& t : threads)
        t.join();

    return 0;
}
