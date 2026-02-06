//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_KQUEUE_ACCEPTORS_HPP
#define BOOST_COROSIO_DETAIL_KQUEUE_ACCEPTORS_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include "src/detail/intrusive.hpp"
#include "src/detail/socket_service.hpp"

#include "src/detail/kqueue/op.hpp"
#include "src/detail/kqueue/scheduler.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>

/*
    kqueue acceptor components:

    kqueue_acceptor_impl   – per-listener state; owns the listening fd,
                             a descriptor_state for edge-triggered readiness,
                             and a single kqueue_accept_op slot (acc_).
                             Inherits enable_shared_from_this so pending ops
                             can prevent premature destruction.

    kqueue_acceptor_state  – shared state for the service: an intrusive list
                             of live acceptor impls, a shared_ptr map for
                             ownership, and a mutex guarding both.

    kqueue_acceptor_service – execution_context service keyed by
                              acceptor_service (base class). Creates/destroys
                              impls, opens listening sockets, and forwards
                              post/work_started/work_finished to the scheduler.
                              Shutdown walks the impl list and closes all fds.
*/

namespace boost::corosio::detail {

class kqueue_acceptor_service;
class kqueue_acceptor_impl;
class kqueue_socket_service;

/// Acceptor implementation for kqueue backend.
class kqueue_acceptor_impl
    : public tcp_acceptor::acceptor_impl
    , public std::enable_shared_from_this<kqueue_acceptor_impl>
    , public intrusive_list<kqueue_acceptor_impl>::node
{
    friend class kqueue_acceptor_service;

public:
    explicit kqueue_acceptor_impl(kqueue_acceptor_service& svc) noexcept;

    void release() override;

    std::coroutine_handle<> accept(
        std::coroutine_handle<> caller,
        capy::executor_ref ex,
        std::stop_token token,
        std::error_code* ec,
        io_object::io_object_impl** out_impl) override;

    int native_handle() const noexcept { return fd_; }
    endpoint local_endpoint() const noexcept override { return local_endpoint_; }
    bool is_open() const noexcept { return fd_ >= 0; }
    void cancel() noexcept override;
    void cancel_single_op(kqueue_op& op) noexcept;
    void close_socket() noexcept;
    void set_local_endpoint(endpoint ep) noexcept { local_endpoint_ = ep; }

    kqueue_acceptor_service& service() noexcept { return svc_; }

private:
    kqueue_acceptor_service& svc_;
    kqueue_accept_op acc_;
    descriptor_state desc_state_;
    int fd_ = -1;
    endpoint local_endpoint_;
};

/** State for kqueue acceptor service. */
class kqueue_acceptor_state
{
public:
    explicit kqueue_acceptor_state(kqueue_scheduler& sched) noexcept
        : sched_(sched)
    {
    }

    kqueue_scheduler& sched_;
    std::mutex mutex_;
    intrusive_list<kqueue_acceptor_impl> acceptor_list_;
    std::unordered_map<kqueue_acceptor_impl*, std::shared_ptr<kqueue_acceptor_impl>> acceptor_ptrs_;
};

/** kqueue acceptor service implementation.

    Inherits from acceptor_service to enable runtime polymorphism.
    Uses key_type = acceptor_service for service lookup.
*/
class kqueue_acceptor_service : public acceptor_service
{
public:
    explicit kqueue_acceptor_service(capy::execution_context& ctx);
    ~kqueue_acceptor_service();

    kqueue_acceptor_service(kqueue_acceptor_service const&) = delete;
    kqueue_acceptor_service& operator=(kqueue_acceptor_service const&) = delete;

    /** Synchronously close all acceptor fds and cancel pending ops.
        Idempotent; called by the execution_context during teardown.
    */
    void shutdown() override;

    /** Create a new acceptor impl owned by this service.
        The returned tcp_acceptor::acceptor_impl must be destroyed
        via destroy_acceptor_impl() or by shutdown().
    */
    tcp_acceptor::acceptor_impl& create_acceptor_impl() override;

    /** Remove and destroy an impl previously returned by
        create_acceptor_impl(). Closes the socket if still open.
    */
    void destroy_acceptor_impl(tcp_acceptor::acceptor_impl& impl) override;

    /** Bind and listen on @p ep with the given @p backlog.
        Registers the fd with kqueue on success and caches the
        local endpoint. Returns a non-zero std::error_code on
        any syscall failure (socket, bind, listen, fcntl).
    */
    std::error_code open_acceptor(
        tcp_acceptor::acceptor_impl& impl,
        endpoint ep,
        int backlog) override;

    kqueue_scheduler& scheduler() const noexcept { return state_->sched_; }
    void post(kqueue_op* op);
    void work_started() noexcept;
    void work_finished() noexcept;

    /** Get the socket service for creating peer sockets during accept. */
    kqueue_socket_service* socket_service() const noexcept;

private:
    capy::execution_context& ctx_;
    std::unique_ptr<kqueue_acceptor_state> state_;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE

#endif // BOOST_COROSIO_DETAIL_KQUEUE_ACCEPTORS_HPP
