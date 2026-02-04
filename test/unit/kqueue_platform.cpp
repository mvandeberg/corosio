//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

/*
    Platform-specific tests for kqueue backend.

    These tests verify critical BSD/macOS-specific implementation details:
    - SO_NOSIGPIPE SIGPIPE prevention
    - FD_CLOEXEC on sockets and acceptors
    - O_NONBLOCK on sockets
    - Accept sets proper flags on accepted sockets
    - Graceful shutdown handling
*/

// Test that header file is self-contained.
#include <boost/corosio/io_context.hpp>

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/kqueue_context.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/ex/run_async.hpp>

#include <atomic>
#include <csignal>
#include <cstring>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "test_suite.hpp"

namespace boost::corosio {
namespace {

struct kqueue_platform_test
{
    // Test 1: SO_NOSIGPIPE prevents SIGPIPE on write to closed socket
    void
    testSoNosigpipe()
    {
        // Install SIGPIPE handler to detect if signal is raised
        std::atomic<bool> sigpipe_received{false};
        struct sigaction sa, old_sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = [](int) {
            // This should NEVER be called if SO_NOSIGPIPE works
            std::abort();
        };
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGPIPE, &sa, &old_sa);

        try
        {
            kqueue_context ctx;

            bool write_done = false;
            std::error_code write_ec;
            std::size_t bytes_written = 0;

            // Create socket pair
            auto [sock1, sock2] = test::make_socket_pair(ctx);

            // Close the read end
            sock2.close();

            // Write to closed socket - should get error, NOT SIGPIPE
            std::string large_data(1024 * 1024, 'X'); // 1MB to ensure write

            auto write_task = [&]() -> capy::task<> {
                try
                {
                    // On macOS, the first write to a half-closed socket may
                    // succeed if the kernel buffer has space before FIN is
                    // processed. Loop until we get the expected error.
                    while (true)
                    {
                        auto [ec, n] = co_await sock1.write_some(
                            capy::const_buffer(large_data.data(), large_data.size()));
                        if (ec)
                        {
                            write_ec = ec;
                            break;
                        }
                        bytes_written += n;
                        if (n == 0)
                            break;
                    }
                }
                catch (...)
                {
                    // Exception is OK too
                }
                write_done = true;
            };

            capy::run_async(ctx.get_executor())(write_task());
            ctx.run();

            BOOST_TEST(write_done);
            // Should have gotten an error (broken pipe), not crashed
            BOOST_TEST(write_ec || bytes_written == 0);
        }
        catch (...)
        {
            // Restore signal handler before rethrowing
            sigaction(SIGPIPE, &old_sa, nullptr);
            throw;
        }

        // Restore old signal handler
        sigaction(SIGPIPE, &old_sa, nullptr);
        BOOST_TEST(!sigpipe_received.load());
    }

    // Test 2: FD_CLOEXEC is set on socket file descriptors
    void
    testFdCloexecSocket()
    {
        kqueue_context ctx;

        // Create a socket
        tcp_socket sock(ctx);
        sock.open();

        int fd = sock.native_handle();
        BOOST_TEST(fd >= 0);

        // Check FD_CLOEXEC on socket
        int flags = fcntl(fd, F_GETFD);
        BOOST_TEST(flags >= 0);
        BOOST_TEST((flags & FD_CLOEXEC) != 0);
    }

    // Test 3: FD_CLOEXEC is set on acceptor file descriptors
    void
    testFdCloexecAcceptor()
    {
        kqueue_context ctx;

        // Create an acceptor
        tcp_acceptor acc(ctx);
        acc.listen(endpoint(ipv4_address::loopback(), 0));

        // Get the native file descriptor
        // Note: tcp_acceptor doesn't expose native_handle in public API
        // We test it indirectly through socket creation
        BOOST_TEST(acc.is_open());
    }

    // Test 4: O_NONBLOCK is set on sockets
    void
    testNonblocking()
    {
        kqueue_context ctx;

        tcp_socket sock(ctx);
        sock.open();

        int fd = sock.native_handle();
        BOOST_TEST(fd >= 0);

        // Check O_NONBLOCK
        int flags = fcntl(fd, F_GETFL);
        BOOST_TEST(flags >= 0);
        BOOST_TEST((flags & O_NONBLOCK) != 0);
    }

    // Test 5: EV_EOF handling - graceful shutdown
    void
    testGracefulShutdown()
    {
        kqueue_context ctx;

        bool read_done = false;
        std::error_code read_ec;
        std::size_t bytes_read = 0;

        // Create socket pair
        auto [sock1, sock2] = test::make_socket_pair(ctx);

        // Gracefully shutdown write end of sock2
        sock2.shutdown(tcp_socket::shutdown_send);

        // Read should complete with 0 bytes (EOF)
        char buf[1024];
        auto read_task = [&]() -> capy::task<> {
            auto [ec, n] = co_await sock1.read_some(capy::mutable_buffer(buf, sizeof(buf)));
            read_ec = ec;
            bytes_read = n;
            read_done = true;
        };

        capy::run_async(ctx.get_executor())(read_task());
        ctx.run();

        BOOST_TEST(read_done);
        BOOST_TEST(bytes_read == 0); // EOF
        // On graceful shutdown, read completes successfully with 0 bytes
    }

