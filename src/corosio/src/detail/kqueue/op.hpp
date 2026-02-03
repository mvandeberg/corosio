//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_KQUEUE_OP_HPP
#define BOOST_COROSIO_DETAIL_KQUEUE_OP_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/io_object.hpp>
#include <boost/corosio/endpoint.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/coro.hpp>
#include <boost/capy/error.hpp>
#include <system_error>

#include "src/detail/make_err.hpp"
#include "src/detail/resume_coro.hpp"
#include "src/detail/scheduler_op.hpp"
#include "src/detail/endpoint_convert.hpp"

#include <unistd.h>
#include <errno.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>

/*
    kqueue Operation State
    ======================

    Each async I/O operation has a corresponding kqueue_op-derived struct that
    holds the operation's state while it's in flight. The socket impl owns
    fixed slots for each operation type (conn_, rd_, wr_), so only one
    operation of each type can be pending per socket at a time.

    Persistent Registration
    -----------------------
    File descriptors are registered with kqueue once (via descriptor_data) and
    stay registered until closed. The descriptor_data tracks which operations
    are pending (read_op, write_op, connect_op). When an event arrives, the
    reactor dispatches to the appropriate pending operation.

    Impl Lifetime Management
    ------------------------
    When cancel() posts an op to the scheduler's ready queue, the socket impl
    might be destroyed before the scheduler processes the op. The `impl_ptr`
    member holds a shared_ptr to the impl, keeping it alive until the op
    completes. This is set by cancel() and cleared in operator() after the
    coroutine is resumed.

    EOF Detection
    -------------
    For reads, 0 bytes with no error means EOF. But an empty user buffer also
    returns 0 bytes. The `empty_buffer_read` flag distinguishes these cases.

    SIGPIPE Prevention
    ------------------
    Writes use writev() with SO_NOSIGPIPE socket option set at socket creation
    to prevent SIGPIPE when the peer has closed.
*/

namespace boost::corosio::detail {

// Forward declarations
class kqueue_socket_impl;
class kqueue_acceptor_impl;
struct kqueue_op;

/** Per-descriptor state for persistent kqueue registration.

    Tracks pending operations for a file descriptor. The fd is registered
    once with kqueue and stays registered until closed. Events are dispatched
    to the appropriate pending operation (EVFILT_READ -> read_op, etc.).

    With edge-triggered kqueue (EV_CLEAR), atomic operations are required to
    synchronize between operation registration and reactor event delivery.
    The read_ready/write_ready flags cache edge events that arrived before
    an operation was registered.
*/
struct descriptor_data
{
    /// Currently registered events (EPOLLIN, EPOLLOUT, etc.)
    std::uint32_t registered_events = 0;

    /// Pending read operation (nullptr if none)
    std::atomic<kqueue_op*> read_op{nullptr};

    /// Pending write operation (nullptr if none)
    std::atomic<kqueue_op*> write_op{nullptr};

    /// Pending connect operation (nullptr if none)
    std::atomic<kqueue_op*> connect_op{nullptr};

    /// Cached read readiness (edge event arrived before op registered)
    std::atomic<bool> read_ready{false};

    /// Cached write readiness (edge event arrived before op registered)
    std::atomic<bool> write_ready{false};

    /// The file descriptor
    int fd = -1;

    /// Whether this descriptor is managed by persistent registration
    bool is_registered = false;
};

struct kqueue_op : scheduler_op
{
    struct canceller
    {
        kqueue_op* op;
        void operator()() const noexcept;
    };

    capy::coro h;
    capy::executor_ref ex;
    std::error_code* ec_out = nullptr;
    std::size_t* bytes_out = nullptr;

    int fd = -1;
    int errn = 0;
    std::size_t bytes_transferred = 0;

    std::atomic<bool> cancelled{false};
    std::optional<std::stop_callback<canceller>> stop_cb;

    // Prevents use-after-free when socket is closed with pending ops.
    // See "Impl Lifetime Management" in file header.
    std::shared_ptr<void> impl_ptr;

    // For stop_token cancellation - pointer to owning socket/acceptor impl.
    // When stop is requested, we call back to the impl to perform actual I/O cancellation.
    kqueue_socket_impl* socket_impl_ = nullptr;
    kqueue_acceptor_impl* acceptor_impl_ = nullptr;

    kqueue_op()
    {
        data_ = this;
    }

    void reset() noexcept
    {
        fd = -1;
        errn = 0;
        bytes_transferred = 0;
        cancelled.store(false, std::memory_order_relaxed);
        impl_ptr.reset();
        socket_impl_ = nullptr;
        acceptor_impl_ = nullptr;
    }

