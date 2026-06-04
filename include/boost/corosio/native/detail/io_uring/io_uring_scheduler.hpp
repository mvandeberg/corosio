//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_SCHEDULER_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_SCHEDULER_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IO_URING

// Include before any project headers open a namespace — prevents the
// boost::corosio::io_uring tag variable from shadowing struct ::io_uring.
#include <liburing.h>

#include <boost/corosio/detail/conditionally_enabled_event.hpp>
#include <boost/corosio/detail/conditionally_enabled_mutex.hpp>
#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/scheduler.hpp>
#include <boost/corosio/detail/scheduler_op.hpp>
#include <boost/corosio/detail/timer_service.hpp>
#include <boost/corosio/native/detail/reactor/reactor_scheduler.hpp>
#include <boost/corosio/native/detail/io_uring/io_uring_op.hpp>
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/native/detail/posix/posix_resolver_service.hpp>
#include <boost/corosio/native/detail/posix/posix_signal_service.hpp>
#include <boost/capy/ex/execution_context.hpp>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <errno.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace boost::corosio::detail {

/** io_uring scheduler — proactor model on Linux 6.x+.

    Owns one io_uring per io_context. Lazy batched submit;
    cross-thread post wakes a registered eventfd via multishot
    POLL_ADD.

    @par Thread Safety
    All public member functions are thread-safe.
*/
// io_uring is a proactor, but its run loop is the same leader/follower
// event loop the reactors use: one thread polls (here: submit SQEs +
// wait for CQEs + drain them into completed_ops_) while others dispatch
// ready ops. So it derives from the backend-neutral reactor_scheduler and
// supplies only run_task() (the kernel poll) + interrupt_reactor() (the
// wake) + ensure_initialized() (lazy ring creation), keeping its
// io_uring-specific ring/submit/cancel machinery below. Mirrors how
// Boost.Asio plugs io_uring_service into its generic scheduler as a task.
class BOOST_COROSIO_DECL io_uring_scheduler final
    : public reactor_scheduler
{
public:
    io_uring_scheduler(capy::execution_context& ctx, int concurrency_hint = -1);
    ~io_uring_scheduler() override;
    io_uring_scheduler(io_uring_scheduler const&)            = delete;
    io_uring_scheduler& operator=(io_uring_scheduler const&) = delete;

    void shutdown() override;

    /** Return the underlying liburing ring.

        Triggers lazy ring initialisation on first call. Used by
        socket op submission helpers (e.g. `io_uring_submit_op`) and
        any other code path that needs a live ring pointer.
    */
    struct ::io_uring* ring() noexcept
    {
        lazy_init_ring();
        return &ring_;
    }

    /// Return the dispatch mutex (the base leader/follower queue lock,
    /// which protects completed_ops_ / cond_). Exposed for the submit
    /// path's synchronous-completion enqueue (push_completed_locked).
    mutex_type& dispatch_mutex() const noexcept { return mutex_; }

    /// Return the ring mutex (serialises userspace SQ/CQ access).
    mutex_type& ring_mutex() const noexcept { return ring_mutex_; }

    // reset_inline_budget() / try_consume_inline_budget() /
    // running_in_this_thread() are provided by reactor_scheduler.

    /// Exchange the submit-batch posted flag. Returns the prior value.
    /// Caller MUST hold ring_mutex_ — the flag is plain bool, not atomic,
    /// and the mutex provides the read-modify-write atomicity.
    bool submit_op_posted_exchange(bool desired) const noexcept
    {
        bool prev = submit_op_posted_;
        submit_op_posted_ = desired;
        return prev;
    }

    /// Return a reference to the mutable embedded submit_sqes_op.
    scheduler_op& submit_op_ref() const noexcept
    {
        return submit_op_;
    }

    /// Increment the io_uring in-flight counter. Callers prep an SQE
    /// whose CQE will require IORING_ENTER_GETEVENTS to surface under
    /// DEFER_TASKRUN. Excluded: the wakeup-eventfd multishot SQE, whose
    /// progress doesn't depend on userspace getevents.
    void inflight_inc() const noexcept
    {
        io_uring_inflight_.fetch_add(1, std::memory_order_release);
    }

    /// Initialize the io_uring ring on first access. Idempotent.
    void lazy_init_ring() const;

    /// Wake the leader if it's blocked in the kernel CQE wait
    /// (reactor_scheduler hook). Writes the wakeup eventfd, whose
    /// multishot poll fires a CQE that returns the leader from
    /// io_uring_wait_cqe_timeout.
    void interrupt_reactor() const noexcept override;

    /** Submit `IORING_OP_ASYNC_CANCEL` targeting an in-flight op by its
        user_data pointer.

        The kernel delivers `-ECANCELED` on the target's CQE if it was
        still in flight; the op's completion handler then reports
        `operation_aborted`.  Best-effort: if the SQ is full after one
        flush attempt the function returns without cancelling (the op
        will complete normally on its own).

        @param target The in-flight op to cancel.
    */
    void submit_cancel_by_user_data(io_uring_op* target) noexcept;

    /** Submit `IORING_OP_ASYNC_CANCEL` with `IORING_ASYNC_CANCEL_FD`
        to cancel every in-flight op on the given fd in one SQE.

        Best-effort: if the SQ is full after one flush attempt the
        function returns without cancelling.

        @param fd The file descriptor whose in-flight ops should be
            cancelled.
    */
    void submit_cancel_by_fd(int fd) noexcept;

    /** Submit `IORING_OP_ASYNC_CANCEL` for `fd` and immediately flush
        the submission ring to the kernel.

        Must be called while `fd` is still open so the kernel can
        resolve the file from the fd number before it is closed and
        potentially recycled.

        Best-effort: if the SQ is full the function still flushes any
        earlier pending SQEs to the kernel.

        @param fd The file descriptor whose in-flight ops should be
            cancelled.
    */
    void cancel_and_flush(int fd) noexcept;

    /** Drain pending CQEs for a specific op's `user_data`.

        Submits an ASYNC_CANCEL by user_data to short-circuit any
        in-flight op holding `target`, then iterates the CQ ring and
        consumes every CQE matching `target` so its memory can be
        freed safely. Used by member-owned ops (e.g.
        `uring_multi_accept_op`) whose destructor cannot tolerate
        outstanding CQEs.

        @par Thread Safety
        Safe to call from any thread. Internally takes `ring_mutex_`
        to serialise against the run-loop leader; calls
        `interrupt_reactor()` first so the leader returns from its
        kernel wait promptly.

        @param target The op pointer used as user_data on the SQE.
    */
    void drain_cqes_for(io_uring_op* target) noexcept;

    /** Queue an already-counted op while the caller holds dispatch_mutex_.

        Does NOT increment `outstanding_work_`. Use for synchronous
        completion paths (e.g. SQE backpressure) where the caller called
        `work_started()` and already holds the dispatch lock.

        @pre `dispatch_mutex_` must be locked by the calling thread.
    */
    void push_completed_locked(scheduler_op* op) const noexcept
    {
        completed_ops_.push(op);
    }

    /// Single-threaded mode toggle. The base disables the dispatch mutex
    /// and condvar; we additionally disable the ring mutex.
    void configure_single_threaded(bool v) noexcept override
    {
        reactor_scheduler::configure_single_threaded(v);
        ring_mutex_.set_enabled(!v);
    }

    /** Configure SQPOLL parameters.

        Must be called before the first run/poll/post — the values
        are cached and read by `lazy_init_ring_unlocked` when the
        ring is first constructed. No-op if `enable` is false (the
        default).

        @note  When combined with single-threaded mode,
        IORING_SETUP_DEFER_TASKRUN is suppressed — the kernel
        rejects that combination. SINGLE_ISSUER still applies.

        @param enable    Set IORING_SETUP_SQPOLL on ring init.
        @param idle_ms   sq_thread_idle in milliseconds; 0 = kernel
                         default (1ms).
        @param cpu       Pin the polling thread to this CPU; -1 to
                         not pin.
    */
    void configure_sqpoll(
        bool enable, unsigned idle_ms, int cpu) noexcept
    {
        enable_sqpoll_     = enable;
        sq_thread_idle_ms_ = idle_ms;
        sq_thread_cpu_     = cpu;
    }

protected:
    /// reactor_scheduler hooks.
    void run_task(lock_type& lock, context_type* ctx,
        long timeout_us) override;
    void ensure_initialized() override { lazy_init_ring(); }

private:
    // The leader/follower queue (completed_ops_), its dispatch mutex_ and
    // cond_, outstanding_work_, stopped_, the inline-budget context stack,
    // single_threaded_, and timer_svc_ all live in reactor_scheduler.
    //
    // ring_ + wakeup_eventfd_ are mutable so lazy_init_ring() (called from
    // const contexts) can populate them on first use. ring_mutex_ protects
    // every userspace touch of ring_ (SQ tail, CQ head): get_sqe / submit /
    // wait_cqe_timeout / for_each_cqe / cq_advance. It is io_uring-specific
    // (the reactors and IOCP have no equivalent) and stays here, not in the
    // base. Lock order when both are held: ring_mutex_ -> mutex_.
    mutable struct ::io_uring          ring_{};
    mutable int                       wakeup_eventfd_ = -1;
    mutable mutex_type                ring_mutex_{true};
    // Count of io_uring SQEs in flight whose completion requires user-
    // space to enter the kernel via IORING_ENTER_GETEVENTS for task
    // work to progress under IORING_SETUP_DEFER_TASKRUN. Excludes the
    // wakeup-eventfd multishot poll (registered in lazy_init_ring), and
    // is updated by io_uring_submit_op and by process_completions on
    // each non-F_MORE, non-eventfd CQE. Used by run_task to skip the
    // ring pump when there is no io_uring work pending. On its own cache
    // line to avoid false sharing with the base's outstanding_work_.
    alignas(64) mutable std::atomic<std::int64_t> io_uring_inflight_{0};
    bool                              enable_sqpoll_     = false;
    unsigned                          sq_thread_idle_ms_ = 0;
    int                               sq_thread_cpu_     = -1;

    int                               cancel_sentinel_ = 0;
    mutable std::atomic<bool>         wakeup_armed_{false};

    /// Flushes the SQ ring and drains CQEs in one mutex-held pass.
    /// One instance covers a whole batch; subsequent SQEs in the same
    /// batch skip the post, amortising syscall cost across the batch.
    /// Mirrors Asio's `submit_sqes_op` (`io_uring_service.ipp:730-742`).
    struct submit_sqes_op final : scheduler_op
    {
        io_uring_scheduler* sched_ = nullptr;

        submit_sqes_op() noexcept : scheduler_op(&do_handler) {}

        static void do_handler(
            void* owner, scheduler_op* base,
            std::uint32_t /*bytes*/, std::uint32_t /*error*/) noexcept;
    };

    /// True between the first submitter of a batch posting `submit_op_`
    /// and the dispatched op clearing the flag inside its handler. Read
    /// and written only while holding `ring_mutex_`.
    mutable bool                      submit_op_posted_ = false;

    /// Single embedded `submit_sqes_op` instance, owned by the scheduler.
    mutable submit_sqes_op            submit_op_;

    // drain_cqes_for tuning. The bound exists to avoid stalling a
    // destructor if the kernel never returns a cancel completion (best-
    // effort drain); 8 rounds * 1ms == 8ms worst case.
    static constexpr int              drain_cqes_max_rounds = 8;
    static constexpr unsigned long    drain_cqes_kick_ns    = 1'000'000;

    // ring_inited_ goes true once on first run/poll/submit. The init is
    // deferred from the constructor so configure_single_threaded(true)
    // can take effect before io_uring_queue_init_params chooses flags.
    mutable std::once_flag            ring_init_once_;
    mutable bool                      ring_inited_ = false;

    void        process_completions();
    void        drain_wakeup_eventfd() const noexcept;
    void        lazy_init_ring_unlocked() const;
};

