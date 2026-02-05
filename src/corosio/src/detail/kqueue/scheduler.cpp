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

#include "src/detail/kqueue/scheduler.hpp"
#include "src/detail/kqueue/op.hpp"
#include "src/detail/make_err.hpp"
#include "src/detail/posix/resolver_service.hpp"
#include "src/detail/posix/signals.hpp"

#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/thread_local_ptr.hpp>

#include <atomic>
#include <chrono>
#include <limits>

#include <errno.h>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/*
    kqueue Scheduler - Single Reactor Model
    ========================================

    This scheduler uses a thread coordination strategy to provide handler
    parallelism and avoid the thundering herd problem.
    Instead of all threads blocking on kevent(), one thread becomes the
    "reactor" while others wait on a condition variable for handler work.

    Thread Model
    ------------
    - ONE thread runs kevent() at a time (the reactor thread)
    - OTHER threads wait on wakeup_event_ (condition variable) for handlers
    - When work is posted, exactly one waiting thread wakes via notify_one()
    - This matches Windows IOCP semantics where N posted items wake N threads

    Event Loop Structure (do_one)
    -----------------------------
    1. Lock mutex, try to pop handler from queue
    2. If got handler: execute it (unlocked), return
    3. If queue empty and no reactor running: become reactor
       - Run kevent (unlocked), queue I/O completions, loop back
    4. If queue empty and reactor running: wait on condvar for work

    The reactor_running_ flag ensures only one thread owns kevent().
    After the reactor queues I/O completions, it loops back to try getting
    a handler, giving priority to handler execution over more I/O polling.

    Wake Coordination (wake_one_thread_and_unlock)
    ----------------------------------------------
    When posting work:
    - If idle threads exist: notify_one() wakes exactly one worker
    - Else if reactor running: interrupt via EVFILT_USER trigger
    - Else: no-op (thread will find work when it checks queue)

    This is critical for matching IOCP behavior. With the old model, posting
    N handlers would wake all threads (thundering herd). Now each post()
    wakes at most one thread, and that thread handles exactly one item.

    Work Counting
    -------------
    outstanding_work_ tracks pending operations. When it hits zero, run()
    returns. Each operation increments on start, decrements on completion.

    Timer Integration
    -----------------
    Timers are handled by timer_service. The reactor adjusts kevent
    timeout to wake for the nearest timer expiry. When a new timer is
    scheduled earlier than current, timer_service calls interrupt_reactor()
    to re-evaluate the timeout.
*/

namespace boost::corosio::detail {

namespace {

constexpr uintptr_t KQUEUE_WAKEUP_IDENT = 0;

struct scheduler_context
{
    kqueue_scheduler const* key;
    scheduler_context* next;
};

corosio::detail::thread_local_ptr<scheduler_context> context_stack;

struct thread_context_guard
{
    scheduler_context frame_;

    explicit thread_context_guard(
        kqueue_scheduler const* ctx) noexcept
        : frame_{ctx, context_stack.get()}
    {
        context_stack.set(&frame_);
    }

    ~thread_context_guard() noexcept
    {
        context_stack.set(frame_.next);
    }
};

} // namespace

kqueue_scheduler::
kqueue_scheduler(
    capy::execution_context& ctx,
    int)
    : kq_(-1)
    , outstanding_work_(0)
    , stopped_(false)
    , shutdown_(false)
    , reactor_running_(false)
    , reactor_interrupted_(false)
    , idle_thread_count_(0)
{
    kq_ = ::kqueue();
    if (kq_ < 0)
        detail::throw_system_error(make_err(errno), "kqueue");

    // Set close-on-exec on kqueue fd
    if (::fcntl(kq_, F_SETFD, FD_CLOEXEC) < 0)
    {
        int errn = errno;
        ::close(kq_);
        detail::throw_system_error(make_err(errn), "fcntl FD_CLOEXEC");
    }

    // Register EVFILT_USER for reactor wakeup
    struct kevent ev;
    EV_SET(&ev, KQUEUE_WAKEUP_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (::kevent(kq_, &ev, 1, nullptr, 0, nullptr) < 0)
    {
        int errn = errno;
        ::close(kq_);
        detail::throw_system_error(make_err(errn), "kevent EVFILT_USER");
    }

    timer_svc_ = &get_timer_service(ctx, *this);
    timer_svc_->set_on_earliest_changed(
        timer_service::callback(
            this,
            [](void* p) { static_cast<kqueue_scheduler*>(p)->interrupt_reactor(); }));

    // Initialize resolver service
    get_resolver_service(ctx, *this);

    // Initialize signal service
    get_signal_service(ctx, *this);

    // Push task sentinel to interleave reactor runs with handler execution
    completed_ops_.push(&task_op_);
}

kqueue_scheduler::
~kqueue_scheduler()
{
    if (kq_ >= 0)
        ::close(kq_);
}

void
kqueue_scheduler::
shutdown()
{
    {
        std::unique_lock lock(mutex_);
        shutdown_ = true;

        while (auto* h = completed_ops_.pop())
        {
            if (h == &task_op_)
                continue;
            lock.unlock();
            h->destroy();
            lock.lock();
        }
    }

    outstanding_work_.store(0, std::memory_order_release);

    interrupt_reactor();

    wakeup_event_.notify_all();
}

void
kqueue_scheduler::
post(capy::coro h) const
{
    struct post_handler final
        : scheduler_op
    {
        capy::coro h_;

        explicit
        post_handler(capy::coro h)
            : h_(h)
        {
        }

        ~post_handler() = default;

        void operator()() override
        {
            auto h = h_;
            delete this;
            std::atomic_thread_fence(std::memory_order_acquire);
            h.resume();
        }

        void destroy() override
        {
            delete this;
        }
    };

    auto ph = std::make_unique<post_handler>(h);
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);

    std::unique_lock lock(mutex_);
    completed_ops_.push(ph.release());
    wake_one_thread_and_unlock(lock);
}

void
kqueue_scheduler::
post(scheduler_op* h) const
{
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);

