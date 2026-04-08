//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_BACKEND_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_BACKEND_HPP

/* Parameterized reactor backend.

   Generates all socket, acceptor, op, and service types for a given
   backend Traits type. This single header replaces ~15 per-backend
   files for each reactor (epoll, kqueue, select).

   Usage: instantiate the nested types via the reactor_types<Traits>
   template in backend.hpp.
*/

#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/udp_socket.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/local_datagram_socket.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/local_stream_acceptor.hpp>
#include <boost/corosio/shutdown_type.hpp>
#include <boost/corosio/detail/tcp_service.hpp>
#include <boost/corosio/detail/udp_service.hpp>
#include <boost/corosio/detail/local_stream_service.hpp>
#include <boost/corosio/detail/local_datagram_service.hpp>
#include <boost/corosio/detail/tcp_acceptor_service.hpp>
#include <boost/corosio/detail/local_stream_acceptor_service.hpp>

#include <boost/corosio/native/detail/reactor/reactor_stream_socket.hpp>
#include <boost/corosio/native/detail/reactor/reactor_datagram_socket.hpp>
#include <boost/corosio/native/detail/reactor/reactor_acceptor.hpp>
#include <boost/corosio/native/detail/reactor/reactor_socket_service.hpp>
#include <boost/corosio/native/detail/reactor/reactor_acceptor_service.hpp>
#include <boost/corosio/native/detail/reactor/reactor_stream_ops.hpp>
#include <boost/corosio/native/detail/reactor/reactor_datagram_ops.hpp>
#include <boost/corosio/native/detail/reactor/reactor_op_complete.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/native/detail/endpoint_convert.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>

#include <mutex>
#include <system_error>

#include <unistd.h>