inline
io_uring_scheduler::io_uring_scheduler(
    capy::execution_context& ctx, int /*concurrency_hint*/)
{
    // sched_ cannot be set in the member initialiser — `this` is not
    // available there.
    submit_op_.sched_ = this;

    // Seed the leader/follower queue with the task sentinel so do_one
    // has something to pop that triggers run_task (the io_uring poll).
    // Matches epoll_scheduler / kqueue_scheduler construction.
    completed_ops_.push(&task_op_);

    // Wire timer service. on_earliest_changed wakes the run loop so it
    // recomputes its wait timeout.
    timer_svc_ = &get_timer_service(ctx, *this);
    timer_svc_->set_on_earliest_changed(
        timer_service::callback(this, [](void* p) {
            static_cast<io_uring_scheduler*>(p)->interrupt_reactor();
        }));

    get_resolver_service(ctx, *this);
    get_signal_service(ctx, *this);

    // Ring init is deferred to lazy_init_ring() so configure_single_-
    // threaded(true), which the io_context applies after construction,
    // can take effect before io_uring_queue_init_params chooses flags.
}

inline
io_uring_scheduler::~io_uring_scheduler()
{
    if (ring_inited_)
    {
        if (wakeup_eventfd_ >= 0)
            ::close(wakeup_eventfd_);
        ::io_uring_queue_exit(&ring_);
    }
}

