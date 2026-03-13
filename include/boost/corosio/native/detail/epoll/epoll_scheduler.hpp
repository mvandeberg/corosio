//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_SCHEDULER_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_SCHEDULER_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/backend.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <boost/corosio/native/native_scheduler.hpp>
#include <boost/corosio/detail/scheduler_op.hpp>

#include <boost/corosio/native/detail/epoll/epoll_op.hpp>
#include <boost/corosio/detail/timer_service.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/native/detail/posix/posix_resolver_service.hpp>
#include <boost/corosio/native/detail/posix/posix_signal_service.hpp>

#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/thread_local_ptr.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <utility>

#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace boost::corosio::detail {

struct epoll_op;
struct descriptor_state;
namespace epoll {
struct scheduler_context;
} // namespace epoll

/* epoll_scheduler_core — non-template base class

    Contains all data members, thread context management, and methods
    that don't depend on epoll_config template parameters. Services
    (epoll_socket_service, epoll_acceptor_service) reference this type
    rather than the templated epoll_scheduler<Config>, avoiding a
    cascade of template parameters through the service layer.

    Config-dependent behavior (event array sizing, post node recycling)
    lives on the derived epoll_scheduler<Config>.
*/

class BOOST_COROSIO_DECL epoll_scheduler_core
    : public native_scheduler
    , public capy::execution_context::service
{
public:
    /** Service lookup key.

        Allows services to find this scheduler via
        ctx.use_service<epoll_scheduler_core>().
    */
    using key_type = epoll_scheduler_core;

    epoll_scheduler_core(epoll_scheduler_core const&)            = delete;
    epoll_scheduler_core& operator=(epoll_scheduler_core const&) = delete;

    /** Return the epoll file descriptor.

        Used by socket services to register file descriptors
        for I/O event notification.

        @return The epoll file descriptor.
    */
    int epoll_fd() const noexcept
    {
        return epoll_fd_;
    }

    /** Reset the thread's inline completion budget.

        Called at the start of each posted completion handler to
        grant a fresh budget for speculative inline completions.
    */
    void reset_inline_budget() const noexcept;

    /** Consume one unit of inline budget if available.

        @return True if budget was available and consumed.
    */
    bool try_consume_inline_budget() const noexcept;

    /** Register a descriptor for persistent monitoring.

        The fd is registered once and stays registered until explicitly
        deregistered. Events are dispatched via descriptor_state which
        tracks pending read/write/connect operations.

        @param fd The file descriptor to register.
        @param desc Pointer to descriptor data (stored in epoll_event.data.ptr).
    */
    void register_descriptor(int fd, descriptor_state* desc) const;

    /** Deregister a persistently registered descriptor.

        @param fd The file descriptor to deregister.
    */
    void deregister_descriptor(int fd) const;

    void work_started() noexcept override;
    void work_finished() noexcept override;

    /** Offset a forthcoming work_finished from work_cleanup.

        Called by descriptor_state when all I/O returned EAGAIN and no
        handler will be executed. Must be called from a scheduler thread.
    */
    void compensating_work_started() const noexcept;

    /** Drain work from thread context's private queue to global queue.

        Called by thread_context_guard destructor when a thread exits run().
        Transfers pending work to the global queue under mutex protection.

        @param queue The private queue to drain.
        @param count Item count for wakeup decisions (wakes other threads if positive).
    */
    void drain_thread_queue(op_queue& queue, long count) const;

    /** Post completed operations for deferred invocation.

        If called from a thread running this scheduler, operations go to
        the thread's private queue (fast path). Otherwise, operations are
        added to the global queue under mutex and a waiter is signaled.

        @par Preconditions
        work_started() must have been called for each operation.

        @param ops Queue of operations to post.
    */
    void post_deferred_completions(op_queue& ops) const;

    // Post handler node recycling (global free list, protected by mutex_)
    scheduler_op* try_acquire_post_node() const;
    void release_post_node(scheduler_op* node) const;

    // scheduler interface overrides (non-Config-dependent)
    void post(scheduler_op* h) const override;
    bool running_in_this_thread() const noexcept override;
    void stop() override;
    bool stopped() const noexcept override;
    void restart() override;

protected:
    epoll_scheduler_core(
        capy::execution_context& ctx,
        int concurrency_hint,
        unsigned budget_initial,
        unsigned budget_max,
        unsigned budget_unassisted);
    ~epoll_scheduler_core() override;

    void shutdown() override;

    // Allow descriptor_state (from epoll_op.hpp) to push itself
    // into local_ops in run_task via op_queue::push(scheduler_op*)
    friend struct descriptor_state;

    struct work_cleanup
    {
        epoll_scheduler_core* scheduler;
        std::unique_lock<std::mutex>* lock;
        epoll::scheduler_context* ctx;
        ~work_cleanup();
    };

    struct task_cleanup
    {
        epoll_scheduler_core const* scheduler;
        std::unique_lock<std::mutex>* lock;
        epoll::scheduler_context* ctx;
        ~task_cleanup();
    };

    void wake_one_thread_and_unlock(std::unique_lock<std::mutex>& lock) const;
    void interrupt_reactor() const;
    void update_timerfd() const;
    void signal_all(std::unique_lock<std::mutex>& lock) const;
    bool maybe_unlock_and_signal_one(std::unique_lock<std::mutex>& lock) const;
    bool unlock_and_signal_one(std::unique_lock<std::mutex>& lock) const;
    void clear_signal() const;
    void wait_for_signal(std::unique_lock<std::mutex>& lock) const;
    void wait_for_signal_for(
        std::unique_lock<std::mutex>& lock, long timeout_us) const;

    // Budget config values (set from Config in derived constructor)
    unsigned budget_initial_;
    unsigned budget_max_;
    unsigned budget_unassisted_;

    // Global free list for post_handler recycling (protected by mutex_)
    mutable scheduler_op* post_free_list_ = nullptr;

    int epoll_fd_;
    int event_fd_; // for interrupting reactor
    int timer_fd_; // timerfd for kernel-managed timer expiry
    mutable std::mutex mutex_;
    mutable std::condition_variable cond_;
    mutable op_queue completed_ops_;
    mutable std::atomic<long> outstanding_work_;
    bool stopped_;

    // True while a thread is blocked in epoll_wait. Used by
    // wake_one_thread_and_unlock and work_finished to know when
    // an eventfd interrupt is needed instead of a condvar signal.
    mutable std::atomic<bool> task_running_{false};

    // True when the reactor has been told to do a non-blocking poll
    // (more handlers queued or poll mode). Prevents redundant eventfd
    // writes and controls the epoll_wait timeout.
    mutable bool task_interrupted_ = false;

    // Signaling state: bit 0 = signaled, upper bits = waiter count (incremented by 2)
    mutable std::size_t state_ = 0;

    // Edge-triggered eventfd state
    mutable std::atomic<bool> eventfd_armed_{false};

    // Set when the earliest timer changes; flushed before epoll_wait
    // blocks. Avoids timerfd_settime syscalls for timers that are
    // scheduled then cancelled without being waited on.
    mutable std::atomic<bool> timerfd_stale_{false};

    // Sentinel operation for interleaving reactor runs with handler execution.
    // Ensures the reactor runs periodically even when handlers are continuously
    // posted, preventing starvation of I/O events, timers, and signals.
    struct task_op final : scheduler_op
    {
        void operator()() override {}
        void destroy() override {}
    };
    task_op task_op_;
};

