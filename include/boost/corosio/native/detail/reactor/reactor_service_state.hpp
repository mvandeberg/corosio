//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SERVICE_STATE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SERVICE_STATE_HPP

#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/native/detail/reactor/impl_ref.hpp>

#include <mutex>

namespace boost::corosio::detail {

/** Shared service state for reactor backends.

    Holds the scheduler reference, service mutex, and per-impl
    ownership tracking. Used by both socket and acceptor services.

    @tparam Scheduler The backend's scheduler type.
    @tparam Impl The backend's socket or acceptor impl type.
*/
template<class Scheduler, class Impl>
struct reactor_service_state
{
    /// Construct with a reference to the owning scheduler.
    explicit reactor_service_state(Scheduler& sched) noexcept : sched_(sched) {}

    ~reactor_service_state()
    {
        // After scheduler shutdown all pending ops have been drained,
        // so every impl that was kept alive only by op impl_refs has
        // already been returned to the freelist. Delete any impls
        // that were parked during shutdown (their service ref was
        // never decremented).
        while (auto* impl = shutdown_list_.pop_front())
            delete impl;
    }

    /// Reference to the owning scheduler.
    Scheduler& sched_;

    /// Protects impl_list_, freelist_, and shutdown_list_.
    std::mutex mutex_;

    /// All live impl objects for shutdown traversal.
    intrusive_list<Impl> impl_list_;

    /// Recycled impl objects available for reuse.
    impl_freelist<Impl> freelist_;

    /// Impls parked during shutdown, deleted in destructor.
    intrusive_list<Impl> shutdown_list_;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SERVICE_STATE_HPP