inline void
io_uring_scheduler::lazy_init_ring() const
{
    std::call_once(ring_init_once_, [this] {
        lazy_init_ring_unlocked();
    });
}

inline void
io_uring_scheduler::lazy_init_ring_unlocked() const
{
    io_uring_params params{};
    if (single_threaded_)
    {
        // SINGLE_ISSUER promises the kernel one submitter thread,
        // letting it skip internal SQ locking. DEFER_TASKRUN tells
        // it to batch task_work delivery at io_uring_enter(GETEVENTS)
        // boundaries instead of interrupting the run thread via
        // TWA_SIGNAL — eliminates cache pollution from mid-flight
        // task_work and gives a meaningful single-threaded
        // throughput uplift.
        //
        // Plan 3 disabled DEFER_TASKRUN defensively over a misread
        // of the GETEVENTS contract. Plan 4a re-enabled it: liburing's
        // io_uring_submit_and_wait_timeout always sets
        // IORING_ENTER_GETEVENTS when wait_nr > 0, regardless of
        // ts. Our run loop's only kernel-wait call passes wait_nr=1.
        // Submit-only paths (cancel_and_flush, etc.) leave their
        // CQEs queued until the leader's next GETEVENTS-bearing
        // wait — benign.
        //
        // Multi-thread mode never sets these flags: SINGLE_ISSUER
        // would be unsafe with multiple submitter threads.
        //
        // DEFER_TASKRUN is suppressed when SQPOLL is also enabled
        // — the kernel rejects that combination with -EINVAL. The
        // SQPOLL polling thread already delivers completions
        // without TWA_SIGNAL interruption, so DEFER_TASKRUN's
        // benefit is moot in that mode.
        params.flags = IORING_SETUP_SINGLE_ISSUER;
        if (!enable_sqpoll_)
            params.flags |= IORING_SETUP_DEFER_TASKRUN;
    }

    if (enable_sqpoll_)
    {
        // SQPOLL forks a kernel thread that busy-polls the SQ ring;
        // submission becomes a userspace-only memory store. Combines
        // with SINGLE_ISSUER (the kernel accepts that pair) but NOT
        // with DEFER_TASKRUN (kernel returns -EINVAL); the
        // single_threaded_ branch above suppresses DEFER_TASKRUN
        // when SQPOLL is also set. Idle timeout 0 means kernel
        // default (1ms); we only forward when explicitly set so
        // the kernel default is preserved.
        params.flags |= IORING_SETUP_SQPOLL;
        if (sq_thread_idle_ms_ != 0)
            params.sq_thread_idle = sq_thread_idle_ms_;
        if (sq_thread_cpu_ >= 0)
        {
            params.flags |= IORING_SETUP_SQ_AFF;
            params.sq_thread_cpu = static_cast<__u32>(sq_thread_cpu_);
        }
    }

    int rc = ::io_uring_queue_init_params(256, &ring_, &params);
    if (rc < 0)
        detail::throw_system_error(
            make_err(-rc), "io_uring_queue_init_params");

    wakeup_eventfd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_eventfd_ < 0)
    {
        int errn = errno;
        ::io_uring_queue_exit(&ring_);
        detail::throw_system_error(make_err(errn), "eventfd");
    }

    // Register a one-shot poll on the wake eventfd. user_data nullptr
    // is the sentinel recognized by process_completions, which calls
    // drain_wakeup_eventfd() to consume the eventfd byte AND re-arm
    // the poll. Plan 5a switched away from IORING_POLL_MULTISHOT
    // because multishot ops can silently terminate (e.g. under CQ
    // pressure), and we don't observe the termination — leaving the
    // wake mechanism dead and the leader stuck in kernel wait. One-
    // shot rearm-on-fire is fail-fast: every wake event is paired
    // with an explicit rearm, so a missed rearm would manifest
    // immediately as the next wake being lost (test-visible).
    ::io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_);
    if (!sqe)
    {
        ::close(wakeup_eventfd_);
        ::io_uring_queue_exit(&ring_);
        detail::throw_system_error(
            make_err(ENOSPC), "io_uring_get_sqe (wakeup)");
    }
    // Multishot poll: fires a CQE on each eventfd POLLIN without
    // consuming the SQE. Avoids the re-arm hazard of one-shot poll
    // (where drain_wakeup_eventfd's get_sqe could return null on a
    // full SQ, leaving no SQE to detect future wakes).
    ::io_uring_prep_poll_multishot(sqe, wakeup_eventfd_, POLLIN);
    ::io_uring_sqe_set_data(sqe, nullptr);
    int submit_rc = ::io_uring_submit(&ring_);
    if (submit_rc < 0)
    {
        ::close(wakeup_eventfd_);
        ::io_uring_queue_exit(&ring_);
        detail::throw_system_error(
            make_err(-submit_rc), "io_uring_submit (wakeup)");
    }

    ring_inited_ = true;
}

