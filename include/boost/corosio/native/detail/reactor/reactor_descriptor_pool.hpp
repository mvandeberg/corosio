//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DESCRIPTOR_POOL_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DESCRIPTOR_POOL_HPP

#include <boost/corosio/native/detail/reactor/reactor_descriptor_state.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

/* Type-stable storage for reactor_descriptor_state.

   The reactor dereferences the descriptor_state pointers the kernel
   hands back after dropping the scheduler mutex, so another thread can
   destroy the owning object between the wait returning and the
   dispatch. A state embedded in that object died with it, and the
   dispatch wrote freed memory (#380).

   States therefore live as long as the scheduler. A released state is
   reused for a later descriptor, so a stale event lands on a live
   state: at worst a spurious readiness edge for the new owner, which
   read/write/connect ops absorb by re-probing and re-parking on
   EAGAIN. A parked wait(wait_type::error) does not re-probe, so a
   stale error edge can complete it. asio's epoll_reactor pools its
   descriptor_state for the same reason.
*/

namespace boost::corosio::detail {

/** Scheduler-lifetime pool of descriptor states.

    @par Thread Safety
    Safe to call concurrently.
*/
class reactor_descriptor_pool
{
    std::mutex mutex_;
    std::vector<std::unique_ptr<reactor_descriptor_state>> all_;
    // Capacity tracks all_, so release() never allocates.
    std::vector<reactor_descriptor_state*> free_;

public:
    reactor_descriptor_pool() = default;
    reactor_descriptor_pool(reactor_descriptor_pool const&)            = delete;
    reactor_descriptor_pool& operator=(reactor_descriptor_pool const&) = delete;

    /** Return an unowned state, reusing a released one if any.

        @throws std::bad_alloc
    */
    reactor_descriptor_state* acquire()
    {
        std::lock_guard lock(mutex_);
        if (!free_.empty())
        {
            auto* s = free_.back();
            free_.pop_back();
            return s;
        }
        if (free_.capacity() < all_.size() + 1)
            free_.reserve((std::max)(all_.size() + 1, 2 * free_.capacity()));
        all_.push_back(std::make_unique<reactor_descriptor_state>());
        return all_.back().get();
    }

    /// Make @a s available to a later acquire().
    void release(reactor_descriptor_state* s) noexcept
    {
        std::lock_guard lock(mutex_);
        free_.push_back(s);
    }
};

} // namespace boost::corosio::detail

#endif
