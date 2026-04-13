//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SOCKET_FINALS_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SOCKET_FINALS_HPP

/* Parameterized socket, datagram, and acceptor implementation bases.

   Named per-backend classes (e.g. epoll_tcp_socket) inherit from
   these templates, supplying Derived (CRTP self) and the concrete
   service / peer types. This file no longer contains final classes
   or type aliases that compute types from Traits+Endpoint — those
   moved to per-backend type files (epoll_types.hpp, etc.).
*/

#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/udp_socket.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/shutdown_type.hpp>

#include <boost/corosio/native/detail/reactor/reactor_stream_socket.hpp>
#include <boost/corosio/native/detail/reactor/reactor_datagram_socket.hpp>
#include <boost/corosio/native/detail/reactor/reactor_acceptor.hpp>
#include <boost/corosio/native/detail/reactor/reactor_stream_ops.hpp>
#include <boost/corosio/native/detail/reactor/reactor_datagram_ops.hpp>

#include <boost/corosio/native/detail/make_err.hpp>

namespace boost::corosio::detail {

// ============================================================
// Stream socket implementation base
// ============================================================

// release_socket below cannot be marked 'override' unconditionally: it
// overrides a pure virtual only when ImplBase is local_stream_socket::
// implementation. For the tcp_socket::implementation instantiation the
// base has no such virtual, so 'override' would fail to compile. Scope-
// suppress clang's -Winconsistent-missing-override for the overriding
// instantiation. clang-tidy's modernize-use-override is silenced
// separately via NOLINTNEXTLINE below.
BOOST_COROSIO_CLANG_WARNING_PUSH
BOOST_COROSIO_CLANG_WARNING_DISABLE("-Winconsistent-missing-override")

/** Intermediate base for reactor stream sockets.

    Holds the per-socket hook (e.g., kqueue SO_LINGER tracking),
    the set_option override, and the close/release shadows.
    Named per-backend classes inherit from this as final.

    @tparam Derived      The named final class (CRTP self).
    @tparam Traits       Backend traits (epoll_traits, etc.).
    @tparam Service      The concrete service type.
    @tparam AcceptorType The concrete acceptor type (for op base).
    @tparam ImplBase     The public vtable base.
    @tparam Endpoint     endpoint or local_endpoint.
*/
template<class Derived, class Traits, class Service,
         class AcceptorType, class ImplBase, class Endpoint>
class reactor_stream_socket_impl
    : public reactor_stream_socket<
          Derived,
          Service,
          reactor_stream_connect_op<Traits, Derived, AcceptorType, Endpoint>,
          reactor_stream_read_op<Traits, Derived, AcceptorType, Endpoint>,
          reactor_stream_write_op<Traits, Derived, AcceptorType, Endpoint>,
          typename Traits::desc_state_type,
          ImplBase,
          Endpoint>
{
    friend Service;

public:
    using impl_base_type = ImplBase;

    /// Per-socket hook state (e.g., kqueue SO_LINGER tracking).
    [[no_unique_address]] typename Traits::stream_socket_hook hook_;

    explicit reactor_stream_socket_impl(Service& svc) noexcept
        : reactor_stream_socket_impl::reactor_stream_socket(svc)
    {
    }

    ~reactor_stream_socket_impl() override = default;

    std::error_code set_option(
        int level, int optname,
        void const* data, std::size_t size) noexcept override
    {
        return hook_.on_set_option(this->fd_, level, optname, data, size);
    }

    // Shadows reactor_stream_socket::close_socket so the hook fires on
    // every fd close path (user close(), service shutdown/destroy,
    // do_assign_fd reuse). The hook needs the fd before do_close_socket
    // resets it to -1.
    void close_socket() noexcept
    {
        hook_.pre_shutdown(this->fd_);
        this->do_close_socket();
    }

    // Overrides local_stream_socket::implementation::release_socket().
    // Cannot use 'override' — tcp_socket::implementation has no such method.
    // NOLINTNEXTLINE(modernize-use-override)
    native_handle_type release_socket() noexcept
    {
        hook_ = {};
        return this->do_release_socket();
    }
};

BOOST_COROSIO_CLANG_WARNING_POP

// ============================================================
// Datagram socket implementation base
// ============================================================

// shutdown/bind/release_socket below cannot be marked 'override'
// unconditionally: they override pure virtuals only when ImplBase is
// local_datagram_socket::implementation. For the udp_socket::implementation
// instantiation the base has no such virtuals, so 'override' would fail
// to compile. Scope-suppress clang's -Winconsistent-missing-override for
// the overriding instantiation. clang-tidy's modernize-use-override is
// silenced separately via NOLINTNEXTLINE below.
BOOST_COROSIO_CLANG_WARNING_PUSH
BOOST_COROSIO_CLANG_WARNING_DISABLE("-Winconsistent-missing-override")

/** Intermediate base for reactor datagram sockets.

    @tparam Derived      The named final class (CRTP self).
    @tparam Traits       Backend traits.
    @tparam Service      The concrete datagram service type.
    @tparam AcceptorType The concrete acceptor type (placeholder for op base).
    @tparam ImplBase     The public vtable base.
    @tparam Endpoint     endpoint or local_endpoint.
*/
template<class Derived, class Traits, class Service,
         class AcceptorType, class ImplBase, class Endpoint>
class reactor_dgram_socket_impl
    : public reactor_datagram_socket<
          Derived,
          Service,
          reactor_dgram_connect_op<Traits, Derived, AcceptorType, Endpoint>,
          reactor_dgram_send_to_op<Traits, Derived, AcceptorType, Endpoint>,
          reactor_dgram_recv_from_op<Traits, Derived, AcceptorType, Endpoint>,
          reactor_dgram_send_op<Traits, Derived, AcceptorType, Endpoint>,
          reactor_dgram_recv_op<Traits, Derived, AcceptorType, Endpoint>,
          typename Traits::desc_state_type,
          ImplBase,
          Endpoint>
{
    friend Service;

public:
    using impl_base_type = ImplBase;

    explicit reactor_dgram_socket_impl(Service& svc) noexcept
        : reactor_dgram_socket_impl::reactor_datagram_socket(svc)
    {
    }

    ~reactor_dgram_socket_impl() override = default;

    // Overrides local_datagram_socket pure virtuals.
    // Cannot use 'override' — udp_socket::implementation has no such methods.
    // NOLINTNEXTLINE(modernize-use-override)
    std::error_code shutdown(corosio::shutdown_type what) noexcept
    {
        return this->do_shutdown(static_cast<int>(what));
    }

    // NOLINTNEXTLINE(modernize-use-override)
    std::error_code bind(Endpoint ep) noexcept
    {
        return this->do_bind(ep);
    }

    // NOLINTNEXTLINE(modernize-use-override)
    native_handle_type release_socket() noexcept
    {
        return this->do_release_socket();
    }
};

BOOST_COROSIO_CLANG_WARNING_POP

// ============================================================
// Acceptor implementation base
// ============================================================

// release_socket below cannot be marked 'override' unconditionally: it
// overrides a pure virtual only when AccImplBase is
// local_stream_acceptor::implementation. For the tcp_acceptor::implementation
// instantiation the base has no such virtual, so 'override' would fail to
// compile. Scope-suppress clang's -Winconsistent-missing-override for the
// overriding instantiation. clang-tidy's modernize-use-override is silenced
// separately via NOLINTNEXTLINE below.
BOOST_COROSIO_CLANG_WARNING_PUSH
BOOST_COROSIO_CLANG_WARNING_DISABLE("-Winconsistent-missing-override")

/** Intermediate base for reactor stream acceptors.

    @tparam Derived      The named final class (CRTP self).
    @tparam Traits       Backend traits.
    @tparam Service      The concrete acceptor service type.
    @tparam SocketFinal  The concrete stream socket type (for accept).
    @tparam AccImplBase  The public vtable base.
    @tparam Endpoint     endpoint or local_endpoint.
*/
template<class Derived, class Traits, class Service,
         class SocketFinal, class AccImplBase, class Endpoint>
class reactor_acceptor_impl
    : public reactor_acceptor<
          Derived,
          Service,
          reactor_stream_base_op<Traits, SocketFinal, Derived, Endpoint>,
          reactor_stream_accept_op<Traits, SocketFinal, Derived, Endpoint>,
          typename Traits::desc_state_type,
          AccImplBase,
          Endpoint>
{
    friend Service;

public:
    using impl_base_type = AccImplBase;

    explicit reactor_acceptor_impl(Service& svc) noexcept
        : reactor_acceptor_impl::reactor_acceptor(svc)
    {
    }

    ~reactor_acceptor_impl() override = default;

    // NOLINTNEXTLINE(modernize-use-override)
    native_handle_type release_socket() noexcept
    {
        return this->do_release_socket();
    }

    std::coroutine_handle<> accept(
        std::coroutine_handle<>,
        capy::executor_ref,
        std::stop_token,
        std::error_code*,
        io_object::implementation**) override;
};

BOOST_COROSIO_CLANG_WARNING_POP

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SOCKET_FINALS_HPP
