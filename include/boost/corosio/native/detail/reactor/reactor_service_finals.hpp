//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SERVICE_FINALS_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SERVICE_FINALS_HPP

/* Parameterized final service types for reactor backends.

   One service template per protocol (TCP, local stream, UDP, local
   datagram, TCP acceptor, local stream acceptor) because each abstract
   service base declares different virtual methods.
*/

#include <boost/corosio/native/detail/reactor/reactor_socket_finals.hpp>
#include <boost/corosio/native/detail/reactor/reactor_socket_service.hpp>
#include <boost/corosio/native/detail/reactor/reactor_acceptor_service.hpp>
#include <boost/corosio/detail/tcp_service.hpp>
#include <boost/corosio/detail/udp_service.hpp>
#include <boost/corosio/detail/local_stream_service.hpp>
#include <boost/corosio/detail/local_datagram_service.hpp>

#include <boost/corosio/native/detail/make_err.hpp>

#include <system_error>
#include <unistd.h>

namespace boost::corosio::detail {

// ============================================================
// Shared socket creation helpers
// ============================================================

template<class Traits, class SocketFinal>
std::error_code
do_open_socket(
    SocketFinal* socket_impl,
    int family, int type, int protocol,
    bool is_ip) noexcept
{
    socket_impl->close_socket();

    int fd = Traits::create_socket(family, type, protocol);
    if (fd < 0)
        return make_err(errno);

    std::error_code ec = is_ip
        ? Traits::configure_ip_socket(fd, family)
        : Traits::configure_local_socket(fd);

    if (ec)
    {
        ::close(fd);
        return ec;
    }

    socket_impl->init_and_register(fd);
    return {};
}

template<class Traits, class SocketFinal>
std::error_code
do_assign_fd(
    SocketFinal* socket_impl,
    int fd) noexcept
{
    socket_impl->close_socket();
    socket_impl->init_and_register(fd);
    return {};
}

template<class Traits, class AccFinal>
std::error_code
do_open_acceptor(
    AccFinal* acc_impl,
    int family, int type, int protocol,
    bool is_ip) noexcept
{
    acc_impl->close_socket();

    int fd = Traits::create_socket(family, type, protocol);
    if (fd < 0)
        return make_err(errno);

    std::error_code ec = is_ip
        ? Traits::configure_ip_acceptor(fd, family)
        : Traits::configure_local_socket(fd);

    if (ec)
    {
        ::close(fd);
        return ec;
    }

    acc_impl->init_acceptor_fd(fd);
    return {};
}

// ============================================================
// TCP service
// ============================================================

template<class Traits, class SocketFinal>
class reactor_tcp_service_final final
    : public reactor_socket_service<
          reactor_tcp_service_final<Traits, SocketFinal>,
          tcp_service,
          typename Traits::scheduler_type,
          SocketFinal>
{
    using base_service = reactor_socket_service<
        reactor_tcp_service_final, tcp_service,
        typename Traits::scheduler_type, SocketFinal>;
    friend base_service;

public:
    explicit reactor_tcp_service_final(capy::execution_context& ctx)
        : base_service(ctx) {}

    std::error_code open_socket(
        tcp_socket::implementation& impl,
        int family, int type, int protocol) override
    {
        return do_open_socket<Traits>(
            static_cast<SocketFinal*>(&impl),
            family, type, protocol, true);
    }

    std::error_code bind_socket(
        tcp_socket::implementation& impl, endpoint ep) override
    {
        return static_cast<SocketFinal*>(&impl)->do_bind(ep);
    }

    void pre_shutdown(SocketFinal* impl) noexcept
    {
        impl->hook_.pre_shutdown(impl->native_handle());
    }

    void pre_destroy(SocketFinal* impl) noexcept
    {
        impl->hook_.pre_destroy(impl->native_handle());
    }
};

// ============================================================
// Local stream service
// ============================================================

template<class Traits, class SocketFinal>
class reactor_local_stream_service_final final
    : public reactor_socket_service<
          reactor_local_stream_service_final<Traits, SocketFinal>,
          local_stream_service,
          typename Traits::scheduler_type,
          SocketFinal>
{
    using base_service = reactor_socket_service<
        reactor_local_stream_service_final, local_stream_service,
        typename Traits::scheduler_type, SocketFinal>;
    friend base_service;

public:
    explicit reactor_local_stream_service_final(capy::execution_context& ctx)
        : base_service(ctx) {}

    std::error_code open_socket(
        local_stream_socket::implementation& impl,
        int family, int type, int protocol) override
    {
        return do_open_socket<Traits>(
            static_cast<SocketFinal*>(&impl),
            family, type, protocol, false);
    }

    std::error_code assign_socket(
        local_stream_socket::implementation& impl, int fd) override
    {
        return do_assign_fd<Traits>(
            static_cast<SocketFinal*>(&impl), fd);
    }
};

// ============================================================
// UDP service
// ============================================================

template<class Traits, class SocketFinal>
class reactor_udp_service_final final
    : public reactor_socket_service<
          reactor_udp_service_final<Traits, SocketFinal>,
          udp_service,
          typename Traits::scheduler_type,
          SocketFinal>
{
    using base_service = reactor_socket_service<
        reactor_udp_service_final, udp_service,
        typename Traits::scheduler_type, SocketFinal>;
    friend base_service;

public:
    explicit reactor_udp_service_final(capy::execution_context& ctx)
        : base_service(ctx) {}

    std::error_code open_datagram_socket(
        udp_socket::implementation& impl,
        int family, int type, int protocol) override
    {
        return do_open_socket<Traits>(
            static_cast<SocketFinal*>(&impl),
            family, type, protocol, true);
    }

    std::error_code bind_datagram(
        udp_socket::implementation& impl, endpoint ep) override
    {
        return static_cast<SocketFinal*>(&impl)->do_bind(ep);
    }
};

// ============================================================
// Local datagram service
// ============================================================

template<class Traits, class SocketFinal>
class reactor_local_dgram_service_final final
    : public reactor_socket_service<
          reactor_local_dgram_service_final<Traits, SocketFinal>,
          local_datagram_service,
          typename Traits::scheduler_type,
          SocketFinal>
{
    using base_service = reactor_socket_service<
        reactor_local_dgram_service_final, local_datagram_service,
        typename Traits::scheduler_type, SocketFinal>;
    friend base_service;

public:
    explicit reactor_local_dgram_service_final(capy::execution_context& ctx)
        : base_service(ctx) {}

    std::error_code open_socket(
        local_datagram_socket::implementation& impl,
        int family, int type, int protocol) override
    {
        return do_open_socket<Traits>(
            static_cast<SocketFinal*>(&impl),
            family, type, protocol, false);
    }

    std::error_code assign_socket(
        local_datagram_socket::implementation& impl, int fd) override
    {
        return do_assign_fd<Traits>(
            static_cast<SocketFinal*>(&impl), fd);
    }

    std::error_code bind_socket(
        local_datagram_socket::implementation& impl,
        corosio::local_endpoint ep) override
    {
        return static_cast<SocketFinal*>(&impl)->do_bind(ep);
    }
};

// ============================================================
// Acceptor service
// ============================================================

template<class Traits, class ServiceBase, class AccFinal,
         class StreamServiceFinal, class Endpoint>
class reactor_acceptor_service_final final
    : public reactor_acceptor_service<
          reactor_acceptor_service_final<Traits, ServiceBase, AccFinal,
                                         StreamServiceFinal, Endpoint>,
          ServiceBase,
          typename Traits::scheduler_type,
          AccFinal,
          StreamServiceFinal>
{
    using base_service = reactor_acceptor_service<
        reactor_acceptor_service_final,
        ServiceBase,
        typename Traits::scheduler_type,
        AccFinal,
        StreamServiceFinal>;
    friend base_service;

public:
    explicit reactor_acceptor_service_final(capy::execution_context& ctx)
        : base_service(ctx)
    {
        // Look up the concrete stream service directly by its type.
        // Avoids dynamic_cast which can fail across template boundaries
        // on some platforms (FreeBSD clang RTTI/visibility).
        this->stream_svc_ =
            this->ctx_.template find_service<StreamServiceFinal>();
    }

    std::error_code open_acceptor_socket(
        typename AccFinal::impl_base_type& impl,
        int family, int type, int protocol) override
    {
        return do_open_acceptor<Traits>(
            static_cast<AccFinal*>(&impl),
            family, type, protocol,
            std::is_same_v<Endpoint, endpoint>);
    }

    std::error_code bind_acceptor(
        typename AccFinal::impl_base_type& impl,
        Endpoint ep) override
    {
        return static_cast<AccFinal*>(&impl)->do_bind(ep);
    }

    std::error_code listen_acceptor(
        typename AccFinal::impl_base_type& impl,
        int backlog) override
    {
        return static_cast<AccFinal*>(&impl)->do_listen(backlog);
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SERVICE_FINALS_HPP
