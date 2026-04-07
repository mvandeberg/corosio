//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_LOCAL_DATAGRAM_OP_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_LOCAL_DATAGRAM_OP_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_SELECT

#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/native/detail/reactor/reactor_op.hpp>
#include <boost/corosio/native/detail/select/select_op.hpp>

namespace boost::corosio::detail {

// Forward declarations
class select_local_datagram_socket;

/// Base operation for local datagram sockets on select.
struct select_local_datagram_op
    : reactor_op<select_local_datagram_socket, select_tcp_acceptor>
{
    void operator()() override;
};

/// Connect operation for local datagram sockets.
struct select_local_dgram_connect_op final
    : reactor_connect_op<select_local_datagram_op, corosio::local_endpoint>
{
    void operator()() override;
    void cancel() noexcept override;
};

/// Send-to operation for local datagram sockets.
struct select_local_send_to_op final
    : reactor_send_to_op<select_local_datagram_op>
{
    void cancel() noexcept override;
};

/// Recv-from operation for local datagram sockets.
struct select_local_recv_from_op final
    : reactor_recv_from_op<select_local_datagram_op, corosio::local_endpoint>
{
    void operator()() override;
    void cancel() noexcept override;
};

/// Connected send operation for local datagram sockets.
struct select_local_dgram_send_op final
    : reactor_send_op<select_local_datagram_op>
{
    void cancel() noexcept override;
};

/// Connected recv operation for local datagram sockets.
struct select_local_dgram_recv_op final
    : reactor_recv_op<select_local_datagram_op>
{
    void operator()() override;
    void cancel() noexcept override;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_SELECT

#endif // BOOST_COROSIO_NATIVE_DETAIL_SELECT_SELECT_LOCAL_DATAGRAM_OP_HPP
