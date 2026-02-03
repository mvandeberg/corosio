//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include "src/detail/kqueue/acceptors.hpp"
#include "src/detail/kqueue/sockets.hpp"
#include "src/detail/endpoint_convert.hpp"
#include "src/detail/make_err.hpp"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <unistd.h>

namespace boost::corosio::detail {

void
kqueue_accept_op::
perform_io() noexcept
{
    sockaddr_in addr{};
    socklen_t addrlen = sizeof(addr);

    // Use accept() instead of accept4() (not available on macOS)
    int new_fd = ::accept(fd, reinterpret_cast<sockaddr*>(&addr), &addrlen);

    if (new_fd >= 0)
    {
        // Set non-blocking
        int flags = ::fcntl(new_fd, F_GETFL, 0);
        if (flags == -1)
        {
            ::close(new_fd);
            complete(errno, 0);
            return;
        }
        if (::fcntl(new_fd, F_SETFL, flags | O_NONBLOCK) == -1)
        {
            ::close(new_fd);
            complete(errno, 0);
            return;
        }

        // Set close-on-exec
        if (::fcntl(new_fd, F_SETFD, FD_CLOEXEC) == -1)
        {
            ::close(new_fd);
            complete(errno, 0);
            return;
        }

        // Set SO_NOSIGPIPE (macOS/BSD only)
        int one = 1;
        if (::setsockopt(new_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) < 0)
        {
            ::close(new_fd);
            complete(errno, 0);
            return;
        }

        accepted_fd = new_fd;
        complete(0, 0);
    }
    else
    {
        complete(errno, 0);
    }
}

void
kqueue_accept_op::
cancel() noexcept
{
    if (acceptor_impl_)
        acceptor_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

void
kqueue_accept_op::
operator()()
{
    stop_cb.reset();

    bool success = (errn == 0 && !cancelled.load(std::memory_order_acquire));

    if (ec_out)
    {
        if (cancelled.load(std::memory_order_acquire))
            *ec_out = capy::error::canceled;
        else if (errn != 0)
            *ec_out = make_err(errn);
        else
            *ec_out = {};
    }

    if (success && accepted_fd >= 0)
    {
        if (acceptor_impl_)
        {
            auto* socket_svc = static_cast<kqueue_acceptor_impl*>(acceptor_impl_)
                ->service().socket_service();
            if (socket_svc)
            {
                auto& impl = static_cast<kqueue_socket_impl&>(socket_svc->create_impl());
                impl.set_socket(accepted_fd);

                // Register accepted socket with kqueue (edge-triggered mode)
                impl.desc_data_.fd = accepted_fd;
                impl.desc_data_.read_op.store(nullptr, std::memory_order_relaxed);
                impl.desc_data_.write_op.store(nullptr, std::memory_order_relaxed);
                impl.desc_data_.connect_op.store(nullptr, std::memory_order_relaxed);
                socket_svc->scheduler().register_descriptor(accepted_fd, &impl.desc_data_);

                sockaddr_in local_addr{};
                socklen_t local_len = sizeof(local_addr);
                sockaddr_in remote_addr{};
                socklen_t remote_len = sizeof(remote_addr);

                endpoint local_ep, remote_ep;
                if (::getsockname(accepted_fd, reinterpret_cast<sockaddr*>(&local_addr), &local_len) == 0)
                    local_ep = from_sockaddr_in(local_addr);
                if (::getpeername(accepted_fd, reinterpret_cast<sockaddr*>(&remote_addr), &remote_len) == 0)
                    remote_ep = from_sockaddr_in(remote_addr);

                impl.set_endpoints(local_ep, remote_ep);

                if (impl_out)
                    *impl_out = &impl;

                accepted_fd = -1;
            }
            else
            {
                if (ec_out && !*ec_out)
                    *ec_out = make_err(ENOENT);
                ::close(accepted_fd);
                accepted_fd = -1;
                if (impl_out)
                    *impl_out = nullptr;
            }
        }
        else
        {
            ::close(accepted_fd);
            accepted_fd = -1;
            if (impl_out)
                *impl_out = nullptr;
        }
    }
    else
    {
        if (accepted_fd >= 0)
        {
            ::close(accepted_fd);
            accepted_fd = -1;
        }

        if (peer_impl)
        {
            peer_impl->release();
            peer_impl = nullptr;
        }

        if (impl_out)
            *impl_out = nullptr;
    }

    // Move to stack before resuming. See kqueue_op::operator()() for rationale.
    capy::executor_ref saved_ex( std::move( ex ) );
    capy::coro saved_h( std::move( h ) );
    auto prevent_premature_destruction = std::move(impl_ptr);
    saved_ex.dispatch( saved_h );
}

kqueue_acceptor_impl::
kqueue_acceptor_impl(kqueue_acceptor_service& svc) noexcept
    : svc_(svc)
{
}

void
kqueue_acceptor_impl::
update_kqueue_events() noexcept
{
    svc_.scheduler().update_descriptor_events(fd_, &desc_data_, 0);
}

void
kqueue_acceptor_impl::
release()
{
    close_socket();
    svc_.destroy_acceptor_impl(*this);
}

void
kqueue_acceptor_impl::
accept(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    std::stop_token token,
    std::error_code* ec,
    io_object::io_object_impl** impl_out)
{
    auto& op = acc_;
    op.reset();
    op.h = h;
    op.ex = ex;
    op.ec_out = ec;
    op.impl_out = impl_out;
    op.fd = fd_;
    op.start(token, this);

    sockaddr_in addr{};
    socklen_t addrlen = sizeof(addr);

    // Use accept() instead of accept4() (not available on macOS)
    int accepted = ::accept(fd_, reinterpret_cast<sockaddr*>(&addr), &addrlen);

    if (accepted >= 0)
    {
        // Set non-blocking
        int flags = ::fcntl(accepted, F_GETFL, 0);
        if (flags != -1)
            ::fcntl(accepted, F_SETFL, flags | O_NONBLOCK);

        // Set close-on-exec
        ::fcntl(accepted, F_SETFD, FD_CLOEXEC);

        // Set SO_NOSIGPIPE
        int one = 1;
        ::setsockopt(accepted, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));

        desc_data_.read_ready.store(false, std::memory_order_relaxed);
        op.accepted_fd = accepted;
        op.complete(0, 0);
        op.impl_ptr = shared_from_this();
        svc_.post(&op);
        return;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
        svc_.work_started();
        op.impl_ptr = shared_from_this();

        desc_data_.read_op.store(&op, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (desc_data_.read_ready.exchange(false, std::memory_order_acquire))
        {
            auto* claimed = desc_data_.read_op.exchange(nullptr, std::memory_order_acq_rel);
            if (claimed)
            {
                claimed->perform_io();
                if (claimed->errn == EAGAIN || claimed->errn == EWOULDBLOCK)
                {
                    claimed->errn = 0;
                    desc_data_.read_op.store(claimed, std::memory_order_release);
                }
                else
                {
                    svc_.post(claimed);
                    svc_.work_finished();
                }
                return;
            }
        }

        if (op.cancelled.load(std::memory_order_acquire))
        {
            auto* claimed = desc_data_.read_op.exchange(nullptr, std::memory_order_acq_rel);
            if (claimed)
            {
                svc_.post(claimed);
                svc_.work_finished();
            }
        }
        return;
    }

    op.complete(errno, 0);
    op.impl_ptr = shared_from_this();
    svc_.post(&op);
}

void
kqueue_acceptor_impl::
cancel() noexcept
{
    std::shared_ptr<kqueue_acceptor_impl> self;
    try {
        self = shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        return;
    }

    acc_.request_cancel();
    // Use atomic exchange - only one of cancellation or reactor will succeed
    auto* claimed = desc_data_.read_op.exchange(nullptr, std::memory_order_acq_rel);
    if (claimed == &acc_)
    {
        acc_.impl_ptr = self;
        svc_.post(&acc_);
        svc_.work_finished();
    }
}

void
kqueue_acceptor_impl::
cancel_single_op(kqueue_op& op) noexcept
{
    op.request_cancel();

    // Use atomic exchange - only one of cancellation or reactor will succeed
    auto* claimed = desc_data_.read_op.exchange(nullptr, std::memory_order_acq_rel);
    if (claimed == &op)
    {
        try {
            op.impl_ptr = shared_from_this();
        } catch (const std::bad_weak_ptr&) {}
        svc_.post(&op);
        svc_.work_finished();
    }
}

void
kqueue_acceptor_impl::
close_socket() noexcept
{
    cancel();

    if (fd_ >= 0)
    {
        if (desc_data_.registered_events != 0)
            svc_.scheduler().deregister_descriptor(fd_);
        ::close(fd_);
        fd_ = -1;
    }

    desc_data_.fd = -1;
    desc_data_.is_registered = false;
    desc_data_.read_op.store(nullptr, std::memory_order_relaxed);
    desc_data_.read_ready.store(false, std::memory_order_relaxed);
    desc_data_.write_ready.store(false, std::memory_order_relaxed);
    desc_data_.registered_events = 0;

    // Clear cached endpoint
    local_endpoint_ = endpoint{};
}

kqueue_acceptor_service::
kqueue_acceptor_service(capy::execution_context& ctx)
    : ctx_(ctx)
    , state_(std::make_unique<kqueue_acceptor_state>(ctx.use_service<kqueue_scheduler>()))
{
}

kqueue_acceptor_service::
~kqueue_acceptor_service()
{
}

void
kqueue_acceptor_service::
shutdown()
{
    std::lock_guard lock(state_->mutex_);

    while (auto* impl = state_->acceptor_list_.pop_front())
        impl->close_socket();

    state_->acceptor_ptrs_.clear();
}

tcp_acceptor::acceptor_impl&
kqueue_acceptor_service::
create_acceptor_impl()
{
    auto impl = std::make_shared<kqueue_acceptor_impl>(*this);
    auto* raw = impl.get();

    std::lock_guard lock(state_->mutex_);
    state_->acceptor_list_.push_back(raw);
    state_->acceptor_ptrs_.emplace(raw, std::move(impl));

    return *raw;
}

void
kqueue_acceptor_service::
destroy_acceptor_impl(tcp_acceptor::acceptor_impl& impl)
{
    auto* kqueue_impl = static_cast<kqueue_acceptor_impl*>(&impl);
    std::lock_guard lock(state_->mutex_);
    state_->acceptor_list_.remove(kqueue_impl);
    state_->acceptor_ptrs_.erase(kqueue_impl);
}

std::error_code
kqueue_acceptor_service::
open_acceptor(
    tcp_acceptor::acceptor_impl& impl,
    endpoint ep,
    int backlog)
{
    auto* kqueue_impl = static_cast<kqueue_acceptor_impl*>(&impl);
    kqueue_impl->close_socket();

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return make_err(errno);

    // Set non-blocking
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }

    // Set close-on-exec
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }

    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr = detail::to_sockaddr_in(ep);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }

    if (::listen(fd, backlog) < 0)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }

    kqueue_impl->fd_ = fd;

    // Register fd with kqueue (edge-triggered mode)
    kqueue_impl->desc_data_.fd = fd;
    kqueue_impl->desc_data_.read_op.store(nullptr, std::memory_order_relaxed);
    scheduler().register_descriptor(fd, &kqueue_impl->desc_data_);

    // Cache the local endpoint (queries OS for ephemeral port if port was 0)
    sockaddr_in local_addr{};
    socklen_t local_len = sizeof(local_addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local_addr), &local_len) == 0)
        kqueue_impl->set_local_endpoint(detail::from_sockaddr_in(local_addr));

    return {};
}

void
kqueue_acceptor_service::
post(kqueue_op* op)
{
    state_->sched_.post(op);
}

void
kqueue_acceptor_service::
work_started() noexcept
{
    state_->sched_.work_started();
}

void
kqueue_acceptor_service::
work_finished() noexcept
{
    state_->sched_.work_finished();
}

kqueue_socket_service*
kqueue_acceptor_service::
socket_service() const noexcept
{
    auto* svc = ctx_.find_service<detail::socket_service>();
    return svc ? dynamic_cast<kqueue_socket_service*>(svc) : nullptr;
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE
