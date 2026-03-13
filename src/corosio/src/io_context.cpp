//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/io_context.hpp>
#include <boost/corosio/detail/backend_construct.hpp>
#include <boost/corosio/detail/timer_service.hpp>
#include <boost/corosio/native/native_scheduler.hpp>

#include <thread>

namespace boost::corosio {

// Non-template helpers called by the default-config specializations
// in backend.hpp. These are regular functions with BOOST_COROSIO_DECL,
// so they export correctly on all compilers (including Clang with
// -fvisibility=hidden).
namespace detail {

#if BOOST_COROSIO_HAS_EPOLL
scheduler&
construct_default_epoll(capy::execution_context& ctx, unsigned concurrency_hint)
{
    auto& sched = ctx.make_service<epoll_scheduler<epoll_config{}>>(
        static_cast<int>(concurrency_hint));
    ctx.make_service<epoll_socket_service>();
    ctx.make_service<epoll_acceptor_service>();
    return sched;
}
#endif

#if BOOST_COROSIO_HAS_SELECT
scheduler&
construct_default_select(capy::execution_context& ctx, unsigned concurrency_hint)
{
    auto& sched = ctx.make_service<select_scheduler<select_config{}>>(
        static_cast<int>(concurrency_hint));
    ctx.make_service<select_socket_service>();
    ctx.make_service<select_acceptor_service>();
    return sched;
}
#endif

#if BOOST_COROSIO_HAS_KQUEUE
scheduler&
construct_default_kqueue(capy::execution_context& ctx, unsigned concurrency_hint)
{
    auto& sched = ctx.make_service<kqueue_scheduler<kqueue_config{}>>(
        static_cast<int>(concurrency_hint));
    ctx.make_service<kqueue_socket_service>();
    ctx.make_service<kqueue_acceptor_service>();
    return sched;
}
#endif

#if BOOST_COROSIO_HAS_IOCP
scheduler&
construct_default_iocp(capy::execution_context& ctx, unsigned concurrency_hint)
{
    auto& sched = ctx.make_service<win_scheduler<iocp_config{}>>(
        static_cast<int>(concurrency_hint));
    auto& sockets = ctx.make_service<win_sockets>();
    ctx.make_service<win_acceptor_service>(sockets);
    ctx.make_service<win_signals>();
    return sched;
}
#endif

} // namespace detail

namespace {

void apply_options(detail::scheduler* sched, io_context_options const& opts)
{
    auto* ns = static_cast<detail::native_scheduler*>(sched);
    ns->recycle_post_nodes_rt_ = opts.recycle_post_nodes;

    if (ns->timer_svc_)
    {
        ns->timer_svc_->recycle_nodes_      = opts.recycle_timer_nodes;
        ns->timer_svc_->max_recycled_nodes_ = opts.max_recycled_nodes;
    }
}

} // anonymous namespace

io_context::io_context() : io_context(std::thread::hardware_concurrency()) {}

io_context::io_context(unsigned concurrency_hint)
    : capy::execution_context(this)
    , sched_(nullptr)
{
#if BOOST_COROSIO_HAS_IOCP
    sched_ = &iocp_t<>::construct(*this, concurrency_hint);
#elif BOOST_COROSIO_HAS_EPOLL
    sched_ = &epoll_t<>::construct(*this, concurrency_hint);
#elif BOOST_COROSIO_HAS_KQUEUE
    sched_ = &kqueue_t<>::construct(*this, concurrency_hint);
#elif BOOST_COROSIO_HAS_SELECT
    sched_ = &select_t<>::construct(*this, concurrency_hint);
#endif
}

io_context::io_context(io_context_options opts)
    : io_context(std::thread::hardware_concurrency(), opts)
{
}

io_context::io_context(unsigned concurrency_hint, io_context_options opts)
    : capy::execution_context(this)
    , sched_(nullptr)
{
#if BOOST_COROSIO_HAS_IOCP
    sched_ = &iocp_t<>::construct(*this, concurrency_hint);
#elif BOOST_COROSIO_HAS_EPOLL
    sched_ = &epoll_t<>::construct(*this, concurrency_hint);
#elif BOOST_COROSIO_HAS_KQUEUE
    sched_ = &kqueue_t<>::construct(*this, concurrency_hint);
#elif BOOST_COROSIO_HAS_SELECT
    sched_ = &select_t<>::construct(*this, concurrency_hint);
#endif
    apply_options(sched_, opts);
}

io_context::~io_context()
{
    shutdown();
    destroy();
}

} // namespace boost::corosio