/** Linux scheduler using epoll for I/O multiplexing.

    This scheduler implements the scheduler interface using Linux epoll
    for efficient I/O event notification. It uses a single reactor model
    where one thread runs epoll_wait while other threads
    wait on a condition variable for handler work. This design provides:

    - Handler parallelism: N posted handlers can execute on N threads
    - No thundering herd: condition_variable wakes exactly one thread
    - IOCP parity: Behavior matches Windows I/O completion port semantics

    When threads call run(), they first try to execute queued handlers.
    If the queue is empty and no reactor is running, one thread becomes
    the reactor and runs epoll_wait. Other threads wait on a condition
    variable until handlers are available.

    @par Thread Safety
    All public member functions are thread-safe.
*/
template<epoll_config Config>
class epoll_scheduler final
    : public epoll_scheduler_core
{
public:
    /** Construct the scheduler.

        Creates an epoll instance, eventfd for reactor interruption,
        and timerfd for kernel-managed timer expiry.

        @param ctx Reference to the owning execution_context.
        @param concurrency_hint Hint for expected thread count (unused).
    */
    epoll_scheduler(capy::execution_context& ctx, int concurrency_hint = -1);

    /// Destroy the scheduler.
    ~epoll_scheduler() override = default;

    epoll_scheduler(epoll_scheduler const&)            = delete;
    epoll_scheduler& operator=(epoll_scheduler const&) = delete;

    using epoll_scheduler_core::post;

    // Config-dependent overrides
    void post(std::coroutine_handle<> h) const override;
    std::size_t run() override;
    std::size_t run_one() override;
    std::size_t wait_one(long usec) override;
    std::size_t poll() override;
    std::size_t poll_one() override;

private:
    std::size_t do_one(
        std::unique_lock<std::mutex>& lock,
        long timeout_us,
        epoll::scheduler_context* ctx);
    void
    run_task(std::unique_lock<std::mutex>& lock, epoll::scheduler_context* ctx);
};

//--------------------------------------------------------------------------
//
// Implementation
//
//--------------------------------------------------------------------------

/*
    epoll Scheduler - Single Reactor Model
    ======================================

    This scheduler uses a thread coordination strategy to provide handler
    parallelism and avoid the thundering herd problem.
    Instead of all threads blocking on epoll_wait(), one thread becomes the
    "reactor" while others wait on a condition variable for handler work.

    Thread Model
    ------------
    - ONE thread runs epoll_wait() at a time (the reactor thread)
    - OTHER threads wait on cond_ (condition variable) for handlers
    - When work is posted, exactly one waiting thread wakes via notify_one()
    - This matches Windows IOCP semantics where N posted items wake N threads

    Event Loop Structure (do_one)
    -----------------------------
    1. Lock mutex, try to pop handler from queue
    2. If got handler: execute it (unlocked), return
    3. If queue empty and no reactor running: become reactor
       - Run epoll_wait (unlocked), queue I/O completions, loop back
    4. If queue empty and reactor running: wait on condvar for work

    The task_running_ flag ensures only one thread owns epoll_wait().
    After the reactor queues I/O completions, it loops back to try getting
    a handler, giving priority to handler execution over more I/O polling.

    Signaling State (state_)
    ------------------------
    The state_ variable encodes two pieces of information:
    - Bit 0: signaled flag (1 = signaled, persists until cleared)
    - Upper bits: waiter count (each waiter adds 2 before blocking)

    This allows efficient coordination:
    - Signalers only call notify when waiters exist (state_ > 1)
    - Waiters check if already signaled before blocking (fast-path)

    Wake Coordination (wake_one_thread_and_unlock)
    ----------------------------------------------
    When posting work:
    - If waiters exist (state_ > 1): signal and notify_one()
    - Else if reactor running: interrupt via eventfd write
    - Else: no-op (thread will find work when it checks queue)

    This avoids waking threads unnecessarily. With cascading wakes,
    each handler execution wakes at most one additional thread if
    more work exists in the queue.

    Work Counting
    -------------
    outstanding_work_ tracks pending operations. When it hits zero, run()
    returns. Each operation increments on start, decrements on completion.

    Timer Integration
    -----------------
    Timers are handled by timer_service. The reactor adjusts epoll_wait
    timeout to wake for the nearest timer expiry. When a new timer is
    scheduled earlier than current, timer_service calls interrupt_reactor()
    to re-evaluate the timeout.
*/

namespace epoll {

/** Per-thread context for a scheduler thread.

    Stores private queue for lock-free fast-path posting and
    inline completion budget state.
*/
struct BOOST_COROSIO_SYMBOL_VISIBLE scheduler_context
{
    void const* key;
    scheduler_context* next;
    op_queue private_queue;
    long private_outstanding_work;
    int inline_budget;
    int inline_budget_max;
    bool unassisted;
    scheduler_op* post_cache = nullptr;

    scheduler_context(void const* k, scheduler_context* n, int budget_initial)
        : key(k)
        , next(n)
        , private_outstanding_work(0)
        , inline_budget(0)
        , inline_budget_max(budget_initial)
        , unassisted(false)
    {
    }
};

inline thread_local_ptr<scheduler_context> context_stack;

struct thread_context_guard
{
    scheduler_context frame_;

    thread_context_guard(
        void const* key, int budget_initial) noexcept
        : frame_(key, context_stack.get(), budget_initial)
    {
        context_stack.set(&frame_);
    }

    ~thread_context_guard() noexcept
    {
        auto* core = static_cast<epoll_scheduler_core const*>(frame_.key);
        if (!frame_.private_queue.empty())
            core->drain_thread_queue(
                frame_.private_queue, frame_.private_outstanding_work);
        if (frame_.post_cache)
            core->release_post_node(frame_.post_cache);
        context_stack.set(frame_.next);
    }
};

inline scheduler_context*
find_context(void const* self) noexcept
{
    for (auto* c = context_stack.get(); c != nullptr; c = c->next)
        if (c->key == self)
            return c;
    return nullptr;
}

} // namespace epoll

//--------------------------------------------------------------------------
// epoll_scheduler_core implementation
//--------------------------------------------------------------------------