    std::unique_lock lock(mutex_);
    completed_ops_.push(h);
    wake_one_thread_and_unlock(lock);
}

void
kqueue_scheduler::
on_work_started() noexcept
{
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);
}

void
kqueue_scheduler::
on_work_finished() noexcept
{
    if (outstanding_work_.fetch_sub(1, std::memory_order_acq_rel) == 1)
        stop();
}

bool
kqueue_scheduler::
running_in_this_thread() const noexcept
{
    for (auto* c = context_stack.get(); c != nullptr; c = c->next)
        if (c->key == this)
            return true;
    return false;
}

void
kqueue_scheduler::
stop()
{
    bool expected = false;
    if (stopped_.compare_exchange_strong(expected, true,
            std::memory_order_release, std::memory_order_relaxed))
    {
        // Wake all threads so they notice stopped_ and exit
        {
            std::lock_guard lock(mutex_);
            wakeup_event_.notify_all();
        }
        interrupt_reactor();
    }
}

bool
kqueue_scheduler::
stopped() const noexcept
{
    return stopped_.load(std::memory_order_acquire);
}

void
kqueue_scheduler::
restart()
{
    stopped_.store(false, std::memory_order_release);
}

std::size_t
kqueue_scheduler::
run()
{
    if (stopped_.load(std::memory_order_acquire))
        return 0;

    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    thread_context_guard ctx(this);

    std::size_t n = 0;
    while (do_one(-1))
        if (n != (std::numeric_limits<std::size_t>::max)())
            ++n;
    return n;
}

std::size_t
kqueue_scheduler::
run_one()
{
    if (stopped_.load(std::memory_order_acquire))
        return 0;

    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    thread_context_guard ctx(this);
    return do_one(-1);
}

std::size_t
kqueue_scheduler::
wait_one(long usec)
{
    if (stopped_.load(std::memory_order_acquire))
        return 0;

    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    thread_context_guard ctx(this);
    return do_one(usec);
}

std::size_t
kqueue_scheduler::
poll()
{
    if (stopped_.load(std::memory_order_acquire))
        return 0;

    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    thread_context_guard ctx(this);

    std::size_t n = 0;
    while (do_one(0))
        if (n != (std::numeric_limits<std::size_t>::max)())
            ++n;
    return n;
}

std::size_t
kqueue_scheduler::
poll_one()
{
    if (stopped_.load(std::memory_order_acquire))
        return 0;

    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    thread_context_guard ctx(this);
    return do_one(0);
}

void
kqueue_scheduler::
register_descriptor(int fd, descriptor_data* desc) const
{
    // Caller must initialize desc fields before calling this function
    // with a release fence to ensure visibility

    struct kevent changes[2];
    EV_SET(&changes[0], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, desc);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, desc);

    if (::kevent(kq_, changes, 2, nullptr, 0, nullptr) < 0)
        detail::throw_system_error(make_err(errno), "kevent register");

    desc->is_registered.store(true, std::memory_order_release);
}