namespace boost::corosio::detail {

// ============================================================
// Forward declarations
// ============================================================

// Socket/acceptor finals
template<class Traits, class ImplBase, class Endpoint> class reactor_stream_socket_final;
template<class Traits, class ImplBase, class Endpoint> class reactor_dgram_socket_final;
template<class Traits, class AccImplBase, class Endpoint> class reactor_acceptor_final;

// Per-protocol service finals
template<class Traits, class SocketFinal> class reactor_tcp_service_final;
template<class Traits, class SocketFinal> class reactor_local_stream_service_final;
template<class Traits, class SocketFinal> class reactor_udp_service_final;
template<class Traits, class SocketFinal> class reactor_local_dgram_service_final;

// Acceptor service final
template<class Traits, class ServiceBase, class AccFinal, class StreamServiceFinal, class Endpoint> class reactor_acceptor_service_final;

// ============================================================
// Op type aliases (parameterized on Traits + socket/acceptor)
// ============================================================

// Stream ops: use the acceptor for the same Endpoint family
template<class Traits, class Endpoint>
using stream_socket_t = reactor_stream_socket_final<Traits,
    std::conditional_t<std::is_same_v<Endpoint, endpoint>,
        tcp_socket::implementation,
        local_stream_socket::implementation>,
    Endpoint>;

template<class Traits, class Endpoint>
using stream_acceptor_t = reactor_acceptor_final<Traits,
    std::conditional_t<std::is_same_v<Endpoint, endpoint>,
        tcp_acceptor::implementation,
        local_stream_acceptor::implementation>,
    Endpoint>;

template<class Traits, class Endpoint>
using stream_base_op = reactor_stream_base_op<
    Traits,
    stream_socket_t<Traits, Endpoint>,
    stream_acceptor_t<Traits, Endpoint>,
    Endpoint>;

template<class Traits, class Endpoint>
using stream_connect_op = reactor_stream_connect_op<
    Traits,
    stream_socket_t<Traits, Endpoint>,
    stream_acceptor_t<Traits, Endpoint>,
    Endpoint>;

template<class Traits, class Endpoint>
using stream_read_op = reactor_stream_read_op<
    Traits,
    stream_socket_t<Traits, Endpoint>,
    stream_acceptor_t<Traits, Endpoint>,
    Endpoint>;

template<class Traits, class Endpoint>
using stream_write_op = reactor_stream_write_op<
    Traits,
    stream_socket_t<Traits, Endpoint>,
    stream_acceptor_t<Traits, Endpoint>,
    Endpoint>;

template<class Traits, class Endpoint>
using stream_accept_op = reactor_stream_accept_op<
    Traits,
    stream_socket_t<Traits, Endpoint>,
    stream_acceptor_t<Traits, Endpoint>,
    Endpoint>;

// Datagram ops: dummy acceptor uses the stream acceptor type (for reactor_op compatibility)
template<class Traits, class Endpoint>
using dgram_socket_t = reactor_dgram_socket_final<Traits,
    std::conditional_t<std::is_same_v<Endpoint, endpoint>,
        udp_socket::implementation,
        local_datagram_socket::implementation>,
    Endpoint>;

template<class Traits, class Endpoint>
using dgram_connect_op = reactor_dgram_connect_op<
    Traits,
    dgram_socket_t<Traits, Endpoint>,
    stream_acceptor_t<Traits, endpoint>,  // dummy acceptor
    Endpoint>;

template<class Traits, class Endpoint>
using dgram_send_to_op = reactor_dgram_send_to_op<
    Traits,
    dgram_socket_t<Traits, Endpoint>,
    stream_acceptor_t<Traits, endpoint>,
    Endpoint>;

template<class Traits, class Endpoint>
using dgram_recv_from_op = reactor_dgram_recv_from_op<
    Traits,
    dgram_socket_t<Traits, Endpoint>,
    stream_acceptor_t<Traits, endpoint>,
    Endpoint>;

template<class Traits, class Endpoint>
using dgram_send_op = reactor_dgram_send_op<
    Traits,
    dgram_socket_t<Traits, Endpoint>,
    stream_acceptor_t<Traits, endpoint>,
    Endpoint>;

template<class Traits, class Endpoint>
using dgram_recv_op = reactor_dgram_recv_op<
    Traits,
    dgram_socket_t<Traits, Endpoint>,
    stream_acceptor_t<Traits, endpoint>,
    Endpoint>;

// ============================================================
// Stream socket final type
// ============================================================

template<class Traits, class ImplBase, class Endpoint>
class reactor_stream_socket_final final
    : public reactor_stream_socket<
          reactor_stream_socket_final<Traits, ImplBase, Endpoint>,
          std::conditional_t<std::is_same_v<Endpoint, endpoint>,
              reactor_tcp_service_final<
                  Traits, reactor_stream_socket_final<Traits, ImplBase, Endpoint>>,
              reactor_local_stream_service_final<
                  Traits, reactor_stream_socket_final<Traits, ImplBase, Endpoint>>>,
          stream_connect_op<Traits, Endpoint>,
          stream_read_op<Traits, Endpoint>,
          stream_write_op<Traits, Endpoint>,
          typename Traits::desc_state_type,
          ImplBase,
          Endpoint>
{
    using service_type = std::conditional_t<std::is_same_v<Endpoint, endpoint>,
        reactor_tcp_service_final<
            Traits, reactor_stream_socket_final>,
        reactor_local_stream_service_final<
            Traits, reactor_stream_socket_final>>;
    friend service_type;

public:
    using impl_base_type = ImplBase;

    explicit reactor_stream_socket_final(service_type& svc) noexcept
        : reactor_stream_socket_final::reactor_stream_socket(svc)
    {
    }

    ~reactor_stream_socket_final() override = default;
};

// ============================================================
// Datagram socket final type
// ============================================================

template<class Traits, class ImplBase, class Endpoint>
class reactor_dgram_socket_final final
    : public reactor_datagram_socket<
          reactor_dgram_socket_final<Traits, ImplBase, Endpoint>,
          std::conditional_t<std::is_same_v<Endpoint, endpoint>,
              reactor_udp_service_final<
                  Traits, reactor_dgram_socket_final<Traits, ImplBase, Endpoint>>,
              reactor_local_dgram_service_final<
                  Traits, reactor_dgram_socket_final<Traits, ImplBase, Endpoint>>>,
          dgram_connect_op<Traits, Endpoint>,
          dgram_send_to_op<Traits, Endpoint>,
          dgram_recv_from_op<Traits, Endpoint>,
          dgram_send_op<Traits, Endpoint>,
          dgram_recv_op<Traits, Endpoint>,
          typename Traits::desc_state_type,
          ImplBase,
          Endpoint>
{
    using service_type = std::conditional_t<std::is_same_v<Endpoint, endpoint>,
        reactor_udp_service_final<
            Traits, reactor_dgram_socket_final>,
        reactor_local_dgram_service_final<
            Traits, reactor_dgram_socket_final>>;
    friend service_type;

public:
    using impl_base_type = ImplBase;

    explicit reactor_dgram_socket_final(service_type& svc) noexcept
        : reactor_dgram_socket_final::reactor_datagram_socket(svc)
    {
    }

    ~reactor_dgram_socket_final() override = default;
};

// ============================================================
// Acceptor final type
// ============================================================

// Helper: map Endpoint to the correct stream service type
template<class Traits, class Endpoint>
using stream_service_for = std::conditional_t<std::is_same_v<Endpoint, endpoint>,
    reactor_tcp_service_final<Traits, stream_socket_t<Traits, Endpoint>>,
    reactor_local_stream_service_final<Traits, stream_socket_t<Traits, Endpoint>>>;

template<class Traits, class AccImplBase, class Endpoint>
class reactor_acceptor_final final
    : public reactor_acceptor<
          reactor_acceptor_final<Traits, AccImplBase, Endpoint>,
          reactor_acceptor_service_final<
              Traits,
              std::conditional_t<std::is_same_v<Endpoint, endpoint>,
                  tcp_acceptor_service, local_stream_acceptor_service>,
              reactor_acceptor_final<Traits, AccImplBase, Endpoint>,
              stream_service_for<Traits, Endpoint>,
              Endpoint>,
          stream_base_op<Traits, Endpoint>,
          stream_accept_op<Traits, Endpoint>,
          typename Traits::desc_state_type,
          AccImplBase,
          Endpoint>
{
    using acc_service_type = reactor_acceptor_service_final<
        Traits,
        std::conditional_t<std::is_same_v<Endpoint, endpoint>,
            tcp_acceptor_service, local_stream_acceptor_service>,
        reactor_acceptor_final,
        stream_service_for<Traits, Endpoint>,
        Endpoint>;
    friend acc_service_type;

public:
    explicit reactor_acceptor_final(acc_service_type& svc) noexcept
        : reactor_acceptor_final::reactor_acceptor(svc)
    {
    }

    ~reactor_acceptor_final() override = default;

    using impl_base_type = AccImplBase;

    std::coroutine_handle<> accept(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        std::stop_token token,
        std::error_code* ec,
        io_object::implementation** impl_out) override;
};

// ============================================================
// Shared socket creation helper
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
// Service macros — CRTP base, friend, and explicit constructor
// ============================================================

#define COROSIO_REACTOR_SOCKET_SERVICE(name, ServiceBase, SocketFinal)    \
    using base_service = reactor_socket_service<                            \
        name, ServiceBase,                                                  \
        typename Traits::scheduler_type, SocketFinal>;                      \
    friend base_service;                                                    \
public:                                                                     \
    explicit name(capy::execution_context& ctx)                             \
        : base_service(ctx) {}

#define COROSIO_REACTOR_ACCEPTOR_SERVICE(name, ServiceBase, AccFinal, StreamSvc)  \
    using base_service = reactor_acceptor_service<                                \
        name, ServiceBase, typename Traits::scheduler_type,                       \
        AccFinal, StreamSvc>;                                                     \
    friend base_service;                                                          \
public:                                                                           \
    explicit name(capy::execution_context& ctx)                                   \
        : base_service(ctx) {}

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
    COROSIO_REACTOR_SOCKET_SERVICE(
        reactor_tcp_service_final, tcp_service, SocketFinal)

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
    COROSIO_REACTOR_SOCKET_SERVICE(
        reactor_local_stream_service_final, local_stream_service, SocketFinal)

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
    COROSIO_REACTOR_SOCKET_SERVICE(
        reactor_udp_service_final, udp_service, SocketFinal)

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
    COROSIO_REACTOR_SOCKET_SERVICE(
        reactor_local_dgram_service_final, local_datagram_service, SocketFinal)

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
// Acceptor service final type
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
        // Look up the associated stream service for peer socket creation.
        // The ServiceBase's key_type determines which abstract service to look up.
        using stream_svc_key = std::conditional_t<
            std::is_same_v<Endpoint, endpoint>,
            tcp_service, local_stream_service>;

