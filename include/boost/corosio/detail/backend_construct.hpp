//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

/* Defines the Backend_t<Config>::construct() member functions.

   These are out-of-class definitions for the backend tag templates
   declared in backend.hpp. They require the scheduler and service
   types to be complete, so this header pulls in the full scheduler
   and service headers for each platform.

   Included by native_io_context.hpp and io_context.cpp.
*/

#ifndef BOOST_COROSIO_DETAIL_BACKEND_CONSTRUCT_HPP
#define BOOST_COROSIO_DETAIL_BACKEND_CONSTRUCT_HPP

#include <boost/corosio/backend.hpp>
#include <boost/capy/ex/execution_context.hpp>

// -----------------------------------------------------------------
// epoll
// -----------------------------------------------------------------
#if BOOST_COROSIO_HAS_EPOLL

#include <boost/corosio/native/detail/epoll/epoll_scheduler.hpp>
#include <boost/corosio/native/detail/epoll/epoll_socket_service.hpp>
#include <boost/corosio/native/detail/epoll/epoll_acceptor_service.hpp>

namespace boost::corosio {

template<epoll_config Config>
detail::scheduler&
epoll_t<Config>::construct(
    capy::execution_context& ctx, unsigned concurrency_hint)
{
    auto& sched = ctx.make_service<detail::epoll_scheduler<Config>>(
        static_cast<int>(concurrency_hint));

    ctx.make_service<detail::epoll_socket_service>();
    ctx.make_service<detail::epoll_acceptor_service>();

    return sched;
}

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_EPOLL

// -----------------------------------------------------------------
// select
// -----------------------------------------------------------------
#if BOOST_COROSIO_HAS_SELECT

#include <boost/corosio/native/detail/select/select_scheduler.hpp>
#include <boost/corosio/native/detail/select/select_socket_service.hpp>
#include <boost/corosio/native/detail/select/select_acceptor_service.hpp>

namespace boost::corosio {

template<select_config Config>
detail::scheduler&
select_t<Config>::construct(
    capy::execution_context& ctx, unsigned concurrency_hint)
{
    auto& sched = ctx.make_service<detail::select_scheduler<Config>>(
        static_cast<int>(concurrency_hint));

    ctx.make_service<detail::select_socket_service>();
    ctx.make_service<detail::select_acceptor_service>();

    return sched;
}

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_SELECT

// -----------------------------------------------------------------
// kqueue
// -----------------------------------------------------------------
#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/native/detail/kqueue/kqueue_scheduler.hpp>
#include <boost/corosio/native/detail/kqueue/kqueue_socket_service.hpp>
#include <boost/corosio/native/detail/kqueue/kqueue_acceptor_service.hpp>

namespace boost::corosio {

template<kqueue_config Config>
detail::scheduler&
kqueue_t<Config>::construct(
    capy::execution_context& ctx, unsigned concurrency_hint)
{
    auto& sched = ctx.make_service<detail::kqueue_scheduler<Config>>(
        static_cast<int>(concurrency_hint));

    ctx.make_service<detail::kqueue_socket_service>();
    ctx.make_service<detail::kqueue_acceptor_service>();

    return sched;
}

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_KQUEUE

// -----------------------------------------------------------------
// IOCP
// -----------------------------------------------------------------
#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/native/detail/iocp/win_scheduler.hpp>
#include <boost/corosio/native/detail/iocp/win_acceptor_service.hpp>
#include <boost/corosio/native/detail/iocp/win_signals.hpp>

namespace boost::corosio {

template<iocp_config Config>
detail::scheduler&
iocp_t<Config>::construct(
    capy::execution_context& ctx, unsigned concurrency_hint)
{
    auto& sched = ctx.make_service<detail::win_scheduler<Config>>(
        static_cast<int>(concurrency_hint));

    auto& sockets = ctx.make_service<detail::win_sockets>();
    ctx.make_service<detail::win_acceptor_service>(sockets);
    ctx.make_service<detail::win_signals>();

    return sched;
}

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_DETAIL_BACKEND_CONSTRUCT_HPP