void
kqueue_scheduler::
update_descriptor_events(int, descriptor_data*, std::uint32_t) const
{
    // Provides memory fence for operation pointer visibility across threads
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

void
kqueue_scheduler::
deregister_descriptor(int fd) const
{
    struct kevent changes[2];
    EV_SET(&changes[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    ::kevent(kq_, changes, 2, nullptr, 0, nullptr);
    // Ignore errors - fd might already be closed
}

void
kqueue_scheduler::
work_started() const noexcept
{
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);
}

void
kqueue_scheduler::
work_finished() const noexcept
{
    if (outstanding_work_.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        // Last work item completed - wake all threads so they can exit.
        // notify_all() wakes threads waiting on the condvar.
        // interrupt_reactor() wakes the reactor thread blocked in kevent().
        // Both are needed because they target different blocking mechanisms.
        std::unique_lock lock(mutex_);
        wakeup_event_.notify_all();
        if (reactor_running_.load(std::memory_order_acquire) &&
            !reactor_interrupted_.load(std::memory_order_acquire))
        {
            reactor_interrupted_.store(true, std::memory_order_release);
            lock.unlock();
            interrupt_reactor();
        }
    }
}

void
kqueue_scheduler::
interrupt_reactor() const
{
    // Only trigger if not already armed to avoid redundant triggers
    bool expected = false;
    if (wakeup_armed_.compare_exchange_strong(expected, true,
            std::memory_order_release, std::memory_order_relaxed))
    {
        struct kevent ev;
        EV_SET(&ev, KQUEUE_WAKEUP_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
        [[maybe_unused]] auto r = ::kevent(kq_, &ev, 1, nullptr, 0, nullptr);
    }
}

void
kqueue_scheduler::
wake_one_thread_and_unlock(std::unique_lock<std::mutex>& lock) const
{
    if (idle_thread_count_ > 0)
    {
        wakeup_event_.notify_one();
        lock.unlock();
    }
    else if (reactor_running_.load(std::memory_order_acquire) &&
             !reactor_interrupted_.load(std::memory_order_acquire))
    {
        reactor_interrupted_.store(true, std::memory_order_release);
        lock.unlock();
        interrupt_reactor();
    }
    else
    {
        lock.unlock();
    }
}

struct work_guard
{
    kqueue_scheduler const* self;
    ~work_guard() { self->work_finished(); }
};

void
kqueue_scheduler::
run_reactor(std::unique_lock<std::mutex>& lock)
{
    // Convert timeout from timer_service to timespec
    struct timespec* ts_ptr = nullptr;
    struct timespec ts;

    if (reactor_interrupted_.load(std::memory_order_acquire))
    {
        // Poll only
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
        ts_ptr = &ts;
    }
    else
    {
        auto nearest = timer_svc_->nearest_expiry();
        if (nearest == timer_service::time_point::max())
        {
            // Block indefinitely
            ts_ptr = nullptr;
        }
        else
        {
            auto now = std::chrono::steady_clock::now();
            if (nearest <= now)
            {
                // Immediate timeout
                ts.tv_sec = 0;
                ts.tv_nsec = 0;
                ts_ptr = &ts;
            }
            else
            {
                auto usec = std::chrono::duration_cast<std::chrono::microseconds>(
                    nearest - now).count();
                ts.tv_sec = usec / 1000000;
                ts.tv_nsec = (usec % 1000000) * 1000;
                ts_ptr = &ts;
            }
        }
    }

    lock.unlock();

    struct kevent events[128];
    int nev = ::kevent(kq_, nullptr, 0, events, 128, ts_ptr);
    int saved_errno = errno;

    timer_svc_->process_expired();

    if (nev < 0 && saved_errno != EINTR)
        detail::throw_system_error(make_err(saved_errno), "kevent");

    lock.lock();

    int completions_queued = 0;
    for (int i = 0; i < nev; ++i)
    {
        // Skip wakeup event
        if (events[i].filter == EVFILT_USER && events[i].ident == KQUEUE_WAKEUP_IDENT)
        {
            wakeup_armed_.store(false, std::memory_order_relaxed);
            continue;
        }

        auto* desc = static_cast<descriptor_data*>(events[i].udata);

        // Skip events for descriptors that have been deregistered
        // This can happen if close_socket() was called after kevent() returned
        if (!desc->is_registered.load(std::memory_order_acquire))
            continue;

        int err = 0;

        // Error handling: check EV_ERROR and EV_EOF
        if (events[i].flags & EV_ERROR)
        {
            err = static_cast<int>(events[i].data);
            if (err == 0) err = EIO;
        }
        else if (events[i].flags & EV_EOF && events[i].fflags != 0)
        {
            err = static_cast<int>(events[i].fflags);
            if (err == 0) err = EIO;
        }

        // For reliability, call getsockopt(SO_ERROR) if error detected
        if (err == 0 && (events[i].flags & (EV_ERROR | EV_EOF)))
        {
            socklen_t len = sizeof(err);
            if (::getsockopt(desc->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
                err = errno;
            if (err == 0 && (events[i].flags & EV_ERROR))
                err = EIO;
        }

        if (events[i].filter == EVFILT_READ)
        {
            auto* op = desc->read_op.exchange(nullptr, std::memory_order_seq_cst);
            if (op)
            {
                if (err)
                {
                    op->complete(err, 0);
                    completed_ops_.push(op);
                    ++completions_queued;
                }
                else
                {
                    op->perform_io();
                    if (op->errn == EAGAIN || op->errn == EWOULDBLOCK)
                    {
                        op->errn = 0;
                        // Only re-register if descriptor is still active
                        if (desc->is_registered.load(std::memory_order_acquire))
                            desc->read_op.store(op, std::memory_order_seq_cst);
                        else
                        {
                            // Descriptor was closed, complete with cancellation
                            op->complete(ECANCELED, 0);
                            completed_ops_.push(op);
                            ++completions_queued;
                        }
                    }
                    else
                    {
                        completed_ops_.push(op);
                        ++completions_queued;
                    }
                }
            }
            else
            {
                // Cache the ready state for later operation registration
                desc->read_ready.store(true, std::memory_order_seq_cst);

                // Double-check: an operation might have been registered between
                // our exchange and setting ready. If so, we must process it now
                // since this edge event won't be delivered again.
                op = desc->read_op.exchange(nullptr, std::memory_order_seq_cst);
                if (op)
                {
                    desc->read_ready.store(false, std::memory_order_seq_cst);
                    if (err)
                    {
                        op->complete(err, 0);
                        completed_ops_.push(op);
                        ++completions_queued;
                    }
                    else
                    {
                        op->perform_io();
                        if (op->errn == EAGAIN || op->errn == EWOULDBLOCK)
                        {
                            op->errn = 0;
                            if (desc->is_registered.load(std::memory_order_acquire))
                                desc->read_op.store(op, std::memory_order_seq_cst);
                            else
                            {
                                op->complete(ECANCELED, 0);
                                completed_ops_.push(op);
                                ++completions_queued;
                            }
                        }
                        else
                        {
                            completed_ops_.push(op);
                            ++completions_queued;
                        }
                    }
                }
            }
        }

        if (events[i].filter == EVFILT_WRITE)
        {
            // Connect uses write readiness - try it first
            auto* conn_op = desc->connect_op.exchange(nullptr, std::memory_order_seq_cst);
            if (conn_op)
            {
                if (err)
                {
                    conn_op->complete(err, 0);
                    completed_ops_.push(conn_op);
                    ++completions_queued;
                }
                else
                {
                    conn_op->perform_io();
                    if (conn_op->errn == EAGAIN || conn_op->errn == EWOULDBLOCK)
                    {
                        conn_op->errn = 0;
                        if (desc->is_registered.load(std::memory_order_acquire))
                            desc->connect_op.store(conn_op, std::memory_order_seq_cst);
                        else
                        {
                            conn_op->complete(ECANCELED, 0);
                            completed_ops_.push(conn_op);
                            ++completions_queued;
                        }
                    }
                    else
                    {
                        completed_ops_.push(conn_op);
                        ++completions_queued;
                    }
                }
            }

            auto* write_op = desc->write_op.exchange(nullptr, std::memory_order_seq_cst);
            if (write_op)
            {
                if (err)
                {
                    write_op->complete(err, 0);
                    completed_ops_.push(write_op);
                    ++completions_queued;
                }
                else
                {
                    write_op->perform_io();
                    if (write_op->errn == EAGAIN || write_op->errn == EWOULDBLOCK)
                    {
                        write_op->errn = 0;
                        if (desc->is_registered.load(std::memory_order_acquire))
                            desc->write_op.store(write_op, std::memory_order_seq_cst);
                        else
                        {
                            write_op->complete(ECANCELED, 0);
                            completed_ops_.push(write_op);
                            ++completions_queued;
                        }
                    }
                    else
                    {
                        completed_ops_.push(write_op);
                        ++completions_queued;
                    }
                }
            }

            if (!conn_op && !write_op)
            {
                // Cache the ready state for later operation registration
                desc->write_ready.store(true, std::memory_order_seq_cst);

                // Double-check: operations might have been registered between
                // our exchanges and setting ready. If so, we must process them
                // since this edge event won't be delivered again.
                conn_op = desc->connect_op.exchange(nullptr, std::memory_order_seq_cst);
                if (conn_op)
                {
                    desc->write_ready.store(false, std::memory_order_seq_cst);
                    if (err)
                    {
                        conn_op->complete(err, 0);
                        completed_ops_.push(conn_op);
                        ++completions_queued;
                    }
                    else
                    {
                        conn_op->perform_io();
                        if (conn_op->errn == EAGAIN || conn_op->errn == EWOULDBLOCK)
                        {
                            conn_op->errn = 0;
                            if (desc->is_registered.load(std::memory_order_acquire))
                                desc->connect_op.store(conn_op, std::memory_order_seq_cst);
                            else
                            {
                                conn_op->complete(ECANCELED, 0);
                                completed_ops_.push(conn_op);
                                ++completions_queued;
                            }
                        }
                        else
                        {
                            completed_ops_.push(conn_op);
                            ++completions_queued;
                        }
                    }
                }

                write_op = desc->write_op.exchange(nullptr, std::memory_order_seq_cst);
                if (write_op)
                {
                    desc->write_ready.store(false, std::memory_order_seq_cst);
                    if (err)
                    {
                        write_op->complete(err, 0);
                        completed_ops_.push(write_op);
                        ++completions_queued;
                    }
                    else
                    {
                        write_op->perform_io();
                        if (write_op->errn == EAGAIN || write_op->errn == EWOULDBLOCK)
                        {
                            write_op->errn = 0;
                            if (desc->is_registered.load(std::memory_order_acquire))
                                desc->write_op.store(write_op, std::memory_order_seq_cst);
                            else
                            {
                                write_op->complete(ECANCELED, 0);
                                completed_ops_.push(write_op);
                                ++completions_queued;
                            }
                        }
                        else
                        {
                            completed_ops_.push(write_op);
                            ++completions_queued;
                        }
                    }
                }
            }
        }

        // Handle error for ops not processed above
        if (err && events[i].filter != EVFILT_READ && events[i].filter != EVFILT_WRITE)
        {
            auto* read_op = desc->read_op.exchange(nullptr, std::memory_order_seq_cst);
            if (read_op)
            {
                read_op->complete(err, 0);
                completed_ops_.push(read_op);
                ++completions_queued;
            }

            auto* write_op = desc->write_op.exchange(nullptr, std::memory_order_seq_cst);
            if (write_op)
            {
                write_op->complete(err, 0);
                completed_ops_.push(write_op);
                ++completions_queued;
            }

            auto* conn_op = desc->connect_op.exchange(nullptr, std::memory_order_seq_cst);
            if (conn_op)
            {
                conn_op->complete(err, 0);
                completed_ops_.push(conn_op);
                ++completions_queued;
            }
        }
    }

    if (completions_queued > 0)
    {
        if (completions_queued == 1)
            wakeup_event_.notify_one();
        else
            wakeup_event_.notify_all();
    }
}

std::size_t
kqueue_scheduler::
do_one(long timeout_us)
{
    std::unique_lock lock(mutex_);

    for (;;)
    {
        if (stopped_.load(std::memory_order_acquire))
            return 0;

        scheduler_op* op = completed_ops_.pop();

        if (op == &task_op_)
        {
            bool more_handlers = !completed_ops_.empty();

            if (!more_handlers)
            {
                if (outstanding_work_.load(std::memory_order_acquire) == 0)
                {
                    completed_ops_.push(&task_op_);
                    return 0;
                }
                if (timeout_us == 0)
                {
                    completed_ops_.push(&task_op_);
                    return 0;
                }
            }

            reactor_interrupted_.store(more_handlers || timeout_us == 0, std::memory_order_release);
            reactor_running_.store(true, std::memory_order_release);

            if (more_handlers && idle_thread_count_ > 0)
                wakeup_event_.notify_one();

            run_reactor(lock);

            reactor_running_.store(false, std::memory_order_release);
            completed_ops_.push(&task_op_);
            continue;
        }

        if (op != nullptr)
        {
            lock.unlock();
            work_guard g{this};
            (*op)();
            return 1;
        }

        if (outstanding_work_.load(std::memory_order_acquire) == 0)
            return 0;

        if (timeout_us == 0)
            return 0;

        ++idle_thread_count_;
        if (timeout_us < 0)
            wakeup_event_.wait(lock);
        else
            wakeup_event_.wait_for(lock, std::chrono::microseconds(timeout_us));
        --idle_thread_count_;
    }
}

} // namespace boost::corosio::detail

#endif