inline
epoll_scheduler_core::epoll_scheduler_core(
    capy::execution_context& ctx,
    int,
    unsigned budget_initial,
    unsigned budget_max,
    unsigned budget_unassisted)
    : budget_initial_(budget_initial)
    , budget_max_(budget_max)
    , budget_unassisted_(budget_unassisted)
    , epoll_fd_(-1)
    , event_fd_(-1)
    , timer_fd_(-1)
    , outstanding_work_(0)
    , stopped_(false)
    , task_running_{false}
    , task_interrupted_(false)
    , state_(0)
{
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0)
        detail::throw_system_error(make_err(errno), "epoll_create1");

    event_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd_ < 0)
    {
        int errn = errno;
        ::close(epoll_fd_);
        detail::throw_system_error(make_err(errn), "eventfd");
    }

    timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd_ < 0)
    {
        int errn = errno;
        ::close(event_fd_);
        ::close(epoll_fd_);
        detail::throw_system_error(make_err(errn), "timerfd_create");
    }

    epoll_event ev{};
    ev.events   = EPOLLIN | EPOLLET;
    ev.data.ptr = nullptr;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, event_fd_, &ev) < 0)
    {
        int errn = errno;
        ::close(timer_fd_);
        ::close(event_fd_);
        ::close(epoll_fd_);
        detail::throw_system_error(make_err(errn), "epoll_ctl");
    }

    epoll_event timer_ev{};
    timer_ev.events   = EPOLLIN | EPOLLERR;
    timer_ev.data.ptr = &timer_fd_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, timer_fd_, &timer_ev) < 0)
    {
        int errn = errno;
        ::close(timer_fd_);
        ::close(event_fd_);
        ::close(epoll_fd_);
        detail::throw_system_error(make_err(errn), "epoll_ctl (timerfd)");
    }

    timer_svc_ = &get_timer_service(ctx, *this);
    timer_svc_->set_on_earliest_changed(
        timer_service::callback(this, [](void* p) {
            auto* self = static_cast<epoll_scheduler_core*>(p);
            self->timerfd_stale_.store(true, std::memory_order_release);
            if (self->task_running_.load(std::memory_order_acquire))
                self->interrupt_reactor();
        }));

    // Initialize resolver service
    get_resolver_service(ctx, *this);

    // Initialize signal service
    get_signal_service(ctx, *this);

    // Push task sentinel to interleave reactor runs with handler execution
    completed_ops_.push(&task_op_);
}

inline
epoll_scheduler_core::~epoll_scheduler_core()
{
    if (timer_fd_ >= 0)
        ::close(timer_fd_);
    if (event_fd_ >= 0)
        ::close(event_fd_);
    if (epoll_fd_ >= 0)
        ::close(epoll_fd_);
}

inline scheduler_op*
epoll_scheduler_core::try_acquire_post_node() const
{
    std::lock_guard lock(mutex_);
    auto* node = post_free_list_;
    if (node)
        post_free_list_ = node->recycle_next_;
    return node;
}

inline void
epoll_scheduler_core::release_post_node(scheduler_op* node) const
{
    std::lock_guard lock(mutex_);
    node->recycle_next_ = post_free_list_;
    post_free_list_ = node;
}

inline void
epoll_scheduler_core::shutdown()
{
    {
        std::unique_lock lock(mutex_);

        while (auto* h = completed_ops_.pop())
        {
            if (h == &task_op_)
                continue;
            lock.unlock();
            h->destroy();
            lock.lock();
        }

        // Drain post handler free list
        while (post_free_list_)
        {
            auto* next = post_free_list_->recycle_next_;
            delete post_free_list_;
            post_free_list_ = next;
        }

        signal_all(lock);
    }

    if (event_fd_ >= 0)
        interrupt_reactor();
}

inline void
epoll_scheduler_core::reset_inline_budget() const noexcept
{
    if (auto* ctx = epoll::find_context(this))
    {
        // Cap when no other thread absorbed queued work. A moderate
        // cap amortizes scheduling for small buffers while avoiding
        // bursty I/O that fills socket buffers and stalls large transfers.
        if (ctx->unassisted)
        {
            ctx->inline_budget_max = budget_unassisted_;
            ctx->inline_budget     = budget_unassisted_;
            return;
        }
        // Ramp up when previous cycle fully consumed budget.
        // Reset on partial consumption (EAGAIN hit or peer got scheduled).
        if (ctx->inline_budget == 0)
            ctx->inline_budget_max = (std::min)(
                ctx->inline_budget_max * 2,
                static_cast<int>(budget_max_));
        else if (ctx->inline_budget < ctx->inline_budget_max)
            ctx->inline_budget_max = budget_initial_;
        ctx->inline_budget = ctx->inline_budget_max;
    }
}

inline bool
epoll_scheduler_core::try_consume_inline_budget() const noexcept
{
    if (auto* ctx = epoll::find_context(this))
    {
        if (ctx->inline_budget > 0)
        {
            --ctx->inline_budget;
            return true;
        }
    }
    return false;
}

