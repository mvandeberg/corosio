//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_LOCAL_STREAM_OP_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_LOCAL_STREAM_OP_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/local_endpoint.hpp>
#include <boost/corosio/native/detail/reactor/reactor_op.hpp>
#include <boost/corosio/native/detail/kqueue/kqueue_op.hpp>

namespace boost::corosio::detail {

// Forward declarations
class kqueue_local_stream_socket;
class kqueue_local_stream_acceptor;

/// Base operation for local stream sockets on kqueue.
struct kqueue_local_stream_op
    : reactor_op<kqueue_local_stream_socket, kqueue_local_stream_acceptor>
{
    void operator()() override;
};

/// Connect operation for local stream sockets.
struct kqueue_local_connect_op final
    : reactor_connect_op<kqueue_local_stream_op, local_endpoint>
{
    void operator()() override;
    void cancel() noexcept override;
};

/// Scatter-read operation for local stream sockets.
struct kqueue_local_read_op final : reactor_read_op<kqueue_local_stream_op>
{
    void cancel() noexcept override;
};

/// Gather-write operation for local stream sockets.
struct kqueue_local_write_op final
    : reactor_write_op<kqueue_local_stream_op, kqueue_write_policy>
{
    void cancel() noexcept override;
};

/// Accept operation for local stream sockets.
struct kqueue_local_accept_op final
    : reactor_accept_op<kqueue_local_stream_op, kqueue_accept_policy>
{
    void operator()() override;
    void cancel() noexcept override;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE

#endif // BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_LOCAL_STREAM_OP_HPP