inline void
io_uring_scheduler::shutdown()
{
    stopped_.store(true, std::memory_order_release);

    // Drain posted ops, calling destroy() on each so embedded handles
    // (coroutine frames, error_code outputs) get torn down rather than
    // leaked. reactor_scheduler::shutdown_drain pops every op (skipping
    // the task_op_ sentinel), destroys it, then signals all waiters.
    //
    // Service shutdown order (driven by capy::execution_context): each
    // socket/acceptor service::shutdown() submits a cancel SQE for every
    // live impl. The resulting CQEs either land in completed_ops_
    // (drained here) or stay in the kernel ring; ~scheduler's
    // io_uring_queue_exit cleans the latter up at process teardown.
    // Self-referential impl_ptr cycles (e.g. the multishot acceptor's
    // multi_op_->impl_ptr) are broken inside each service before the
    // scheduler shutdown runs.
    shutdown_drain();
}

// stop / stopped / restart / work_started / work_finished are inherited
// from reactor_scheduler. The base stop() sets stopped_, signals all
// followers, and calls interrupt_reactor() (below) to force the leader
// out of its kernel wait — io_uring's interrupt_reactor writes the
// wakeup eventfd unconditionally, so the wake is never dropped.

inline void
io_uring_scheduler::interrupt_reactor() const noexcept
{
    // Skip if the ring hasn't been initialised yet — there's no leader
    // to wake and no eventfd to write.
    if (!ring_inited_)
        return;

    // Single-thread: the user's coroutines run on the leader thread,
    // so when interrupt_reactor is called from user code the leader
    // is not in kernel wait — there is nothing to wake.
    if (single_threaded_)
        return;

    // Multi-thread: write the eventfd unconditionally. CAS-coalescing
    // is unsafe here because the leader's Phase 2 in do_one waits
    // indefinitely for a CQE; a dropped wake leaves the leader
    // blocked forever when there is no other CQE-producing activity.
    // Multishot poll on wakeup_eventfd_ delivers a CQE for every
    // write, so multiple writes in flight produce multiple CQEs
    // (drained together by drain_wakeup_eventfd's single read of
    // the eventfd counter).
    std::uint64_t v = 1;
    [[maybe_unused]] auto r = ::write(wakeup_eventfd_, &v, sizeof(v));
    wakeup_armed_.store(true, std::memory_order_release);
}

