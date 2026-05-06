//
// client.cpp (modern Boost.Asio coroutine-style benchmark)
//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Structurally mirrors perf/throughput/corosio/client.cpp.
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
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#include <atomic>
#include <chrono>
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

struct session
{
    tcp::socket sock;
    std::size_t bytes_written = 0;
    std::size_t bytes_read = 0;

    explicit session(asio::io_context& ioc) : sock(ioc) {}
};

struct stats
{
    std::atomic<std::size_t> total_bytes_written{0};
    std::atomic<std::size_t> total_bytes_read{0};
    int timeout_seconds;

    explicit stats(int to) : timeout_seconds(to) {}

    void add(std::size_t written, std::size_t read)
    {
        total_bytes_written.fetch_add(written, std::memory_order_relaxed);
        total_bytes_read.fetch_add(read, std::memory_order_relaxed);
    }

    void print() const
    {
        auto bw = total_bytes_written.load(std::memory_order_relaxed);
        auto br = total_bytes_read.load(std::memory_order_relaxed);
        std::cout << bw << " total bytes written\n";
        std::cout << br << " total bytes read\n";
        std::cout << static_cast<double>(br) /
                         (static_cast<double>(timeout_seconds) * 1024 * 1024)
                  << " MiB/s throughput\n";
    }
};

awaitable<void>
pingpong(
    session* sess,
    tcp::endpoint ep,
    std::size_t block_size,
    std::atomic<bool>* stop_flag,
    stats* s)
{
    auto& sock = sess->sock;

    try
    {
        co_await sock.async_connect(ep, use_awaitable);
    }
    catch (std::exception&)
    {
        co_return;
    }

    boost::system::error_code opt_ec;
    sock.set_option(tcp::no_delay(true), opt_ec);
    if (opt_ec)
    {
        sock.close();
        co_return;
    }

    std::unique_ptr<char[]> write_buf(new char[block_size]);
    std::unique_ptr<char[]> read_buf(new char[block_size]);
    for (std::size_t i = 0; i < block_size; ++i)
        write_buf[i] = static_cast<char>(i % 128);

    try
    {
        // Prime the pipe with an initial write.
        sess->bytes_written += co_await asio::async_write(
            sock, asio::buffer(write_buf.get(), block_size), use_awaitable);

        while (!stop_flag->load(std::memory_order_relaxed))
        {
            std::size_t rn = co_await sock.async_read_some(
                asio::buffer(read_buf.get(), block_size), use_awaitable);
            sess->bytes_read += rn;

            sess->bytes_written += co_await asio::async_write(
                sock, asio::buffer(read_buf.get(), rn), use_awaitable);
        }
    }
    catch (std::exception&)
    {
    }

    s->add(sess->bytes_written, sess->bytes_read);
    boost::system::error_code ec;
    sock.shutdown(tcp::socket::shutdown_both, ec);
    sock.close(ec);
}

int main(int argc, char* argv[])
{
    if (argc != 7)
    {
        std::cerr << "Usage: client <host> <port> <threads> <blocksize> "
                     "<sessions> <time>\n";
        return 1;
    }

    const char* host = argv[1];
    std::uint16_t port = static_cast<std::uint16_t>(std::atoi(argv[2]));
    int thread_count = std::atoi(argv[3]);
    if (thread_count < 1)
        thread_count = 1;
    std::size_t block_size = static_cast<std::size_t>(std::atoi(argv[4]));
    std::size_t session_count = static_cast<std::size_t>(std::atoi(argv[5]));
    int timeout_seconds = std::atoi(argv[6]);

    boost::system::error_code addr_ec;
    auto address = asio::ip::make_address(host, addr_ec);
    if (addr_ec)
    {
        std::cerr << "Failed to parse address '" << host
                  << "' (client expects an IP literal)\n";
        return 1;
    }
    tcp::endpoint server_ep(address, port);

    asio::io_context ioc(thread_count);
    stats s(timeout_seconds);
    std::atomic<bool> stop_flag{false};

    std::vector<std::unique_ptr<session>> sessions;
    sessions.reserve(session_count);
    for (std::size_t i = 0; i < session_count; ++i)
    {
        sessions.push_back(std::make_unique<session>(ioc));
        co_spawn(
            ioc,
            pingpong(sessions.back().get(), server_ep, block_size,
                     &stop_flag, &s),
            detached);
    }

    // Stop timer: flip the flag; sessions observe it each iteration.
    auto stopper = [&]() -> awaitable<void> {
        asio::steady_timer t(co_await asio::this_coro::executor);
        t.expires_after(std::chrono::seconds(timeout_seconds));
        try
        {
            co_await t.async_wait(use_awaitable);
        }
        catch (std::exception&)
        {
        }
        stop_flag.store(true, std::memory_order_relaxed);
    };
    co_spawn(ioc, stopper(), detached);

    std::vector<std::thread> threads;
    threads.reserve(thread_count - 1);
    for (int i = 1; i < thread_count; ++i)
        threads.emplace_back([&ioc] { ioc.run(); });

    ioc.run();

    for (auto& t : threads)
        t.join();

    s.print();
    return 0;
}
