//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SOCKET_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SOCKET_SERVICE_HPP

#include <boost/corosio/io/io_object.hpp>
#include <boost/corosio/detail/scheduler_op.hpp>
#include <boost/corosio/native/detail/reactor/reactor_service_state.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <memory>
#include <mutex>

namespace boost::corosio::detail {

/** CRTP base for reactor-backed socket/datagram service implementations.

    Provides the shared construct/destroy/shutdown/close/post/work
    logic that is identical across all reactor backends and socket
    types. Derived classes add only protocol-specific open/bind.

    @tparam Derived     The concrete service type (CRTP).
    @tparam ServiceBase The abstract service base (tcp_service
                        or udp_service).
    @tparam Scheduler   The backend's scheduler type.
    @tparam Impl        The backend's socket/datagram impl type.
*/
template<class Derived, class ServiceBase, class Scheduler, class Impl>
class reactor_socket_service : public ServiceBase
{
    friend Derived;
    using state_type = reactor_service_state<Scheduler, Impl>;

protected:
    // NOLINTNEXTLINE(bugprone-crtp-constructor-accessibility)
    explicit reactor_socket_service(capy::execution_context& ctx)
        : state_(
              std::make_unique<state_type>(
                  ctx.template use_service<Scheduler>()))
    {
    }

public:
    ~reactor_socket_service() override = default;

    void shutdown() override
    {
        std::lock_guard lock(state_->mutex_);

        while (auto* impl = state_->impl_list_.pop_front())
        {
            static_cast<Derived*>(this)->pre_shutdown(impl);
            impl->close_socket();
            // Park on shutdown_list; ~state_ deletes after scheduler
            // has drained all pending ops.
            state_->shutdown_list_.push_back(impl);
        }
    }

    io_object::implementation* construct() override
    {
        Impl* raw;
        {
            std::lock_guard lock(state_->mutex_);
            raw = state_->freelist_.pop();
            if (raw)
                state_->impl_list_.push_back(raw);
        }
        if (!raw)
        {
            raw = new Impl(static_cast<Derived&>(*this));
            std::lock_guard lock(state_->mutex_);
            state_->impl_list_.push_back(raw);
        }

        raw->ref_count_.store(1, std::memory_order_relaxed);
        raw->release_fn_ = +[](ref_counted_base* p) noexcept {
            auto* impl = static_cast<Impl*>(p);
            static_cast<Derived&>(impl->service()).recycle(impl);
        };

        return raw;
    }

    void destroy(io_object::implementation* impl) override
    {
        auto* typed = static_cast<Impl*>(impl);
        static_cast<Derived*>(this)->pre_destroy(typed);
        typed->close_socket();
        std::lock_guard lock(state_->mutex_);
        state_->impl_list_.remove(typed);
        if (typed->sub_ref())
            state_->freelist_.push(typed);
    }

    void close(io_object::handle& h) override
    {
        static_cast<Impl*>(h.get())->close_socket();
    }

    Scheduler& scheduler() const noexcept
    {
        return state_->sched_;
    }

    void post(scheduler_op* op)
    {
        state_->sched_.post(op);
    }

    void work_started() noexcept
    {
        state_->sched_.work_started();
    }

    void work_finished() noexcept
    {
        state_->sched_.work_finished();
    }

    /// Return an impl to the freelist (called when its refcount hits 0).
    void recycle(Impl* impl)
    {
        std::lock_guard lock(state_->mutex_);
        state_->freelist_.push(impl);
    }

protected:
    // Override in derived to add pre-close logic (e.g. kqueue linger reset)
    void pre_shutdown(Impl*) noexcept {}
    void pre_destroy(Impl*) noexcept {}

    std::unique_ptr<state_type> state_;

private:
    reactor_socket_service(reactor_socket_service const&)            = delete;
    reactor_socket_service& operator=(reactor_socket_service const&) = delete;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_SOCKET_SERVICE_HPP
