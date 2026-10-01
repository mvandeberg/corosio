//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/backend.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/win_random_access_handle.hpp>
#include <boost/corosio/win_stream_handle.hpp>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <tuple>

#include "../../win_test_handles.hpp"
#include "test_suite.hpp"

namespace boost::corosio {

// Destroying the context with a read still pending on a handle must
// drain the operation without resuming the coroutine.
struct iocp_handle_teardown_test
{
    void testDestroyWithPendingStreamHandleRead()
    {
        auto p       = test::make_pipe_pair();
        bool resumed = false;
        {
            io_context ioc(iocp);
            auto reader = [&]() -> capy::task<> {
                win_stream_handle h(ioc);
                BOOST_TEST(!h.assign(test::as_native(p.server.release())));
                char buf[16];
                std::ignore = co_await h.read_some(
                    capy::mutable_buffer(buf, sizeof(buf)));
                resumed = true;
            };
            capy::run_async(ioc.get_executor())(reader());
            std::ignore = ioc.run_one();
        }
        BOOST_TEST(!resumed);
    }

    void testDestroyWithPendingRandomAccessHandleRead()
    {
        auto p       = test::make_pipe_pair();
        bool resumed = false;
        {
            io_context ioc(iocp);
            auto reader = [&]() -> capy::task<> {
                win_random_access_handle h(ioc);
                BOOST_TEST(!h.assign(test::as_native(p.server.release())));
                char buf[16];
                std::ignore = co_await h.read_some_at(
                    0, capy::mutable_buffer(buf, sizeof(buf)));
                resumed = true;
            };
            capy::run_async(ioc.get_executor())(reader());
            std::ignore = ioc.run_one();
        }
        BOOST_TEST(!resumed);
    }

    void run()
    {
        testDestroyWithPendingStreamHandleRead();
        testDestroyWithPendingRandomAccessHandleRead();
    }
};

TEST_SUITE(iocp_handle_teardown_test, "boost.corosio.iocp_handle_teardown");

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_IOCP
