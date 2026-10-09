//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL || BOOST_COROSIO_HAS_KQUEUE || \
    BOOST_COROSIO_HAS_SELECT

#include <boost/corosio/native/detail/reactor/reactor_descriptor_pool.hpp>

#include "test_suite.hpp"

namespace boost::corosio::detail {

struct reactor_descriptor_pool_test
{
    // The fix rests on this: a released state's storage stays a live
    // state, so a stale event naming it is never a use-after-free.
    void testReleasedStateIsReused()
    {
        reactor_descriptor_pool pool;
        auto* a = pool.acquire();
        pool.release(a);
        BOOST_TEST(pool.acquire() == a);
    }

    void testLiveStatesAreDistinct()
    {
        reactor_descriptor_pool pool;
        auto* a = pool.acquire();
        auto* b = pool.acquire();
        BOOST_TEST(a != b);
        pool.release(a);
        pool.release(b);
    }

    void run()
    {
        testReleasedStateIsReused();
        testLiveStatesAreDistinct();
    }
};

TEST_SUITE(
    reactor_descriptor_pool_test, "boost.corosio.reactor_descriptor_pool");

} // namespace boost::corosio::detail

#endif
