//
// client.cpp (corosio port of Christopher M. Kohlhoff's asio throughput
//             benchmark, mirroring perf/throughput/asio_callback/client.cpp)
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
#include <boost/corosio/native/native_tcp_socket.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/write.hpp>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace corosio = boost::corosio;
namespace capy = boost::capy;

using io_context_t = corosio::native_io_context<corosio::epoll>;
using tcp_socket_t = corosio::native_tcp_socket<corosio::epoll>;
template<class T = void>
using ttask = capy::task<T>;

struct session
{
    tcp_socket_t sock;
    std::size_t bytes_written = 0;
    std::size_t bytes_read = 0;

    explicit session(io_context_t& ioc) : sock(ioc) {}
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

ttask<>
pingpong(
    session* sess,
    corosio::endpoint ep,
    std::size_t block_size,
    std::atomic<bool>* stop_flag,
    stats* s)
{
    auto& sock = sess->sock;

    sock.open(corosio::tcp::v4());
    if (auto [ec] = co_await sock.connect(ep); ec)
        co_return;

    try
    {
        sock.set_option(corosio::native_socket_option::no_delay(true));
    }
    catch (std::exception&)
    {
        sock.close();
        co_return;
    }

    std::unique_ptr<char[]> write_buf(new char[block_size]);
    std::unique_ptr<char[]> read_buf(new char[block_size]);
    for (std::size_t i = 0; i < block_size; ++i)
        write_buf[i] = static_cast<char>(i % 128);

    // Prime the pipe with an initial write.
    {
        auto [wec, wn] = co_await capy::write(
            sock, capy::const_buffer(write_buf.get(), block_size));
        if (wec)
        {
            s->add(sess->bytes_written, sess->bytes_read);
            sock.close();
            co_return;
        }
        sess->bytes_written += wn;
    }

    while (!stop_flag->load(std::memory_order_relaxed))
    {
        auto [rec, rn] = co_await sock.read_some(
            capy::mutable_buffer(read_buf.get(), block_size));
        if (rec)
            break;
        sess->bytes_read += rn;

        auto [wec, wn] = co_await capy::write(
            sock, capy::const_buffer(read_buf.get(), rn));
        if (wec)
            break;
        sess->bytes_written += wn;
    }

    s->add(sess->bytes_written, sess->bytes_read);
    sock.shutdown(corosio::tcp_socket::shutdown_both);
    sock.close();
}

int main(int argc, char* argv[])
{
    if (argc != 7 && argc != 10)
    {
        std::cerr << "Usage: client <host> <port> <threads> <blocksize> "
                     "<sessions> <time> "
                     "[<inline_initial> <inline_max> <unassisted>]\n";
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
    unsigned inline_initial = (argc == 10) ? std::atoi(argv[7]) : 0;
    unsigned inline_max     = (argc == 10) ? std::atoi(argv[8]) : 1;
    unsigned unassisted     = (argc == 10) ? std::atoi(argv[9]) : 0;

    corosio::endpoint server_ep;
    {
        corosio::ipv4_address addr;
        if (auto ec = corosio::parse_ipv4_address(host, addr); !ec)
        {
            server_ep = corosio::endpoint(addr, port);
        }
        else
        {
            std::cerr << "Failed to parse address '" << host
                      << "' (client expects an IPv4 literal)\n";
            return 1;
        }
    }

    // Match asio's concurrency_hint=1 lockless single-thread mode: corosio
    // exposes this as io_context_options::single_threaded, which is NOT
    // enabled by the concurrency_hint constructor alone.
    corosio::io_context_options opts;
    opts.single_threaded = (thread_count == 1);
    if (argc == 10) {
        opts.inline_budget_initial = inline_initial;
        opts.inline_budget_max = inline_max;
        opts.unassisted_budget = unassisted;
    }
    io_context_t ioc(opts, static_cast<unsigned>(thread_count));
    stats s(timeout_seconds);
    std::atomic<bool> stop_flag{false};

    std::vector<std::unique_ptr<session>> sessions;
    sessions.reserve(session_count);
    for (std::size_t i = 0; i < session_count; ++i)
    {
        sessions.push_back(std::make_unique<session>(ioc));
        capy::run_async(ioc.get_executor())(pingpong(
            sessions.back().get(), server_ep, block_size, &stop_flag, &s));
    }

    // Flip the stop flag after the timeout. Sessions check it on each
    // iteration. We avoid sock.cancel() across threads: corosio forbids
    // concurrent operations on a socket, and in multi-threaded mode the
    // stopper coroutine may resume on a different thread than the one
    // driving the session.
    auto stopper = [&]() -> capy::task<> {
        corosio::timer t(ioc);
        t.expires_after(std::chrono::seconds(timeout_seconds));
        co_await t.wait();
        stop_flag.store(true, std::memory_order_relaxed);
    };
    capy::run_async(ioc.get_executor())(stopper());

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
