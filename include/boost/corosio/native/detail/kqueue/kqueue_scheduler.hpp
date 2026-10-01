//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_SCHEDULER_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_SCHEDULER_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <boost/corosio/native/detail/reactor/reactor_scheduler.hpp>
#include <boost/corosio/native/detail/reactor/reactor_signal_pipe.hpp>

#include <boost/corosio/native/detail/kqueue/kqueue_traits.hpp>
#include <boost/corosio/detail/timer_service.hpp>
#include <boost/corosio/native/detail/make_err.hpp>

#include <boost/corosio/detail/except.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

namespace boost::corosio::detail {

struct kqueue_op;

/** macOS/BSD scheduler using kqueue for I/O multiplexing.

    This scheduler implements the scheduler interface using the BSD kqueue
    API for efficient I/O event notification. It uses a single reactor model
    where one thread runs kevent() while other threads
    wait on a condition variable for handler work. This design provides:

    - Handler parallelism: N posted handlers can execute on N threads
    - No thundering herd: condition_variable wakes exactly one thread
    - IOCP parity: Behavior matches Windows I/O completion port semantics

    When threads call run(), they first try to execute queued handlers.
    If the queue is empty and no reactor is running, one thread becomes
    the reactor and runs kevent(). Other threads wait on a condition
    variable until handlers are available.

    kqueue uses EV_CLEAR for edge-triggered semantics (equivalent to
    epoll's EPOLLET). File descriptors are registered once with both
    EVFILT_READ and EVFILT_WRITE and stay registered until closed.

    @par Thread Safety
    All public member functions are thread-safe.
*/
class BOOST_COROSIO_DECL kqueue_scheduler final : public reactor_scheduler
{
public:
    /** Construct the scheduler.

        Creates a kqueue file descriptor via kqueue(), sets
        close-on-exec, and registers EVFILT_USER for reactor
        interruption. On failure the kqueue fd is closed before
        throwing.

        @param ctx Reference to the owning execution_context.
        @param concurrency_hint Hint for expected thread count (unused).

        @throws std::system_error if kqueue() fails, if setting
            FD_CLOEXEC on the kqueue fd fails, or if registering
            the EVFILT_USER event fails. The error code contains
            the errno from the failed syscall.
    */
    kqueue_scheduler(capy::execution_context& ctx, int concurrency_hint = -1);

    /** Destructor.

        Closes the kqueue file descriptor if valid. Does not throw.
    */
    ~kqueue_scheduler();

    kqueue_scheduler(kqueue_scheduler const&)            = delete;
    kqueue_scheduler& operator=(kqueue_scheduler const&) = delete;

    /// Shut down the scheduler, draining pending operations.
    void shutdown() override;

    /// Apply runtime configuration, resizing the event buffer.
    void configure_reactor(
        unsigned max_events,
        unsigned budget_init,
        unsigned budget_max,
        unsigned unassisted) override;

    /** Return the kqueue file descriptor.

        Used by socket services to register file descriptors
        for I/O event notification.

        @return The kqueue file descriptor.
    */
    int kq_fd() const noexcept
    {
        return kq_fd_;
    }

    /** Register a descriptor for persistent monitoring.

        Adds EVFILT_READ and EVFILT_WRITE (both EV_CLEAR) for @a fd
        and stores @a desc in the kevent udata field so that the
        reactor can dispatch events to the correct reactor_descriptor_state.

        @param fd The file descriptor to register.
        @param desc Pointer to the caller-owned reactor_descriptor_state.

        @return The error if kevent(EV_ADD) fails, otherwise a default
        constructed error code.
    */
    std::error_code
    register_descriptor(int fd, reactor_descriptor_state* desc) const;

    /** Deregister a persistently registered descriptor.

        Issues kevent(EV_DELETE) for both EVFILT_READ and EVFILT_WRITE.
        Errors are silently ignored because the fd may already be
        closed and kqueue automatically removes closed descriptors.

        @param fd The file descriptor to deregister.
    */
    void deregister_descriptor(int fd) const;

    /// Watch the read end of the POSIX signal self-pipe (see scheduler.hpp).
    [[nodiscard]] std::error_code register_signal_reader(int read_fd) override
    {
        return register_descriptor(read_fd, signal_pipe_reader_.arm());
    }

private:
    void run_task(lock_type& lock, context_type& ctx, long timeout_us) override;
    void interrupt_reactor() const override;
    long calculate_timeout(long requested_timeout_us) const;

    int kq_fd_;

    // Watches the global signal self-pipe's read end (armed lazily by
    // register_signal_reader on the first signal registration).
    reactor_signal_pipe_reader signal_pipe_reader_;

    // EVFILT_USER idempotency
    mutable std::atomic<bool> user_event_armed_{false};