inline void
io_uring_scheduler::drain_wakeup_eventfd() const noexcept
{
    std::uint64_t v;
    [[maybe_unused]] auto r = ::read(wakeup_eventfd_, &v, sizeof(v));

    // Multishot poll never needs re-arming. The poll-add was queued
    // once at lazy_init_ring with IORING_POLL_ADD_MULTI; each eventfd
    // POLLIN produces a CQE without consuming the SQE.
    //
    // Release pairs with the acquire side of interrupt_reactor's CAS:
    // a posting thread that observes wakeup_armed_ == false from this
    // store will see the eventfd already drained by the leader.
    wakeup_armed_.store(false, std::memory_order_release);
}

// post(coroutine_handle) / post(scheduler_op*), running_in_this_thread,
// reset_inline_budget / try_consume_inline_budget, the inline-budget
// frame stack + run guard, and run/run_one/wait_one/poll/poll_one are
// all inherited from reactor_scheduler. Only the backend poll below
// (run_task) and the wake (interrupt_reactor, above) are io_uring's.

inline void
io_uring_scheduler::run_task(
    lock_type& lock, context_type* ctx, long timeout_us)
{
    // reactor_scheduler hook: the io_uring "poll". The base do_one has
    // already elected this thread as the leader (task_running_ set) and,
    // when other handlers are queued, dropped the dispatch lock and
    // signalled a follower. Our job: submit pending SQEs, wait for at
    // least one CQE (or the deadline), and splice the completed ops into
    // completed_ops_ via process_completions. The base re-pushes the
    // task sentinel and dispatches what we produced.
    //
    // task_interrupted_ (set by the base when more handlers are pending)
    // or a zero timeout means a non-blocking peek.
    bool const nonblocking = task_interrupted_ || timeout_us == 0;

    if (lock.owns_lock())
        lock.unlock();

    // Drain this thread's private queue back into completed_ops_ on every
    // exit path (including the early return and the throw). Completions
    // posted during this call land in the private queue when post() runs
    // on a run thread — most importantly timer completions from
    // process_expired() below (sched_->post(&w->op_)). Without this they
    // stay stranded in the private queue: do_one's more_handlers check
    // sees private_queue non-empty forever, so it keeps calling run_task
    // non-blocking (busy spin) and the timer handler never dispatches.
    // Matches epoll_scheduler::run_task.
    task_cleanup on_exit{this, &lock, ctx};

    // Wait deadline. Non-blocking -> {0,0}. Otherwise blend the nearest
    // timer expiry; for an indefinite run() with no timer, cap at 1s as a
    // lost-wakeup safety net (a dropped eventfd-poll re-arm would
    // otherwise block the leader forever).
    __kernel_timespec  ts{};
    __kernel_timespec* ts_ptr = nullptr;
    if (nonblocking)
    {
        ts_ptr = &ts;   // {0, 0}
    }
    else
    {
        auto next_expiry = timer_svc_->nearest_expiry();
        if (next_expiry != timer_service::time_point::max())
        {
            auto now = std::chrono::steady_clock::now();
            auto delta_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    next_expiry - now)
                    .count();
            if (delta_ns < 0) delta_ns = 0;
            ts.tv_sec  = delta_ns / 1'000'000'000;
            ts.tv_nsec = delta_ns % 1'000'000'000;
            ts_ptr     = &ts;
        }
        else if (timeout_us > 0)
        {
            ts.tv_sec  = timeout_us / 1'000'000;
            ts.tv_nsec = (timeout_us % 1'000'000) * 1000;
            ts_ptr     = &ts;
        }
        else
        {
            ts.tv_sec  = 1;
            ts.tv_nsec = 0;
            ts_ptr     = &ts;
        }
    }

    // Phase 1 - flush pending SQEs. For a non-blocking peek, skip the
    // kernel entirely when there is no io_uring work (no in-flight ops
    // needing DEFER_TASKRUN GETEVENTS, no unsubmitted SQEs, no ready
    // CQEs): a kernel entry would have nothing to do. Checked under
    // ring_mutex_ so a cross-thread io_uring_submit_op cannot prep an SQE
    // we then race past. Preserves the no-I/O microbenchmark win.
    //
    // We only skip the *kernel* pass here, not the timer drain below: an
    // expired corosio timer is steady_clock state independent of the ring,
    // so it must still be processed even when there is no kernel work, or a
    // fired timer is stranded until the next pass that does touch the kernel
    // (a hang when the ring is otherwise idle — e.g. a stop-timer driving an
    // all-inline workload).
    bool kernel_pass = true;
    {
        lock_type ring_lock(ring_mutex_);
        bool const has_kernel_work =
            io_uring_inflight_.load(std::memory_order_acquire) != 0
            || ::io_uring_sq_ready(&ring_) != 0
            || ::io_uring_cq_ready(&ring_) != 0;
        if (nonblocking && !has_kernel_work)
            kernel_pass = false;
        else
            ::io_uring_submit(&ring_);
    }

    if (kernel_pass)
    {
        // Phase 2 - wait for >=1 CQE without holding either mutex. Cross-
        // thread submitters can take ring_mutex_ during the wait; their
        // eventfd write fires the multishot poll and returns us promptly.
        ::io_uring_cqe* cqe = nullptr;
        int rc = ::io_uring_wait_cqe_timeout(&ring_, &cqe, ts_ptr);

        // Phase 3 - drain CQEs under ring_mutex_. process_completions
        // splices the ready ops into completed_ops_ (taking the dispatch
        // mutex_) and notifies a follower.
        {
            lock_type ring_lock(ring_mutex_);
            if (rc == 0 || rc == -ETIME || rc == -EINTR)
                process_completions();
        }

        if (rc < 0 && rc != -ETIME && rc != -EINTR)
            detail::throw_system_error(
                make_err(-rc), "io_uring_wait_cqe_timeout");
    }

    if (!timer_svc_->empty())
        timer_svc_->process_expired();

    lock.lock();
}