inline void
descriptor_state::operator()()
{
    is_enqueued_.store(false, std::memory_order_relaxed);

    // Take ownership of impl ref set by close_socket() to prevent
    // the owning impl from being freed while we're executing
    auto prevent_impl_destruction = std::move(impl_ref_);

    std::uint32_t ev = ready_events_.exchange(0, std::memory_order_acquire);
    if (ev == 0)
    {
        scheduler_->compensating_work_started();
        return;
    }

    op_queue local_ops;

    int err = 0;
    if (ev & EPOLLERR)
    {
        socklen_t len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
            err = errno;
        if (err == 0)
            err = EIO;
    }

    {
        std::lock_guard lock(mutex);
        if (ev & EPOLLIN)
        {
            if (read_op)
            {
                auto* rd = read_op;
                if (err)
                    rd->complete(err, 0);
                else
                    rd->perform_io();

                if (rd->errn == EAGAIN || rd->errn == EWOULDBLOCK)
                {
                    rd->errn = 0;
                }
                else
                {
                    read_op = nullptr;
                    local_ops.push(rd);
                }
            }
            else
            {
                read_ready = true;
            }
        }
        if (ev & EPOLLOUT)
        {
            bool had_write_op = (connect_op || write_op);
            if (connect_op)
            {
                auto* cn = connect_op;
                if (err)
                    cn->complete(err, 0);
                else
                    cn->perform_io();
                connect_op = nullptr;
                local_ops.push(cn);
            }
            if (write_op)
            {
                auto* wr = write_op;
                if (err)
                    wr->complete(err, 0);
                else
                    wr->perform_io();

                if (wr->errn == EAGAIN || wr->errn == EWOULDBLOCK)
                {
                    wr->errn = 0;
                }
                else
                {
                    write_op = nullptr;
                    local_ops.push(wr);
                }
            }
            if (!had_write_op)
                write_ready = true;
        }
        if (err)
        {
            if (read_op)
            {
                read_op->complete(err, 0);
                local_ops.push(std::exchange(read_op, nullptr));
            }
            if (write_op)
            {
                write_op->complete(err, 0);
                local_ops.push(std::exchange(write_op, nullptr));
            }
            if (connect_op)
            {
                connect_op->complete(err, 0);
                local_ops.push(std::exchange(connect_op, nullptr));
            }
        }
    }

    // Execute first handler inline — the scheduler's work_cleanup
    // accounts for this as the "consumed" work item
    scheduler_op* first = local_ops.pop();
    if (first)
    {
        scheduler_->post_deferred_completions(local_ops);
        (*first)();
    }
    else
    {
        scheduler_->compensating_work_started();
    }
}

inline void
epoll_scheduler_core::post(scheduler_op* h) const
{
    // Fast path: same thread posts to private queue
    // Only count locally; work_cleanup batches to global counter
    if (auto* ctx = epoll::find_context(this))
    {
        ++ctx->private_outstanding_work;
        ctx->private_queue.push(h);
        return;
    }

    // Slow path: cross-thread post requires mutex
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);

    std::unique_lock lock(mutex_);
    completed_ops_.push(h);
    wake_one_thread_and_unlock(lock);
}

inline bool
epoll_scheduler_core::running_in_this_thread() const noexcept
{
    for (auto* c = epoll::context_stack.get(); c != nullptr; c = c->next)
        if (c->key == this)
            return true;
    return false;
}

inline void
epoll_scheduler_core::stop()
{
    std::unique_lock lock(mutex_);
    stopped_ = true;
    signal_all(lock);

    if (task_running_.load(std::memory_order_acquire))
        interrupt_reactor();
}

inline bool
epoll_scheduler_core::stopped() const noexcept
{
    return stopped_;
}

inline void
epoll_scheduler_core::restart()
{
    stopped_ = false;
}

inline void
epoll_scheduler_core::register_descriptor(int fd, descriptor_state* desc) const
{
    epoll_event ev{};
    ev.events =
        EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLET | EPOLLRDHUP;
    ev.data.ptr        = desc;
    desc->scheduler_   = this;
    desc->fd           = fd;

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0)
    {
        detail::throw_system_error(make_err(errno), "epoll_ctl");
    }
}

inline void
epoll_scheduler_core::deregister_descriptor(int fd) const
{
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
}

inline void
epoll_scheduler_core::work_started() noexcept
{
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);
}

inline void
epoll_scheduler_core::work_finished() noexcept
{
    if (outstanding_work_.fetch_sub(1, std::memory_order_release) == 1)
    {
        stop();
    }
}

inline void
epoll_scheduler_core::compensating_work_started() const noexcept
{
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);
}

