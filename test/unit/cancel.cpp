//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Test that header file is self-contained.
#include <boost/corosio/cancel.hpp>

#include <boost/corosio/timer.hpp>
#include <boost/corosio/test/socket_pair.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/cond.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>

#include <chrono>
#include <stop_token>

#include "context.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

template<auto Backend>
struct cancel_test
{
    void testTimeoutFires()
    {
        io_context ioc(Backend);
        timer inner_timer(ioc);
        timer timeout_timer(ioc);

        bool completed = false;
        std::error_code result_ec;

        inner_timer.expires_after(std::chrono::seconds(60));

        auto task = [&]() -> capy::task<> {
            auto [ec] = co_await cancel_after(
                inner_timer.wait(), timeout_timer,
                std::chrono::milliseconds(10));
            result_ec = ec;
            completed = true;
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void testInnerCompletesFirst()
    {
        io_context ioc(Backend);
        timer inner_timer(ioc);
        timer timeout_timer(ioc);

        bool completed = false;
        std::error_code result_ec;

        inner_timer.expires_after(std::chrono::milliseconds(10));

        auto task = [&]() -> capy::task<> {
            auto [ec] = co_await cancel_after(
                inner_timer.wait(), timeout_timer, std::chrono::seconds(1));
            result_ec = ec;
            completed = true;
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(!result_ec);
    }

    void testZeroTimeout()
    {
        io_context ioc(Backend);
        timer inner_timer(ioc);
        timer timeout_timer(ioc);

        bool completed = false;
        std::error_code result_ec;

        inner_timer.expires_after(std::chrono::seconds(60));

        auto task = [&]() -> capy::task<> {
            auto [ec] = co_await cancel_after(
                inner_timer.wait(), timeout_timer,
                std::chrono::milliseconds(0));
            result_ec = ec;
            completed = true;
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void testParentCancellation()
    {
        io_context ioc(Backend);
        timer inner_timer(ioc);
        timer timeout_timer(ioc);
        timer delay(ioc);

        std::stop_source stop_src;
        bool completed = false;
        std::error_code result_ec;

        inner_timer.expires_after(std::chrono::seconds(60));

        auto task = [&]() -> capy::task<> {
            auto [ec] = co_await cancel_after(
                inner_timer.wait(), timeout_timer, std::chrono::seconds(60));
            result_ec = ec;
            completed = true;
        };

        auto canceller = [&]() -> capy::task<> {
            delay.expires_after(std::chrono::milliseconds(10));
            (void)co_await delay.wait();
            stop_src.request_stop();
        };

        capy::run_async(ioc.get_executor(), stop_src.get_token())(task());
        capy::run_async(ioc.get_executor())(canceller());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void testAlreadyExpiredDeadline()
    {
        io_context ioc(Backend);
        timer inner_timer(ioc);
        timer timeout_timer(ioc);

        bool completed = false;
        std::error_code result_ec;

        inner_timer.expires_after(std::chrono::seconds(60));

        auto task = [&]() -> capy::task<> {
            auto [ec] = co_await cancel_at(
                inner_timer.wait(), timeout_timer,
                timer::clock_type::now() - std::chrono::seconds(1));
            result_ec = ec;
            completed = true;
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void testCancelAt()
    {
        io_context ioc(Backend);
        timer inner_timer(ioc);
        timer timeout_timer(ioc);

        bool completed = false;
        std::error_code result_ec;

        inner_timer.expires_after(std::chrono::seconds(60));
        auto deadline =
            timer::clock_type::now() + std::chrono::milliseconds(10);

        auto task = [&]() -> capy::task<> {
            auto [ec] =
                co_await cancel_at(inner_timer.wait(), timeout_timer, deadline);
            result_ec = ec;
            completed = true;
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void testTimerReuse()
    {
        io_context ioc(Backend);
        timer inner_timer(ioc);
        timer timeout_timer(ioc);

        int completed = 0;

        auto task = [&]() -> capy::task<> {
            // First: inner completes before timeout
            inner_timer.expires_after(std::chrono::milliseconds(10));
            auto [ec1] = co_await cancel_after(
                inner_timer.wait(), timeout_timer, std::chrono::seconds(1));
            BOOST_TEST(!ec1);
            ++completed;

            // Second: timeout fires
            inner_timer.expires_after(std::chrono::seconds(60));
            auto [ec2] = co_await cancel_after(
                inner_timer.wait(), timeout_timer,
                std::chrono::milliseconds(10));
            BOOST_TEST(ec2 == capy::cond::canceled);
            ++completed;

            // Third: inner completes again
            inner_timer.expires_after(std::chrono::milliseconds(10));
            auto [ec3] = co_await cancel_after(
                inner_timer.wait(), timeout_timer, std::chrono::seconds(1));
            BOOST_TEST(!ec3);
            ++completed;
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        BOOST_TEST_EQ(completed, 3);
    }

    // -- Convenience overloads (no user-supplied timer) --

    void testConvenienceTimeoutFires()
    {
        io_context ioc(Backend);
        timer inner_timer(ioc);

        bool completed = false;
        std::error_code result_ec;

        inner_timer.expires_after(std::chrono::seconds(60));

        auto task = [&]() -> capy::task<> {
            auto [ec] = co_await cancel_after(
                inner_timer.wait(), std::chrono::milliseconds(10));
            result_ec = ec;
            completed = true;
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void testConvenienceInnerCompletesFirst()
    {
        io_context ioc(Backend);
        timer inner_timer(ioc);

        bool completed = false;
        std::error_code result_ec;

        inner_timer.expires_after(std::chrono::milliseconds(10));

        auto task = [&]() -> capy::task<> {
            auto [ec] = co_await cancel_after(
                inner_timer.wait(), std::chrono::seconds(1));
            result_ec = ec;
            completed = true;
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(!result_ec);
    }

    void testConvenienceCancelAt()
    {
        io_context ioc(Backend);
        timer inner_timer(ioc);

        bool completed = false;
        std::error_code result_ec;

        inner_timer.expires_after(std::chrono::seconds(60));
        auto deadline =
            timer::clock_type::now() + std::chrono::milliseconds(10);

        auto task = [&]() -> capy::task<> {
            auto [ec] = co_await cancel_at(inner_timer.wait(), deadline);
            result_ec = ec;
            completed = true;
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    // -- cancel_after wrapping socket read/write --
    //
    // Regression tests for a bug where cancel_at_awaitable::await_resume
    // called stop_src_.request_stop() before inner_.await_resume(),
    // causing bytes_op_base-derived awaitables (read_some, write_some)
    // to observe a stale stop_requested() on a token they had retained
    // across the suspend/resume split. The fix clears token_ at the end
    // of bytes_op_base/void_op_base::await_suspend.

    void testReadSomeCompletesFirst()
    {
        io_context ioc(Backend);
        auto [a, b] = test::make_socket_pair(ioc);
        timer timeout_timer(ioc);
        timer write_delay(ioc);

        bool completed = false;
        std::error_code result_ec;
        std::size_t result_n = 0;
        char rbuf[16] = {};
        char const wbuf[] = "hello";

        auto reader = [&]() -> capy::task<> {
            auto [ec, n] = co_await cancel_after(
                a.read_some(capy::mutable_buffer(rbuf, sizeof(rbuf))),
                timeout_timer, std::chrono::seconds(5));
            result_ec = ec;
            result_n  = n;
            completed = true;
        };

        auto writer = [&]() -> capy::task<> {
            write_delay.expires_after(std::chrono::milliseconds(10));
            (void)co_await write_delay.wait();
            (void)co_await capy::write(
                b, capy::const_buffer(wbuf, sizeof(wbuf) - 1));
        };

        capy::run_async(ioc.get_executor())(reader());
        capy::run_async(ioc.get_executor())(writer());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(!result_ec);
        BOOST_TEST_EQ(result_n, std::size_t(sizeof(wbuf) - 1));
    }

    void testReadSomeTimesOut()
    {
        io_context ioc(Backend);
        auto [a, b] = test::make_socket_pair(ioc);
        (void)b;
        timer timeout_timer(ioc);

        bool completed = false;
        std::error_code result_ec;
        char rbuf[16] = {};

        auto task = [&]() -> capy::task<> {
            auto [ec, n] = co_await cancel_after(
                a.read_some(capy::mutable_buffer(rbuf, sizeof(rbuf))),
                timeout_timer, std::chrono::milliseconds(10));
            (void)n;
            result_ec = ec;
            completed = true;
        };
        capy::run_async(ioc.get_executor())(task());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(result_ec == capy::cond::canceled);
    }

    void testWriteCompletesFirst()
    {
        // Wraps capy::write (composed op) — exercises cancellation
        // propagation into the underlying write_some awaitable.
        io_context ioc(Backend);
        auto [a, b] = test::make_socket_pair(ioc);
        timer timeout_timer(ioc);

        bool completed = false;
        std::error_code result_ec;
        std::size_t result_n = 0;
        char const wbuf[] = "hello";
        char rbuf[16] = {};

        auto writer = [&]() -> capy::task<> {
            auto [ec, n] = co_await cancel_after(
                capy::write(a, capy::const_buffer(wbuf, sizeof(wbuf) - 1)),
                timeout_timer, std::chrono::seconds(5));
            result_ec = ec;
            result_n  = n;
            completed = true;
        };

        auto drainer = [&]() -> capy::task<> {
            (void)co_await b.read_some(
                capy::mutable_buffer(rbuf, sizeof(rbuf)));
        };

        capy::run_async(ioc.get_executor())(writer());
        capy::run_async(ioc.get_executor())(drainer());

        ioc.run();
        BOOST_TEST(completed);
        BOOST_TEST(!result_ec);
        BOOST_TEST_EQ(result_n, std::size_t(sizeof(wbuf) - 1));
    }

    void testReadSomeTimerReuse()
    {
        // Reusing one timer across consecutive successful reads, the
        // pattern used by the async-bench hello-timeout example.
        io_context ioc(Backend);
        auto [a, b] = test::make_socket_pair(ioc);
        timer timeout_timer(ioc);
        timer write_delay(ioc);

        int successes = 0;
        char rbuf[16] = {};

        auto reader = [&]() -> capy::task<> {
            for (int i = 0; i < 3; ++i)
            {
                auto [ec, n] = co_await cancel_after(
                    a.read_some(capy::mutable_buffer(rbuf, sizeof(rbuf))),
                    timeout_timer, std::chrono::seconds(5));
                if (!ec && n > 0)
                    ++successes;
                else
                    break;
            }
        };

        auto writer = [&]() -> capy::task<> {
            char const msg[] = "x";
            for (int i = 0; i < 3; ++i)
            {
                write_delay.expires_after(std::chrono::milliseconds(5));
                (void)co_await write_delay.wait();
                (void)co_await capy::write(
                    b, capy::const_buffer(msg, sizeof(msg) - 1));
            }
        };

        capy::run_async(ioc.get_executor())(reader());
        capy::run_async(ioc.get_executor())(writer());

        ioc.run();
        BOOST_TEST_EQ(successes, 3);
    }

    void run()
    {
        testTimeoutFires();
        testInnerCompletesFirst();
        testZeroTimeout();
        testParentCancellation();
        testAlreadyExpiredDeadline();
        testCancelAt();
        testTimerReuse();
        testConvenienceTimeoutFires();
        testConvenienceInnerCompletesFirst();
        testConvenienceCancelAt();
        testReadSomeCompletesFirst();
        testReadSomeTimesOut();
        testWriteCompletesFirst();
        testReadSomeTimerReuse();
    }
};

COROSIO_BACKEND_TESTS(cancel_test, "boost.corosio.cancel")

} // namespace boost::corosio