        auto* svc = this->ctx_.template find_service<stream_svc_key>();
        this->stream_svc_ = svc
            ? dynamic_cast<StreamServiceFinal*>(svc)
            : nullptr;
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

// ============================================================
// Acceptor accept() implementation (deferred — all types complete)
// ============================================================

template<class Traits, class AccImplBase, class Endpoint>
std::coroutine_handle<>
reactor_acceptor_final<Traits, AccImplBase, Endpoint>::accept(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    std::stop_token token,
    std::error_code* ec,
    io_object::implementation** impl_out)
{
    using socket_final = stream_socket_t<Traits, Endpoint>;

    auto& op = this->acc_;
    op.reset();
    op.h        = h;
    op.ex       = ex;
    op.ec_out   = ec;
    op.impl_out = impl_out;
    op.fd       = this->fd_;
    op.start(token, this);

    // Speculative accept using the backend's accept policy
    sockaddr_storage peer_storage{};
    socklen_t addrlen = sizeof(peer_storage);

    // Use the accept_policy from traits (accept4 on epoll, accept+fcntl on others)
    int accepted = Traits::accept_policy::do_accept(this->fd_, peer_storage);

    if (accepted >= 0)
    {
        {
            std::lock_guard lock(this->desc_state_.mutex);
            this->desc_state_.read_ready = false;
        }

        if (this->svc_.scheduler().try_consume_inline_budget())
        {
            auto* socket_svc = this->svc_.stream_service();
            if (socket_svc)
            {
                auto& impl =
                    static_cast<socket_final&>(*socket_svc->construct());
                impl.set_socket(accepted);

                impl.desc_state_.fd = accepted;
                {
                    std::lock_guard lock(impl.desc_state_.mutex);
                    impl.desc_state_.read_op    = nullptr;
                    impl.desc_state_.write_op   = nullptr;
                    impl.desc_state_.connect_op = nullptr;
                }
                socket_svc->scheduler().register_descriptor(
                    accepted, &impl.desc_state_);

                impl.set_endpoints(
                    this->local_endpoint_,
                    from_sockaddr_as(
                        peer_storage,
                        static_cast<socklen_t>(sizeof(peer_storage)),
                        Endpoint{}));

                *ec = {};
                if (impl_out)
                    *impl_out = &impl;
            }
            else
            {
                ::close(accepted);
                *ec = make_err(ENOENT);
                if (impl_out)
                    *impl_out = nullptr;
            }
            op.cont_op.cont.h = h;
            return dispatch_coro(ex, op.cont_op.cont);
        }

        op.accepted_fd  = accepted;
        op.peer_storage = peer_storage;
        op.complete(0, 0);
        op.impl_ptr = this->shared_from_this();
        this->svc_.post(&op);
        return std::noop_coroutine();
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
        op.impl_ptr = this->shared_from_this();
        this->svc_.work_started();

        std::lock_guard lock(this->desc_state_.mutex);
        bool io_done = false;
        if (this->desc_state_.read_ready)
        {
            this->desc_state_.read_ready = false;
            op.perform_io();
            io_done = (op.errn != EAGAIN && op.errn != EWOULDBLOCK);
            if (!io_done)
                op.errn = 0;
        }

        if (io_done || op.cancelled.load(std::memory_order_acquire))
        {
            this->svc_.post(&op);
            this->svc_.work_finished();
        }
        else
        {
            this->desc_state_.read_op = &op;
        }
        return std::noop_coroutine();
    }

    op.complete(errno, 0);
    op.impl_ptr = this->shared_from_this();
    this->svc_.post(&op);
    return std::noop_coroutine();
}

// ============================================================
// Type bundle for backend.hpp
// ============================================================

template<class Traits>
struct reactor_types
{
    using tcp_socket_type = stream_socket_t<Traits, endpoint>;
    using tcp_service_type = reactor_tcp_service_final<
        Traits, tcp_socket_type>;

    using udp_socket_type = dgram_socket_t<Traits, endpoint>;
    using udp_service_type = reactor_udp_service_final<
        Traits, udp_socket_type>;

    using tcp_acceptor_type = stream_acceptor_t<Traits, endpoint>;
    using tcp_acceptor_service_type = reactor_acceptor_service_final<
        Traits, tcp_acceptor_service, tcp_acceptor_type,
        tcp_service_type, endpoint>;

    using local_stream_socket_type = stream_socket_t<Traits, local_endpoint>;
    using local_stream_service_type = reactor_local_stream_service_final<
        Traits, local_stream_socket_type>;

    using local_datagram_socket_type = dgram_socket_t<Traits, local_endpoint>;
    using local_datagram_service_type = reactor_local_dgram_service_final<
        Traits, local_datagram_socket_type>;

    using local_stream_acceptor_type = stream_acceptor_t<Traits, local_endpoint>;
    using local_stream_acceptor_service_type = reactor_acceptor_service_final<
        Traits, local_stream_acceptor_service, local_stream_acceptor_type,
        local_stream_service_type, local_endpoint>;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_BACKEND_HPP