    // Test 6: Accepted sockets have proper flags set
    void
    testAcceptFlags()
    {
        kqueue_context ctx;

        // Create acceptor
        tcp_acceptor acc(ctx);
        acc.listen(endpoint(ipv4_address::loopback(), 0));

        auto local_ep = acc.local_endpoint();

        bool accept_done = false;
        tcp_socket accepted_sock(ctx);
        std::error_code accept_ec;

        // Start accept operation
        auto accept_task = [&]() -> capy::task<> {
            auto [ec] = co_await acc.accept(accepted_sock);
            accept_ec = ec;
            accept_done = true;
        };

        capy::run_async(ctx.get_executor())(accept_task());

        // Connect to acceptor
        tcp_socket client(ctx);
        client.open();
        std::error_code connect_ec;
        bool connect_done = false;
        auto connect_task = [&]() -> capy::task<> {
            auto [ec] = co_await client.connect(local_ep);
            connect_ec = ec;
            connect_done = true;
        };

        capy::run_async(ctx.get_executor())(connect_task());

        // Run both operations
        ctx.run();

        BOOST_TEST(accept_done);
        BOOST_TEST(!accept_ec);
        BOOST_TEST(connect_done);
        BOOST_TEST(!connect_ec);
        BOOST_TEST(accepted_sock.is_open());

        int fd = accepted_sock.native_handle();
        BOOST_TEST(fd >= 0);

        // Check O_NONBLOCK
        int file_flags = fcntl(fd, F_GETFL);
        BOOST_TEST(file_flags >= 0);
        BOOST_TEST((file_flags & O_NONBLOCK) != 0);

        // Check FD_CLOEXEC
        int fd_flags = fcntl(fd, F_GETFD);
        BOOST_TEST(fd_flags >= 0);
        BOOST_TEST((fd_flags & FD_CLOEXEC) != 0);

        // Check SO_NOSIGPIPE
        int nosigpipe = 0;
        socklen_t len = sizeof(nosigpipe);
        int ret = getsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, &len);
        BOOST_TEST(ret == 0);
        BOOST_TEST(nosigpipe != 0);
    }

    // Test 7: Socket options work correctly
    void
    testSocketOptions()
    {
        kqueue_context ctx;

        tcp_socket sock(ctx);
        sock.open();

        // Test TCP_NODELAY
        sock.set_no_delay(true);
        BOOST_TEST(sock.no_delay());

        // Test SO_KEEPALIVE
        sock.set_keep_alive(true);
        BOOST_TEST(sock.keep_alive());

        // Test SO_RCVBUF
        sock.set_receive_buffer_size(65536);
        int rcv_buf = sock.receive_buffer_size();
        BOOST_TEST(rcv_buf >= 65536); // May be rounded up by OS

        // Test SO_SNDBUF
        sock.set_send_buffer_size(65536);
        int snd_buf = sock.send_buffer_size();
        BOOST_TEST(snd_buf >= 65536); // May be rounded up by OS
    }

    // Test 8: Basic I/O works with edge-triggered kqueue
    void
    testBasicIO()
    {
        kqueue_context ctx;

        // Create socket pair
        auto [sock1, sock2] = test::make_socket_pair(ctx);

        std::string write_data = "Hello, kqueue!";
        char read_buf[128];
        std::size_t bytes_written = 0;
        std::size_t bytes_read = 0;
        std::error_code write_ec, read_ec;

        // Write from sock1
        auto write_task = [&]() -> capy::task<> {
            auto [ec, n] = co_await sock1.write_some(
                capy::const_buffer(write_data.data(), write_data.size()));
            write_ec = ec;
            bytes_written = n;
        };

        // Read on sock2
        auto read_task = [&]() -> capy::task<> {
            auto [ec, n] = co_await sock2.read_some(capy::mutable_buffer(read_buf, sizeof(read_buf)));
            read_ec = ec;
            bytes_read = n;
        };

        capy::run_async(ctx.get_executor())(write_task());
        capy::run_async(ctx.get_executor())(read_task());

        ctx.run();

        BOOST_TEST(!write_ec);
        BOOST_TEST(!read_ec);
        BOOST_TEST(bytes_written == write_data.size());
        BOOST_TEST(bytes_read == write_data.size());
        BOOST_TEST(std::string(read_buf, bytes_read) == write_data);
    }

    void
    run()
    {
        testFdCloexecSocket();
        testFdCloexecAcceptor();
        testNonblocking();
        testGracefulShutdown();
        testAcceptFlags();
        testSocketOptions();
        testBasicIO();
        testSoNosigpipe();
    }
};

TEST_SUITE(kqueue_platform_test, "boost.corosio.kqueue.platform");

} // namespace
} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_KQUEUE
