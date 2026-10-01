//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/native/detail/kqueue/kqueue_scheduler.hpp>

#include <sys/event.h>
#include <unistd.h>

#include "test_suite.hpp"

namespace boost::corosio {

struct kqueue_register_rollback_test
{
    static bool has_filter(int kq, int fd, short filter)
    {
        // EV_DELETE of a filter that is not registered fails with ENOENT.
        struct kevent ch;
        EV_SET(
            &ch, static_cast<uintptr_t>(fd), filter, EV_DELETE, 0, 0, nullptr);
        return ::kevent(kq, &ch, 1, nullptr, 0, nullptr) == 0;
    }

    void testBothFiltersAdded()
    {
        int kq = ::kqueue();
        int fds[2];
        BOOST_TEST_EQ(::pipe(fds), 0);
        auto r = detail::kqueue_add_rw(kq, fds[0], nullptr);
        BOOST_TEST_EQ(r.read_err, 0);
        BOOST_TEST_EQ(r.write_err, 0);
        BOOST_TEST(has_filter(kq, fds[0], EVFILT_READ));
        ::close(fds[0]);
        ::close(fds[1]);
        ::close(kq);
    }

    void testWriteRefusalLeavesNothingRegistered()
    {
        // A FreeBSD pipe write end whose reader is gone refuses
        // EVFILT_WRITE with EPIPE while accepting EVFILT_READ.
        int kq = ::kqueue();
        int fds[2];
        BOOST_TEST_EQ(::pipe(fds), 0);
        ::close(fds[0]);
        auto r = detail::kqueue_add_rw(kq, fds[1], nullptr);
        if (r.write_err == 0)
        {
            // This kernel accepts it; the scenario is not reproducible.
            ::close(fds[1]);
            ::close(kq);
            return;
        }
        BOOST_TEST(!has_filter(kq, fds[1], EVFILT_READ));
        ::close(fds[1]);
        ::close(kq);
    }

    void run()
    {
        testBothFiltersAdded();
        testWriteRefusalLeavesNothingRegistered();
    }
};

TEST_SUITE(
    kqueue_register_rollback_test,
    "boost.corosio.native.kqueue.register_rollback");

} // namespace boost::corosio

#endif