inline void
io_uring_scheduler::process_completions()
{
    unsigned head;
    ::io_uring_cqe* cqe;
    unsigned consumed = 0;

    // Collect completed I/O ops locally; splice into completed_ops_
    // after the loop so do_one dispatches them one at a time.
    op_queue local_ops;

    std::int64_t inflight_dec = 0;
    io_uring_for_each_cqe(&ring_, head, cqe)
    {
        void* ud = io_uring_cqe_get_data(cqe);
        if (ud == nullptr)
        {
            // Wakeup eventfd CQE: drain the eventfd byte. Not counted
            // by io_uring_inflight_; we never incremented for the
            // wakeup multishot SQE (its progress doesn't depend on
            // userspace getevents).
            drain_wakeup_eventfd();
            // If multishot terminated (kernel dropped under memory
            // pressure or similar), re-arm. Each CQE except the last
            // sets IORING_CQE_F_MORE.
            if ((cqe->flags & IORING_CQE_F_MORE) == 0)
            {
                ::io_uring_sqe* re = ::io_uring_get_sqe(&ring_);
                if (!re)
                {
                    ::io_uring_submit(&ring_);
                    re = ::io_uring_get_sqe(&ring_);
                }
                if (re)
                {
                    ::io_uring_prep_poll_multishot(
                        re, wakeup_eventfd_, POLLIN);
                    ::io_uring_sqe_set_data(re, nullptr);
                }
            }
        }
        else if (ud == &cancel_sentinel_)
        {
            // CQE for an ASYNC_CANCEL op — ignore; the actual op's
            // CQE arrives separately and is dispatched via cqe_func.
            // Cancels are one-shot, no F_MORE, decrement inflight.
            ++inflight_dec;
        }
        else
        {
            auto* iop = static_cast<io_uring_op*>(ud);
            (*iop->cqe_func)(iop, cqe->res, cqe->flags, local_ops);
            // Decrement inflight on the terminal CQE only — multishot
            // ops (acceptor) hold the SQE alive across F_MORE CQEs and
            // free it only when F_MORE is cleared.
            if ((cqe->flags & IORING_CQE_F_MORE) == 0)
                ++inflight_dec;
        }
        ++consumed;
    }
    if (inflight_dec)
        io_uring_inflight_.fetch_sub(
            inflight_dec, std::memory_order_acq_rel);

    if (consumed)
        io_uring_cq_advance(&ring_, consumed);

    // Caller holds ring_mutex_. Take the base dispatch mutex_ briefly to
    // splice locally-collected ops onto the global queue (lock order
    // ring_mutex_ -> mutex_).
    if (!local_ops.empty())
    {
        lock_type lock(mutex_);
        completed_ops_.splice(local_ops);
        // Wake any follower waiting on cond_; it'll pop and dispatch.
        cond_.notify_one();
    }
}