    // Event buffer sized from max_events_per_poll_.
    std::vector<struct kevent> event_buffer_;
};

inline kqueue_scheduler::kqueue_scheduler(capy::execution_context& ctx, int)
    : kq_fd_(-1)
    , event_buffer_(max_events_per_poll_)
{
    kq_fd_ = ::kqueue();
    if (kq_fd_ < 0)
        detail::throw_system_error(make_err(errno), "kqueue");

    if (::fcntl(kq_fd_, F_SETFD, FD_CLOEXEC) == -1)
    {
        int errn = errno;
        ::close(kq_fd_);
        detail::throw_system_error(make_err(errn), "fcntl (kqueue FD_CLOEXEC)");
    }

    struct kevent ev;
    EV_SET(&ev, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (::kevent(kq_fd_, &ev, 1, nullptr, 0, nullptr) < 0)
    {
        int errn = errno;
        ::close(kq_fd_);
        detail::throw_system_error(make_err(errn), "kevent (EVFILT_USER)");
    }

    timer_svc_ = &get_timer_service(ctx, *this);
    timer_svc_->set_on_earliest_changed(
        timer_service::callback(this, [](void* p) {
            static_cast<kqueue_scheduler*>(p)->interrupt_reactor();
        }));

    completed_ops_.push(&task_op_);
}

inline kqueue_scheduler::~kqueue_scheduler()
{
    if (kq_fd_ >= 0)
        ::close(kq_fd_);
}

inline void
kqueue_scheduler::shutdown()
{
    shutdown_drain();

    if (kq_fd_ >= 0)
        interrupt_reactor();
}

inline void
kqueue_scheduler::configure_reactor(
    unsigned max_events,
    unsigned budget_init,
    unsigned budget_max,
    unsigned unassisted)
{
    reactor_scheduler::configure_reactor(
        max_events, budget_init, budget_max, unassisted);
    event_buffer_.resize(max_events_per_poll_);
}

/// Per-filter outcome of kqueue_add_rw; 0 means added.
struct kqueue_add_result
{
    int read_err;
    int write_err;
};

/** Add edge-triggered EVFILT_READ and EVFILT_WRITE for @p fd.

    All or nothing. Without EV_RECEIPT a refused filter stops the
    changelist with the earlier filters still applied, leaving a knote
    whose udata outlives the caller's state.
*/
inline kqueue_add_result
kqueue_add_rw(int kq, int fd, void* udata) noexcept
{
    struct kevent changes[2];
    EV_SET(
        &changes[0], static_cast<uintptr_t>(fd), EVFILT_READ,
        EV_ADD | EV_CLEAR | EV_RECEIPT, 0, 0, udata);
    EV_SET(
        &changes[1], static_cast<uintptr_t>(fd), EVFILT_WRITE,
        EV_ADD | EV_CLEAR | EV_RECEIPT, 0, 0, udata);

    struct kevent receipts[2];
    int const n = ::kevent(kq, changes, 2, receipts, 2, nullptr);
    if (n < 0)
        return {errno, errno};

    kqueue_add_result r{0, 0};
    for (int i = 0; i < n; ++i)
    {
        if (!(receipts[i].flags & EV_ERROR) || receipts[i].data == 0)
            continue;
        int const err = static_cast<int>(receipts[i].data);
        if (receipts[i].filter == EVFILT_READ)
            r.read_err = err;
        else
            r.write_err = err;
    }

    // Exactly one filter failed: delete the one that was added.
    if ((r.read_err == 0) != (r.write_err == 0))
    {
        struct kevent del;
        short const added = r.read_err == 0 ? EVFILT_READ : EVFILT_WRITE;
        EV_SET(
            &del, static_cast<uintptr_t>(fd), added, EV_DELETE, 0, 0, nullptr);
        ::kevent(kq, &del, 1, nullptr, 0, nullptr);
    }
    return r;
}

inline std::error_code
kqueue_scheduler::register_descriptor(
    int fd, reactor_descriptor_state* desc) const
{
    auto r               = kqueue_add_rw(kq_fd_, fd, desc);
    std::uint32_t events = reactor_event_read | reactor_event_write;

    // EPIPE on EVFILT_WRITE: a pipe or FIFO whose peer is gone (an
    // exited child's stdout). A write on it fails at once and never
    // parks, so watch reads alone rather than refuse the descriptor.
    if (r.read_err == 0 && r.write_err == EPIPE)
    {
        struct kevent ch;
        EV_SET(
            &ch, static_cast<uintptr_t>(fd), EVFILT_READ, EV_ADD | EV_CLEAR, 0,
            0, desc);
        r.read_err =
            ::kevent(kq_fd_, &ch, 1, nullptr, 0, nullptr) < 0 ? errno : 0;
        r.write_err = 0;
        events      = reactor_event_read;
    }

    // EINVAL/ENODEV on EVFILT_READ: a device with no kqfilter. As on
    // epoll's EPERM, adopt it unwatched. kqueue_add_rw left nothing
    // registered.
    bool const unpollable = r.read_err == EINVAL || r.read_err == ENODEV;
    if (!unpollable)
    {
        if (r.read_err != 0)
            return make_err(r.read_err);
        if (r.write_err != 0)
            return make_err(r.write_err);
    }
    desc->registered_events = unpollable ? 0 : events;
    desc->unpollable        = unpollable;
    desc->fd                = fd;
    desc->scheduler_        = this;
    desc->mutex.set_enabled(reactor_io_locking_);
    desc->ready_events_.store(0, std::memory_order_relaxed);

    conditionally_enabled_mutex::scoped_lock lock(desc->mutex);
    desc->impl_ref_.reset();
    desc->read_ready  = false;
    desc->write_ready = false;
    return {};
}

inline void
kqueue_scheduler::deregister_descriptor(int fd) const
{
    struct kevent changes[2];
    EV_SET(
        &changes[0], static_cast<uintptr_t>(fd), EVFILT_READ, EV_DELETE, 0, 0,
        nullptr);
    EV_SET(
        &changes[1], static_cast<uintptr_t>(fd), EVFILT_WRITE, EV_DELETE, 0, 0,
        nullptr);
    ::kevent(kq_fd_, changes, 2, nullptr, 0, nullptr);
}

inline void
kqueue_scheduler::interrupt_reactor() const
{
    bool expected = false;
    if (user_event_armed_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        struct kevent ev;
        EV_SET(&ev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
        if (::kevent(kq_fd_, &ev, 1, nullptr, 0, nullptr) < 0)
        {
            // The flag is what coalesces later interrupts into a
            // trigger already queued on the kqueue; a kevent that
            // failed queued nothing, so leaving it armed would swallow
            // every interrupt that follows. Disarming keeps the cost to
            // the interrupts already in flight -- the next one arms and
            // triggers again, instead of every one after this
            // coalescing into a trigger that does not exist.
            user_event_armed_.store(false, std::memory_order_release);
        }
    }
}

inline long
kqueue_scheduler::calculate_timeout(long requested_timeout_us) const
{
    if (requested_timeout_us == 0)
        return 0;

    auto nearest = timer_svc_->nearest_expiry();
    if (nearest == timer_service::time_point::max())
        return requested_timeout_us;

    auto now = std::chrono::steady_clock::now();
    if (nearest <= now)
        return 0;

    auto timer_timeout_us =
        std::chrono::duration_cast<std::chrono::microseconds>(nearest - now)
            .count();

    constexpr auto long_max =
        static_cast<long long>((std::numeric_limits<long>::max)());
    auto capped_timer_us = std::min(
        std::max(timer_timeout_us, static_cast<long long>(0)), long_max);

    if (requested_timeout_us < 0)
        return static_cast<long>(capped_timer_us);

    return static_cast<long>(std::min(
        static_cast<long long>(requested_timeout_us), capped_timer_us));
}

inline void
kqueue_scheduler::run_task(lock_type& lock, context_type& ctx, long timeout_us)
{
    long effective_timeout_us =
        task_interrupted_ ? 0 : calculate_timeout(timeout_us);

    if (lock.owns_lock())
        lock.unlock();

    task_cleanup on_exit{this, &lock, ctx};

    struct timespec ts;
    struct timespec* ts_ptr = nullptr;
    if (effective_timeout_us >= 0)
    {
        ts.tv_sec  = effective_timeout_us / 1000000;
        ts.tv_nsec = (effective_timeout_us % 1000000) * 1000;
        ts_ptr     = &ts;
    }

    int nev = ::kevent(
        kq_fd_, nullptr, 0, event_buffer_.data(),
        static_cast<int>(event_buffer_.size()), ts_ptr);
    int saved_errno = errno;

    if (nev < 0 && saved_errno != EINTR)
        detail::throw_system_error(make_err(saved_errno), "kevent");

    ready_queue local_ops;

    for (int i = 0; i < nev; ++i)
    {
        if (event_buffer_[i].filter == EVFILT_USER)
        {
            user_event_armed_.store(false, std::memory_order_release);
            continue;
        }

        auto* desc =
            static_cast<reactor_descriptor_state*>(event_buffer_[i].udata);
        if (!desc)
            continue;

        std::uint32_t ready = 0;

        if (event_buffer_[i].filter == EVFILT_READ)
            ready |= reactor_event_read;
        else if (event_buffer_[i].filter == EVFILT_WRITE)
            ready |= reactor_event_write;

        if (event_buffer_[i].flags & EV_ERROR)
            ready |= reactor_event_error;

        if (event_buffer_[i].flags & EV_EOF)
        {
            if (event_buffer_[i].filter == EVFILT_READ)
                ready |= reactor_event_read;
            if (event_buffer_[i].fflags != 0)
                ready |= reactor_event_error;
        }

        desc->add_ready_events(ready);

        bool expected = false;
        if (desc->is_enqueued_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            local_ops.push(desc);
        }
    }

    timer_svc_->process_expired();

    lock.lock();

    completed_ops_.splice(local_ops);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE

#endif // BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_SCHEDULER_HPP
