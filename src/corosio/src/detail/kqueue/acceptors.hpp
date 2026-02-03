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

    void accept(
        std::coroutine_handle<>,
        capy::executor_ref,
        std::stop_token,
        std::error_code*,
        io_object::io_object_impl**) override;

    int native_handle() const noexcept { return fd_; }
    endpoint local_endpoint() const noexcept override { return local_endpoint_; }
    bool is_open() const noexcept { return fd_ >= 0; }
    void cancel() noexcept override;
    void cancel_single_op(kqueue_op& op) noexcept;
    void close_socket() noexcept;
    void update_kqueue_events() noexcept;
    void set_local_endpoint(endpoint ep) noexcept { local_endpoint_ = ep; }

    kqueue_acceptor_service& service() noexcept { return svc_; }

    kqueue_accept_op acc_;
    descriptor_data desc_data_;

private:
    kqueue_acceptor_service& svc_;
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

    void shutdown() override;

    tcp_acceptor::acceptor_impl& create_acceptor_impl() override;
    void destroy_acceptor_impl(tcp_acceptor::acceptor_impl& impl) override;
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