inline void
io_uring_scheduler::submit_sqes_op::do_handler(
    void* owner, scheduler_op* base,
    std::uint32_t /*bytes*/, std::uint32_t /*error*/) noexcept
{
    if (owner == nullptr)
        return;   // shutdown drain — nothing to do; SQE storage is
                  // kernel-mapped and discarded by io_uring_queue_exit.

    auto* self  = static_cast<submit_sqes_op*>(base);
    auto* sched = self->sched_;

    io_uring_scheduler::lock_type ring_lock(sched->ring_mutex_);
    sched->submit_op_posted_ = false;
    ::io_uring_submit_and_get_events(&sched->ring_);
    sched->process_completions();
}

inline void
io_uring_scheduler::submit_cancel_by_user_data(io_uring_op* target) noexcept
{
    lazy_init_ring();
    // Wake the leader (if any) so its submit_and_wait_timeout returns
    // and releases ring_mutex_; otherwise we'd block here until the
    // next CQE arrives organically. Cancellation is best-effort if
    // the SQ stays full after one flush — the op completes on its
    // own and reports cancelled via the in-flight `cancelled` flag.
    interrupt_reactor();
    lock_type lock(ring_mutex_);
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe)
    {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
    }
    if (!sqe)
        return;

    io_uring_prep_cancel(sqe, target, 0);
    io_uring_sqe_set_data(sqe, &cancel_sentinel_);
    inflight_inc();
}

inline void
io_uring_scheduler::submit_cancel_by_fd(int fd) noexcept
{
    lazy_init_ring();
    interrupt_reactor();
    lock_type lock(ring_mutex_);
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe)
    {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
    }
    if (!sqe)
        return;

    io_uring_prep_cancel_fd(sqe, fd, IORING_ASYNC_CANCEL_ALL);
    io_uring_sqe_set_data(sqe, &cancel_sentinel_);
    inflight_inc();
}

