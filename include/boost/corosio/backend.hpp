//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_BACKEND_HPP
#define BOOST_COROSIO_BACKEND_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/platform.hpp>

namespace boost::capy {
class execution_context;
} // namespace boost::capy

namespace boost::corosio {

namespace detail {
struct scheduler;

// Non-template helpers for default-config construct().
// Avoids Clang's refusal to export explicit template instantiation
// symbols under -fvisibility=hidden. Each is defined in io_context.cpp.
#if BOOST_COROSIO_HAS_EPOLL
BOOST_COROSIO_DECL scheduler&
construct_default_epoll(capy::execution_context&, unsigned);
#endif

#if BOOST_COROSIO_HAS_SELECT
BOOST_COROSIO_DECL scheduler&
construct_default_select(capy::execution_context&, unsigned);
#endif

#if BOOST_COROSIO_HAS_KQUEUE
BOOST_COROSIO_DECL scheduler&
construct_default_kqueue(capy::execution_context&, unsigned);
#endif

#if BOOST_COROSIO_HAS_IOCP
BOOST_COROSIO_DECL scheduler&
construct_default_iocp(capy::execution_context&, unsigned);
#endif

} // namespace detail

// -----------------------------------------------------------------
// epoll
// -----------------------------------------------------------------
#if BOOST_COROSIO_HAS_EPOLL

/** Compile-time configuration for the epoll backend.

    All fields have defaults matching the library's current
    hardcoded values. Pass a customized instance as a template
    argument to @ref epoll_t to tune behavior at compile time.

    This is a structural type suitable for use as an NTTP.
*/
struct epoll_config
{
    /// Max events harvested per epoll_wait call.
    unsigned max_events_per_poll = 128;

    /// Starting inline completion budget per handler chain.
    unsigned inline_budget_initial = 2;

    /// Hard ceiling on adaptive inline budget ramp-up.
    unsigned inline_budget_max = 16;

    /// Inline budget when no other thread is in the reactor.
    unsigned unassisted_budget = 4;

    /// Recycle post_handler nodes via a per-thread free list.
    bool recycle_post_nodes = true;
};

namespace detail {

class epoll_socket;
class epoll_socket_service;
class epoll_acceptor;
class epoll_acceptor_service;
template<epoll_config>
class epoll_scheduler;

class posix_signal;
class posix_signal_service;
class posix_resolver;
class posix_resolver_service;

} // namespace detail

/** Backend tag for the Linux epoll I/O multiplexer.

    @tparam Config Compile-time configuration. Defaults produce
        identical behavior to the library's hardcoded constants.

    @par Example
    @code
    // Default config
    io_context ioc(corosio::epoll);

    // Custom config
    constexpr epoll_config cfg{.max_events_per_poll = 256};
    io_context ioc(epoll_t<cfg>{});
    @endcode
*/
template<epoll_config Config = epoll_config{}>
struct epoll_t
{
    static constexpr epoll_config config = Config;

    using scheduler_type        = detail::epoll_scheduler<Config>;
    using socket_type           = detail::epoll_socket;
    using socket_service_type   = detail::epoll_socket_service;
    using acceptor_type         = detail::epoll_acceptor;
    using acceptor_service_type = detail::epoll_acceptor_service;

    using signal_type           = detail::posix_signal;
    using signal_service_type   = detail::posix_signal_service;
    using resolver_type         = detail::posix_resolver;
    using resolver_service_type = detail::posix_resolver_service;

    /// Create the scheduler and services for this backend.
    static detail::scheduler&
    construct(capy::execution_context& ctx, unsigned concurrency_hint);
};

/// Tag value for selecting the epoll backend with default config.
inline constexpr epoll_t<> epoll{};

// Full specialization for default config delegates to an exported
// non-template helper, sidestepping Clang's template visibility issue.
template<>
inline detail::scheduler&
epoll_t<>::construct(capy::execution_context& ctx, unsigned concurrency_hint)
{
    return detail::construct_default_epoll(ctx, concurrency_hint);
}

#endif // BOOST_COROSIO_HAS_EPOLL

// -----------------------------------------------------------------
// select
// -----------------------------------------------------------------
#if BOOST_COROSIO_HAS_SELECT

/** Compile-time configuration for the select backend.

    This is a structural type suitable for use as an NTTP.
*/
struct select_config
{
    /// Recycle post_handler nodes via a free list.
    bool recycle_post_nodes = true;
};

namespace detail {

class select_socket;
class select_socket_service;
class select_acceptor;
class select_acceptor_service;
class select_scheduler_core;
template<select_config>
class select_scheduler;

class posix_signal;
class posix_signal_service;
class posix_resolver;
class posix_resolver_service;

} // namespace detail

/** Backend tag for the portable select() I/O multiplexer.

    @tparam Config Compile-time configuration. Defaults produce
        identical behavior to the library's hardcoded constants.
*/
template<select_config Config = select_config{}>
struct select_t
{
    static constexpr select_config config = Config;

    using scheduler_type        = detail::select_scheduler<Config>;
    using socket_type           = detail::select_socket;
    using socket_service_type   = detail::select_socket_service;
    using acceptor_type         = detail::select_acceptor;
    using acceptor_service_type = detail::select_acceptor_service;

    using signal_type           = detail::posix_signal;
    using signal_service_type   = detail::posix_signal_service;
    using resolver_type         = detail::posix_resolver;
    using resolver_service_type = detail::posix_resolver_service;