inline void
epoll_scheduler_core::drain_thread_queue(op_queue& queue, long count) const
{
    if (count > 0)
        outstanding_work_.fetch_add(count, std::memory_order_relaxed);

    std::unique_lock lock(mutex_);
    completed_ops_.splice(queue);
    wake_one_thread_and_unlock(lock);
}

inline void
epoll_scheduler_core::post_deferred_completions(op_queue& ops) const
{
    if (ops.empty())
        return;

    // Fast path: same thread posts to private queue
    if (auto* ctx = epoll::find_context(this))
    {
        ctx->private_queue.splice(ops);
        return;
    }

    // Slow path: cross-thread post requires mutex
    std::unique_lock lock(mutex_);
    completed_ops_.splice(ops);
    wake_one_thread_and_unlock(lock);
}

inline void
epoll_scheduler_core::interrupt_reactor() const
{
    std::uint64_t val = 1;
    bool expected     = false;
    if (eventfd_armed_.compare_exchange_strong(
            expected, true, std::memory_order_release,
            std::memory_order_relaxed))
    {
        [[maybe_unused]] auto r = ::write(event_fd_, &val, sizeof(val));
    }
}

inline void
epoll_scheduler_core::signal_all(std::unique_lock<std::mutex>&) const
{
    state_ |= 1;
    cond_.notify_all();
}

inline bool
epoll_scheduler_core::maybe_unlock_and_signal_one(
    std::unique_lock<std::mutex>& lock) const
{
    if (state_ > 1) // waiters exist
    {
        state_ |= 1;
        lock.unlock();
        cond_.notify_one();
        return true;
    }
    return false;
}

inline bool
epoll_scheduler_core::unlock_and_signal_one(std::unique_lock<std::mutex>& lock) const
{
    bool has_waiters = (state_ > 1);
    state_ |= 1;
    lock.unlock();
    if (has_waiters)
        cond_.notify_one();
    return has_waiters;
}

inline void
epoll_scheduler_core::clear_signal() const
{
    state_ &= ~std::size_t(1);
}

inline void
epoll_scheduler_core::wait_for_signal(std::unique_lock<std::mutex>& lock) const
{
    // Fast path: already signaled
    if (state_ & 1)
        return;

    state_ += 2; // increment waiter count
    cond_.wait(lock, [this] { return (state_ & 1) != 0; });
    state_ -= 2; // decrement waiter count
}

inline void
epoll_scheduler_core::wait_for_signal_for(
    std::unique_lock<std::mutex>& lock, long timeout_us) const
{
    // Fast path: already signaled
    if (state_ & 1)
        return;

    state_ += 2;
    cond_.wait_for(
        lock,
        std::chrono::microseconds(timeout_us),
        [this] { return (state_ & 1) != 0; });
    state_ -= 2;
}

inline void
epoll_scheduler_core::wake_one_thread_and_unlock(
    std::unique_lock<std::mutex>& lock) const
{
    if (!maybe_unlock_and_signal_one(lock))
    {
        bool need_interrupt =
            task_running_.load(std::memory_order_acquire);
        lock.unlock();
        if (need_interrupt)
            interrupt_reactor();
    }
}

inline void
epoll_scheduler_core::update_timerfd() const
{
    auto nearest = timer_svc_->nearest_expiry();

    itimerspec ts{};
    int flags = 0;

    if (nearest == timer_service::time_point::max())
    {
        // No timers - disarm by setting to 0 (relative)
    }
    else
    {
        auto now = std::chrono::steady_clock::now();
        if (nearest <= now)
        {
            // Use 1ns instead of 0 - zero disarms the timerfd
            ts.it_value.tv_nsec = 1;
        }
        else
        {
            auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            nearest - now)
                            .count();
            ts.it_value.tv_sec  = nsec / 1000000000;
            ts.it_value.tv_nsec = nsec % 1000000000;
            // Ensure non-zero to avoid disarming if duration rounds to 0
            if (ts.it_value.tv_sec == 0 && ts.it_value.tv_nsec == 0)
                ts.it_value.tv_nsec = 1;
        }
    }

    if (::timerfd_settime(timer_fd_, flags, &ts, nullptr) < 0)
        detail::throw_system_error(make_err(errno), "timerfd_settime");
}

inline epoll_scheduler_core::work_cleanup::~work_cleanup()
{
    if (ctx)
    {
        long produced = ctx->private_outstanding_work;
        if (produced > 1)
            scheduler->outstanding_work_.fetch_add(
                produced - 1, std::memory_order_relaxed);
        else if (produced < 1)
            scheduler->work_finished();
        ctx->private_outstanding_work = 0;

        if (!ctx->private_queue.empty())
        {
            lock->lock();
            scheduler->completed_ops_.splice(ctx->private_queue);
        }
    }
    else
    {
        scheduler->work_finished();
    }
}

