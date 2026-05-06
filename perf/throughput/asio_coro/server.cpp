//
// server.cpp (modern Boost.Asio coroutine-style benchmark)
//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Structurally mirrors perf/throughput/corosio/server.cpp so that the two libraries
// are compared on identical control flow.
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace asio = boost::asio;
using asio::ip::tcp;
using asio::awaitable;
using asio::co_spawn;
using asio::detached;
using asio::use_awaitable;

awaitable<void>
session(tcp::socket sock, std::size_t block_size)
{
    boost::system::error_code opt_ec;
    sock.set_option(tcp::no_delay(true), opt_ec);
    if (opt_ec)
        co_return;

    std::unique_ptr<char[]> buf(new char[block_size]);

    try
    {
        for (;;)
        {
            std::size_t n = co_await sock.async_read_some(
                asio::buffer(buf.get(), block_size), use_awaitable);
            co_await asio::async_write(
                sock, asio::buffer(buf.get(), n), use_awaitable);
        }
    }
    catch (std::exception&)
    {
    }
}

awaitable<void>
accept_loop(tcp::acceptor& acc, std::size_t block_size)
{
    for (;;)
    {
        tcp::socket peer = co_await acc.async_accept(use_awaitable);
        co_spawn(
            acc.get_executor(),
            session(std::move(peer), block_size),
            detached);
    }
}

int main(int argc, char* argv[])
{
    if (argc != 5)
    {
        std::cerr << "Usage: server <address> <port> <threads> <blocksize>\n";
        return 1;
    }

    auto address = asio::ip::make_address(argv[1]);
    std::uint16_t port = static_cast<std::uint16_t>(std::atoi(argv[2]));
    int thread_count = std::atoi(argv[3]);
    if (thread_count < 1)
        thread_count = 1;
    std::size_t block_size =
        static_cast<std::size_t>(std::atoi(argv[4]));

    asio::io_context ioc(thread_count);
    tcp::acceptor acc(ioc, tcp::endpoint(address, port));

    co_spawn(ioc, accept_loop(acc, block_size), detached);

    std::vector<std::thread> threads;
    threads.reserve(thread_count - 1);
    for (int i = 1; i < thread_count; ++i)
        threads.emplace_back([&ioc] { ioc.run(); });

    ioc.run();

    for (auto& t : threads)
        t.join();

    return 0;
}