    /// Create the scheduler and services for this backend.
    static detail::scheduler&
    construct(capy::execution_context& ctx, unsigned concurrency_hint);
};

/// Tag value for selecting the select backend with default config.
inline constexpr select_t<> select{};

template<>
inline detail::scheduler&
select_t<>::construct(capy::execution_context& ctx, unsigned concurrency_hint)
{
    return detail::construct_default_select(ctx, concurrency_hint);
}

#endif // BOOST_COROSIO_HAS_SELECT

// -----------------------------------------------------------------
// kqueue
// -----------------------------------------------------------------
#if BOOST_COROSIO_HAS_KQUEUE

/** Compile-time configuration for the kqueue backend.

    All fields have defaults matching the library's current
    hardcoded values. Pass a customized instance as a template
    argument to @ref kqueue_t to tune behavior at compile time.

    This is a structural type suitable for use as an NTTP.
*/
struct kqueue_config
{
    /// Max events harvested per kevent call.
    unsigned max_events_per_poll = 128;

    /// Starting inline completion budget per handler chain.
    unsigned inline_budget_initial = 2;

    /// Hard ceiling on adaptive inline budget ramp-up.
    unsigned inline_budget_max = 16;

    /// Inline budget when no other thread is in the reactor.
    unsigned unassisted_budget = 4;

    /// Recycle post_handler nodes via a per-thread free list.
    bool recycle_post_nodes = true;
};

namespace detail {

class kqueue_socket;
class kqueue_socket_service;
class kqueue_acceptor;
class kqueue_acceptor_service;
class kqueue_scheduler_core;
template<kqueue_config>
class kqueue_scheduler;

class posix_signal;
class posix_signal_service;
class posix_resolver;
class posix_resolver_service;

} // namespace detail

/** Backend tag for the BSD kqueue I/O multiplexer.

    @tparam Config Compile-time configuration. Defaults produce
        identical behavior to the library's hardcoded constants.

    @par Example
    @code
    // Default config
    io_context ioc(corosio::kqueue);

    // Custom config
    constexpr kqueue_config cfg{.max_events_per_poll = 256};
    io_context ioc(kqueue_t<cfg>{});
    @endcode
*/
template<kqueue_config Config = kqueue_config{}>
struct kqueue_t
{
    static constexpr kqueue_config config = Config;

    using scheduler_type        = detail::kqueue_scheduler<Config>;
    using socket_type           = detail::kqueue_socket;
    using socket_service_type   = detail::kqueue_socket_service;
    using acceptor_type         = detail::kqueue_acceptor;
    using acceptor_service_type = detail::kqueue_acceptor_service;

    using signal_type           = detail::posix_signal;
    using signal_service_type   = detail::posix_signal_service;
    using resolver_type         = detail::posix_resolver;
    using resolver_service_type = detail::posix_resolver_service;

    /// Create the scheduler and services for this backend.
    static detail::scheduler&
    construct(capy::execution_context& ctx, unsigned concurrency_hint);
};

/// Tag value for selecting the kqueue backend with default config.
inline constexpr kqueue_t<> kqueue{};

template<>
inline detail::scheduler&
kqueue_t<>::construct(capy::execution_context& ctx, unsigned concurrency_hint)
{
    return detail::construct_default_kqueue(ctx, concurrency_hint);
}

#endif // BOOST_COROSIO_HAS_KQUEUE

// -----------------------------------------------------------------
// IOCP
// -----------------------------------------------------------------
#if BOOST_COROSIO_HAS_IOCP

/** Compile-time configuration for the IOCP backend.

    This is a structural type suitable for use as an NTTP.
*/
struct iocp_config
{
    /// Max GetQueuedCompletionStatus timeout (ms) between timer
    /// rechecks.
    unsigned gqcs_timeout_ms = 500;

    /// Recycle post_handler nodes via a free list.
    bool recycle_post_nodes = true;
};

namespace detail {

class win_socket;
class win_sockets;
class win_acceptor;
class win_acceptor_service;
class win_scheduler_core;
template<iocp_config> class win_scheduler;

class win_signal;
class win_signals;
class win_resolver;
class win_resolver_service;

} // namespace detail

/** Backend tag for the Windows I/O Completion Ports multiplexer.
*/
template<iocp_config Config = iocp_config{}>
struct iocp_t
{
    static constexpr iocp_config config = Config;
    using scheduler_type        = detail::win_scheduler<Config>;
    using socket_type           = detail::win_socket;
    using socket_service_type   = detail::win_sockets;
    using acceptor_type         = detail::win_acceptor;
    using acceptor_service_type = detail::win_acceptor_service;

    using signal_type           = detail::win_signal;
    using signal_service_type   = detail::win_signals;
    using resolver_type         = detail::win_resolver;
    using resolver_service_type = detail::win_resolver_service;

    /// Create the scheduler and services for this backend.
    static detail::scheduler&
    construct(capy::execution_context& ctx, unsigned concurrency_hint);
};

/// Tag value for selecting the IOCP backend with default config.
inline constexpr iocp_t<> iocp{};

template<>
inline detail::scheduler&
iocp_t<>::construct(capy::execution_context& ctx, unsigned concurrency_hint)
{
    return detail::construct_default_iocp(ctx, concurrency_hint);
}

#endif // BOOST_COROSIO_HAS_IOCP

} // namespace boost::corosio

#endif // BOOST_COROSIO_BACKEND_HPP