inline epoll_scheduler_core::task_cleanup::~task_cleanup()
{
    if (!ctx)
        return;

    if (ctx->private_outstanding_work > 0)
    {
        scheduler->outstanding_work_.fetch_add(
            ctx->private_outstanding_work, std::memory_order_relaxed);
        ctx->private_outstanding_work = 0;
    }

    if (!ctx->private_queue.empty())
    {
        if (!lock->owns_lock())
            lock->lock();
        scheduler->completed_ops_.splice(ctx->private_queue);
    }
}

//--------------------------------------------------------------------------
// epoll_scheduler<Config> implementation
//--------------------------------------------------------------------------

template<epoll_config Config>
inline epoll_scheduler<Config>::epoll_scheduler(
    capy::execution_context& ctx, int concurrency_hint)
    : epoll_scheduler_core(
          ctx,
          concurrency_hint,
          Config.inline_budget_initial,
          Config.inline_budget_max,
          Config.unassisted_budget)
{
}

template<epoll_config Config>
inline void
epoll_scheduler<Config>::post(std::coroutine_handle<> h) const
{
    struct post_handler final : scheduler_op
    {
        std::coroutine_handle<> h_;

        explicit post_handler(std::coroutine_handle<> h) : h_(h) {}

        ~post_handler() override = default;

        void operator()() override
        {
            auto coro = h_;
            if constexpr (Config.recycle_post_nodes)
            {
                auto* ctx = epoll::context_stack.get();
                auto* core = static_cast<
                    epoll_scheduler_core const*>(ctx->key);
                if (core->recycle_post_nodes_rt_)
                {
                    if (!ctx->post_cache)
                        ctx->post_cache = this;
                    else
                        core->release_post_node(this);
                }
                else
                {
                    delete this;
                }
                coro.resume();
            }
            else
            {
                delete this;
                coro.resume();
            }
        }

        void destroy() override
        {
            auto coro = h_;
            delete this;
            coro.destroy();
        }
    };

    auto* ctx = epoll::find_context(this);

    // Acquire a recycled node or allocate a new one
    post_handler* ph = nullptr;
    if constexpr (Config.recycle_post_nodes)
    {
        if (recycle_post_nodes_rt_)
        {
            if (ctx && ctx->post_cache)
            {
                ph = static_cast<post_handler*>(ctx->post_cache);
                ctx->post_cache = nullptr;
                ph->h_ = h;
            }
            else if (auto* cached = try_acquire_post_node())
            {
                ph = static_cast<post_handler*>(cached);
                ph->h_ = h;
            }
        }
    }
    if (!ph)
        ph = new post_handler(h);

    // Fast path: same thread posts to private queue
    // Only count locally; work_cleanup batches to global counter
    if (ctx)
    {
        ++ctx->private_outstanding_work;
        ctx->private_queue.push(ph);
        return;
    }

    // Slow path: cross-thread post requires mutex
    outstanding_work_.fetch_add(1, std::memory_order_relaxed);

    std::unique_lock lock(mutex_);
    completed_ops_.push(ph);
    wake_one_thread_and_unlock(lock);
}

template<epoll_config Config>
inline std::size_t
epoll_scheduler<Config>::run()
{
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    epoll::thread_context_guard ctx(this, Config.inline_budget_initial);
    std::unique_lock lock(mutex_);

    std::size_t n = 0;
    for (;;)
    {
        if (!do_one(lock, -1, &ctx.frame_))
            break;
        if (n != (std::numeric_limits<std::size_t>::max)())
            ++n;
        if (!lock.owns_lock())
            lock.lock();
    }
    return n;
}

template<epoll_config Config>
inline std::size_t
epoll_scheduler<Config>::run_one()
{
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    epoll::thread_context_guard ctx(this, Config.inline_budget_initial);
    std::unique_lock lock(mutex_);
    return do_one(lock, -1, &ctx.frame_);
}

template<epoll_config Config>
inline std::size_t
epoll_scheduler<Config>::wait_one(long usec)
{
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    epoll::thread_context_guard ctx(this, Config.inline_budget_initial);
    std::unique_lock lock(mutex_);
    return do_one(lock, usec, &ctx.frame_);
}

template<epoll_config Config>
inline std::size_t
epoll_scheduler<Config>::poll()
{
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    epoll::thread_context_guard ctx(this, Config.inline_budget_initial);
    std::unique_lock lock(mutex_);

    std::size_t n = 0;
    for (;;)
    {
        if (!do_one(lock, 0, &ctx.frame_))
            break;
        if (n != (std::numeric_limits<std::size_t>::max)())
            ++n;
        if (!lock.owns_lock())
            lock.lock();
    }
    return n;
}

