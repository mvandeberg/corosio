//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_SRC_DETAIL_CONDITIONAL_MUTEX_HPP
#define BOOST_COROSIO_SRC_DETAIL_CONDITIONAL_MUTEX_HPP

#include <chrono>
#include <condition_variable>
#include <mutex>

/*
    Conditional locking primitives for single-threaded optimization.

    When concurrency_hint == 1, the user guarantees single-threaded access.
    All mutex operations become no-ops, eliminating pthread_mutex overhead
    on every I/O operation. enabled_ and spin_count_ are const for better
    codegen (compiler can hoist/eliminate the enabled_ branch).

    lock() optionally spins (spin_count iterations of try_lock) before
    falling back to the OS mutex. spin_count defaults to 0 (no spinning);
    set to -1 for infinite spin (busy-wait). conditional_unique_lock does
    NOT spin on construction (hot path), only on re-acquisition via lock().

    conditional_mutex satisfies BasicLockable, so std::lock_guard works
    via CTAD. The scheduler uses conditional_unique_lock + conditional_event
    because std::condition_variable::wait() requires std::unique_lock<std::mutex>.
*/

namespace boost::corosio::detail {

class conditional_mutex
{
public:
    explicit conditional_mutex(bool enabled = true, int spin_count = 0) noexcept
        : spin_count_(spin_count)
        , enabled_(enabled)
    {
    }

    conditional_mutex(conditional_mutex const&) = delete;
    conditional_mutex& operator=(conditional_mutex const&) = delete;

    void lock()
    {
        if (enabled_)
        {
            for (int n = spin_count_; n != 0; n -= (n > 0) ? 1 : 0)
                if (mutex_.try_lock())
                    return;
            mutex_.lock();
        }
    }

    void unlock()
    {
        if (enabled_)
            mutex_.unlock();
    }

    bool try_lock()
    {
        return !enabled_ || mutex_.try_lock();
    }

    bool enabled() const noexcept { return enabled_; }
    int spin_count() const noexcept { return spin_count_; }
    std::mutex& underlying() noexcept { return mutex_; }

private:
    std::mutex mutex_;
    const int spin_count_;
    const bool enabled_;
};

class conditional_unique_lock
{
public:
    // Tag type for adopting an already-held lock.
    enum adopt_lock_t { adopt_lock };

    // Constructor adopts a lock that is already held.
    conditional_unique_lock(conditional_mutex& m, adopt_lock_t)
        : mutex_(m)
        , locked_(m.enabled())
    {
    }

    // Constructor acquires the lock (no spin — hot path).
    explicit conditional_unique_lock(conditional_mutex& m)
        : mutex_(m)
    {
        if (m.enabled())
        {
            mutex_.underlying().lock();
            locked_ = true;
        }
        else
            locked_ = false;
    }

    conditional_unique_lock(conditional_unique_lock const&) = delete;
    conditional_unique_lock& operator=(conditional_unique_lock const&) = delete;

    ~conditional_unique_lock()
    {
        if (locked_)
            mutex_.underlying().unlock();
    }

    // Re-acquire the lock (spins if spin_count configured).
    void lock()
    {
        if (mutex_.enabled() && !locked_)
        {
            for (int n = mutex_.spin_count(); n != 0; n -= (n > 0) ? 1 : 0)
            {
                if (mutex_.underlying().try_lock())
                {
                    locked_ = true;
                    return;
                }
            }
            mutex_.underlying().lock();
            locked_ = true;
        }
    }

    void unlock()
    {
        if (locked_)
        {
            mutex_.unlock();
            locked_ = false;
        }
    }

    bool locked() const noexcept
    {
        return locked_;
    }

    // For backward compatibility with call sites using owns_lock().
    bool owns_lock() const noexcept
    {
        return locked_;
    }

    conditional_mutex& mutex() noexcept { return mutex_; }

private:
    conditional_mutex& mutex_;
    bool locked_;
};

class conditional_event
{
public:
    void notify_one() { cond_.notify_one(); }
    void notify_all() { cond_.notify_all(); }

    void wait(conditional_unique_lock& lock)
    {
        if (lock.locked())
        {
            std::unique_lock<std::mutex> ul(
                lock.mutex().underlying(), std::adopt_lock);
            cond_.wait(ul);
            ul.release();
        }
    }

    template<class Rep, class Period>
    void wait_for(
        conditional_unique_lock& lock,
        std::chrono::duration<Rep, Period> const& dur)
    {
        if (lock.locked())
        {
            std::unique_lock<std::mutex> ul(
                lock.mutex().underlying(), std::adopt_lock);
            cond_.wait_for(ul, dur);
            ul.release();
        }
    }

private:
    std::condition_variable cond_;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_SRC_DETAIL_CONDITIONAL_MUTEX_HPP