    void operator()() override
    {
        stop_cb.reset();

        if (ec_out)
        {
            if (cancelled.load(std::memory_order_acquire))
                *ec_out = capy::error::canceled;
            else if (errn != 0)
                *ec_out = make_err(errn);
            else if (is_read_operation() && bytes_transferred == 0)
                *ec_out = capy::error::eof;
            else
                *ec_out = {};
        }

        if (bytes_out)
            *bytes_out = bytes_transferred;

        // Move to stack before resuming coroutine. The coroutine might close
        // the socket, releasing the last wrapper ref. If impl_ptr were the
        // last ref and we destroyed it while still in operator(), we'd have
        // use-after-free. Moving to local ensures destruction happens at
        // function exit, after all member accesses are complete.
        capy::executor_ref saved_ex( std::move( ex ) );
        capy::coro saved_h( std::move( h ) );
        auto prevent_premature_destruction = std::move(impl_ptr);
        resume_coro(saved_ex, saved_h);
    }

    virtual bool is_read_operation() const noexcept { return false; }
    virtual void cancel() noexcept = 0;

    void destroy() override
    {
        stop_cb.reset();
        impl_ptr.reset();
    }

    void request_cancel() noexcept
    {
        cancelled.store(true, std::memory_order_release);
    }

    void start(std::stop_token token)
    {
        cancelled.store(false, std::memory_order_release);
        stop_cb.reset();
        socket_impl_ = nullptr;
        acceptor_impl_ = nullptr;

        if (token.stop_possible())
            stop_cb.emplace(token, canceller{this});
    }

    void start(std::stop_token token, kqueue_socket_impl* impl)
    {
        cancelled.store(false, std::memory_order_release);
        stop_cb.reset();
        socket_impl_ = impl;
        acceptor_impl_ = nullptr;

        if (token.stop_possible())
            stop_cb.emplace(token, canceller{this});
    }

    void start(std::stop_token token, kqueue_acceptor_impl* impl)
    {
        cancelled.store(false, std::memory_order_release);
        stop_cb.reset();
        socket_impl_ = nullptr;
        acceptor_impl_ = impl;

        if (token.stop_possible())
            stop_cb.emplace(token, canceller{this});
    }

    void complete(int err, std::size_t bytes) noexcept
    {
        errn = err;
        bytes_transferred = bytes;
    }

    virtual void perform_io() noexcept {}
};


struct kqueue_connect_op : kqueue_op
{
    endpoint target_endpoint;

    void reset() noexcept
    {
        kqueue_op::reset();
        target_endpoint = endpoint{};
    }

    void perform_io() noexcept override
    {
        // connect() completion status is retrieved via SO_ERROR, not return value
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
            err = errno;
        complete(err, 0);
    }

    // Defined in sockets.cpp where kqueue_socket_impl is complete
    void operator()() override;
    void cancel() noexcept override;
};


struct kqueue_read_op : kqueue_op
{
    static constexpr std::size_t max_buffers = 16;
    iovec iovecs[max_buffers];
    int iovec_count = 0;
    bool empty_buffer_read = false;

    bool is_read_operation() const noexcept override
    {
        return !empty_buffer_read;
    }

    void reset() noexcept
    {
        kqueue_op::reset();
        iovec_count = 0;
        empty_buffer_read = false;
    }

    void perform_io() noexcept override
    {
        ssize_t n = ::readv(fd, iovecs, iovec_count);
        if (n >= 0)
            complete(0, static_cast<std::size_t>(n));
        else
            complete(errno, 0);
    }

    void cancel() noexcept override;
};


struct kqueue_write_op : kqueue_op
{
    static constexpr std::size_t max_buffers = 16;
    iovec iovecs[max_buffers];
    int iovec_count = 0;

    void reset() noexcept
    {
        kqueue_op::reset();
        iovec_count = 0;
    }

    void perform_io() noexcept override
    {
        // Use writev() instead of sendmsg() since SO_NOSIGPIPE is set at socket level
        ssize_t n = ::writev(fd, iovecs, iovec_count);
        if (n >= 0)
            complete(0, static_cast<std::size_t>(n));
        else
            complete(errno, 0);
    }

    void cancel() noexcept override;
};


struct kqueue_accept_op : kqueue_op
{
    int accepted_fd = -1;
    io_object::io_object_impl* peer_impl = nullptr;
    io_object::io_object_impl** impl_out = nullptr;

    void reset() noexcept
    {
        kqueue_op::reset();
        accepted_fd = -1;
        peer_impl = nullptr;
        impl_out = nullptr;
    }

    // perform_io() defined in acceptors.cpp where kqueue_acceptor_impl is complete
    void perform_io() noexcept override;

    // Defined in acceptors.cpp where kqueue_acceptor_impl is complete
    void operator()() override;
    void cancel() noexcept override;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE

#endif // BOOST_COROSIO_DETAIL_KQUEUE_OP_HPP