template<epoll_config Config>
inline std::size_t
epoll_scheduler<Config>::poll_one()
{
    if (outstanding_work_.load(std::memory_order_acquire) == 0)
    {
        stop();
        return 0;
    }

    epoll::thread_context_guard ctx(this, Config.inline_budget_initial);
    std::unique_lock lock(mutex_);
    return do_one(lock, 0, &ctx.frame_);
}

template<epoll_config Config>
inline void
epoll_scheduler<Config>::run_task(
    std::unique_lock<std::mutex>& lock, epoll::scheduler_context* ctx)
{
    int timeout_ms = task_interrupted_ ? 0 : -1;

    if (lock.owns_lock())
        lock.unlock();

    task_cleanup on_exit{this, &lock, ctx};

    // Flush deferred timerfd programming before blocking
    if (timerfd_stale_.exchange(false, std::memory_order_acquire))
        update_timerfd();

    // Event loop runs without mutex held
    epoll_event events[Config.max_events_per_poll];
    int nfds = ::epoll_wait(epoll_fd_, events, Config.max_events_per_poll, timeout_ms);

    if (nfds < 0 && errno != EINTR)
        detail::throw_system_error(make_err(errno), "epoll_wait");

    bool check_timers = false;
    op_queue local_ops;

    // Process events without holding the mutex
    for (int i = 0; i < nfds; ++i)
    {
        if (events[i].data.ptr == nullptr)
        {
            std::uint64_t val;
            // Mutex released above; analyzer can't track unlock via ref
            // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection)
            [[maybe_unused]] auto r = ::read(event_fd_, &val, sizeof(val));
            eventfd_armed_.store(false, std::memory_order_relaxed);
            continue;
        }

        if (events[i].data.ptr == &timer_fd_)
        {
            std::uint64_t expirations;
            // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection)
            [[maybe_unused]] auto r =
                ::read(timer_fd_, &expirations, sizeof(expirations));
            check_timers = true;
            continue;
        }

        // Deferred I/O: just set ready events and enqueue descriptor
        // No per-descriptor mutex locking in reactor hot path!
        auto* desc = static_cast<descriptor_state*>(events[i].data.ptr);
        desc->add_ready_events(events[i].events);

        // Only enqueue if not already enqueued
        bool expected = false;
        if (desc->is_enqueued_.compare_exchange_strong(
                expected, true, std::memory_order_release,
                std::memory_order_relaxed))
        {
            local_ops.push(desc);
        }
    }

    // Process timers only when timerfd fires
    if (check_timers)
    {
        timer_svc_->process_expired();
        update_timerfd();
    }

    lock.lock();

    if (!local_ops.empty())
        completed_ops_.splice(local_ops);
}

template<epoll_config Config>
inline std::size_t
epoll_scheduler<Config>::do_one(
    std::unique_lock<std::mutex>& lock,
    long timeout_us,
    epoll::scheduler_context* ctx)
{
    for (;;)
    {
        if (stopped_)
            return 0;

        scheduler_op* op = completed_ops_.pop();

        // Handle reactor sentinel - time to poll for I/O
        if (op == &task_op_)
        {
            bool more_handlers = !completed_ops_.empty();

            // Nothing to run the reactor for: no pending work to wait on,
            // or caller requested a non-blocking poll
            if (!more_handlers &&
                (outstanding_work_.load(std::memory_order_acquire) == 0 ||
                 timeout_us == 0))
            {
                completed_ops_.push(&task_op_);
                return 0;
            }

            task_interrupted_ = more_handlers || timeout_us == 0;
            task_running_.store(true, std::memory_order_release);

            if (more_handlers)
                unlock_and_signal_one(lock);

            run_task(lock, ctx);

            task_running_.store(false, std::memory_order_relaxed);
            completed_ops_.push(&task_op_);
            continue;
        }

        // Handle operation
        if (op != nullptr)
        {
            bool more = !completed_ops_.empty();

            if (more)
                ctx->unassisted = !unlock_and_signal_one(lock);
            else
            {
                ctx->unassisted = false;
                lock.unlock();
            }

            work_cleanup on_exit{this, &lock, ctx};

            (*op)();
            return 1;
        }

        // No pending work to wait on, or caller requested non-blocking poll
        if (outstanding_work_.load(std::memory_order_acquire) == 0 ||
            timeout_us == 0)
            return 0;

        clear_signal();
        if (timeout_us < 0)
            wait_for_signal(lock);
        else
            wait_for_signal_for(lock, timeout_us);
    }
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_EPOLL

#endif // BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_SCHEDULER_HPP