inline void
io_uring_op::on_cancel() noexcept
{
    request_cancel();
    // Skip the cancel SQE if we never linked an SQE to this op — the
    // bypass path in the caller will see cancelled=true and complete
    // synchronously without a kernel round-trip.
    if (sched_ && sqe_set.load(std::memory_order_acquire))
        sched_->submit_cancel_by_user_data(this);
}

inline void
io_uring_scheduler::cancel_and_flush(int fd) noexcept
{
    lazy_init_ring();
    interrupt_reactor();
    lock_type lock(ring_mutex_);
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe)
    {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
    }
    if (sqe)
    {
        io_uring_prep_cancel_fd(sqe, fd, IORING_ASYNC_CANCEL_ALL);
        io_uring_sqe_set_data(sqe, &cancel_sentinel_);
        inflight_inc();
    }
    // Flush while fd is still open so the kernel resolves the file
    // from the fd number before the caller closes and recycles it.
    io_uring_submit(&ring_);
}

inline void
io_uring_scheduler::drain_cqes_for(io_uring_op* target) noexcept
{
    lazy_init_ring();
    // Submit a cancel by user_data so the kernel returns CQEs for
    // the target promptly, then iterate the CQ ring and consume
    // every CQE that matches `target`. ring_mutex_ serializes against
    // the leader's kernel wait and any concurrent cancel path; the
    // interrupt_reactor() ensures the leader returns promptly so we
    // can take the mutex.
    interrupt_reactor();
    {
        lock_type lock(ring_mutex_);
        if (auto* sqe = io_uring_get_sqe(&ring_))
        {
            io_uring_prep_cancel(sqe, target, 0);
            io_uring_sqe_set_data(sqe, &cancel_sentinel_);
            inflight_inc();
        }
        io_uring_submit(&ring_);
    }

    // Loop a few rounds: cancel SQE submission, then drain CQEs.
    // Bounded loop avoids stalls if the kernel never returns a
    // cancel completion — best-effort.
    for (int rounds = 0; rounds < drain_cqes_max_rounds; ++rounds)
    {
        lock_type lock(ring_mutex_);

        unsigned        head;
        ::io_uring_cqe* cqe;
        unsigned        consumed = 0;
        bool            saw_target = false;
        std::int64_t    inflight_dec = 0;

        io_uring_for_each_cqe(&ring_, head, cqe)
        {
            void* ud = io_uring_cqe_get_data(cqe);
            if (ud == target)
            {
                saw_target = true;
                // Don't dispatch — caller is destructing target;
                // just consume so the CQE doesn't dangle.
            }
            // Other CQEs are intentionally NOT dispatched here. They
            // may belong to ops freed by sibling teardowns (other
            // acceptors / sockets), and dispatching would UAF. The
            // next normal run-loop iteration will handle them; the
            // io_context's destructor sequence runs services'
            // shutdowns before ~scheduler so any still-live ops get
            // a chance to drain through their own paths first.
            //
            // We still account for the in-flight gate: a CQE consumed
            // here is one process_completions will never see, so it
            // must decrement io_uring_inflight_ on the same terms
            // (every counted SQE — real op or cancel, ud != nullptr —
            // contributes one decrement on its terminal, non-F_MORE
            // CQE). Skipping this strands the count permanently > 0,
            // which would defeat run_task's has_kernel_work early-out
            // for the remaining life of the ring.
            if (ud != nullptr &&
                (cqe->flags & IORING_CQE_F_MORE) == 0)
                ++inflight_dec;
            ++consumed;
        }
        if (inflight_dec)
            io_uring_inflight_.fetch_sub(
                inflight_dec, std::memory_order_acq_rel);
        if (consumed)
        {
            io_uring_cq_advance(&ring_, consumed);
            if (saw_target)
                break;
            continue;
        }

        // Nothing in the CQ — kick the kernel briefly. Hold
        // ring_mutex_ across the wait so we don't race with the
        // run-loop leader.
        __kernel_timespec ts{
            0, static_cast<long long>(drain_cqes_kick_ns)};
        ::io_uring_cqe* one = nullptr;
        int rc = ::io_uring_submit_and_wait_timeout(
            &ring_, &one, 1, &ts, nullptr);
        if (rc < 0 && rc != -ETIME && rc != -EINTR)
            break;
        if (rc == -ETIME)
            break;
    }
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IO_URING

#endif // BOOST_COROSIO_NATIVE_DETAIL_IO_URING_IO_URING_SCHEDULER_HPP
