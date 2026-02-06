//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/kqueue_context.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include "src/detail/kqueue/scheduler.hpp"
#include "src/detail/kqueue/sockets.hpp"
#include "src/detail/kqueue/acceptors.hpp"

#include <thread>

namespace boost::corosio {

kqueue_context::
kqueue_context()
    : kqueue_context(std::thread::hardware_concurrency())
{
}

kqueue_context::
kqueue_context(
    unsigned concurrency_hint)
{
    sched_ = &make_service<detail::kqueue_scheduler>(
        static_cast<int>(concurrency_hint));

    // Install socket/acceptor services.
    // These use socket_service and acceptor_service as key_type,
    // enabling runtime polymorphism.
    make_service<detail::kqueue_socket_service>();
    make_service<detail::kqueue_acceptor_service>();
}

kqueue_context::
~kqueue_context()
{
    shutdown();
    destroy();
}

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_KQUEUE
