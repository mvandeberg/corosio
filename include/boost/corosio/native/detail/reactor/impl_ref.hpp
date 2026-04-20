//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_IMPL_REF_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_IMPL_REF_HPP

/* Lightweight intrusive reference counting for reactor socket/acceptor
   implementations, replacing std::shared_ptr + std::unordered_map.

   ref_counted_base   — mixin base with atomic refcount and freelist linkage
   impl_ref           — move-only RAII handle (replaces shared_ptr<void>)
   impl_freelist<T>   — singly-linked intrusive freelist for recycling impls
*/

#include <atomic>
#include <utility>

namespace boost::corosio::detail {

/** Intrusive reference-count base for socket/acceptor implementations.

    Embedded in every reactor impl object. The owning service sets
    ref_count_ to 1 on construct and decrements on destroy. Pending
    async operations increment/decrement via impl_ref. When the count
    reaches zero the release function returns the impl to its service's
    freelist.
*/
struct ref_counted_base
{
    std::atomic<int> ref_count_{0};

    /// Called when ref_count_ drops to zero. Set by the service's
    /// construct() where the concrete Impl type is known.
    void (*release_fn_)(ref_counted_base*) noexcept = nullptr;

    /// Singly-linked freelist pointer. Only valid when the impl is
    /// on the freelist (ref_count_ == 0, not on the active list).
    ref_counted_base* next_free_ = nullptr;

    void add_ref() noexcept
    {
        ref_count_.fetch_add(1, std::memory_order_relaxed);
    }

    /// Decrement and return true when the caller dropped the last ref.
    bool sub_ref() noexcept
    {
        return ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1;
    }
};

/** Move-only RAII wrapper that prevents an impl from being recycled.

    Replaces std::shared_ptr<void> in reactor_op_base::impl_ptr and
    reactor_descriptor_state::impl_ref_. Eight bytes (one pointer)
    instead of sixteen, no control-block allocation, and the
    increment/decrement is a single atomic rather than two.
*/
class impl_ref
{
    ref_counted_base* ptr_ = nullptr;

public:
    impl_ref() noexcept = default;

    /// Take a new reference to p (increments refcount).
    explicit impl_ref(ref_counted_base* p) noexcept : ptr_(p)
    {
        if (ptr_)
            ptr_->add_ref();
    }

    ~impl_ref() { reset(); }

    impl_ref(impl_ref&& o) noexcept
        : ptr_(std::exchange(o.ptr_, nullptr))
    {
    }

    impl_ref& operator=(impl_ref&& o) noexcept
    {
        if (this != &o)
        {
            reset();
            ptr_ = std::exchange(o.ptr_, nullptr);
        }
        return *this;
    }

    impl_ref(impl_ref const&)            = delete;
    impl_ref& operator=(impl_ref const&) = delete;

    void reset() noexcept
    {
        if (ptr_)
        {
            auto* p = ptr_;
            ptr_    = nullptr;
            if (p->sub_ref())
                p->release_fn_(p);
        }
    }

    explicit operator bool() const noexcept { return ptr_ != nullptr; }
};

/** Singly-linked intrusive freelist for recycling impl objects.

    Impls are returned here instead of being deleted. construct()
    pops from the freelist before falling back to operator new.
    The destructor deletes all pooled objects.

    Not thread-safe — callers must hold the service mutex.
*/
template<class Impl>
class impl_freelist
{
    Impl* head_ = nullptr;

public:
    impl_freelist() noexcept = default;

    ~impl_freelist()
    {
        while (auto* p = pop())
            delete p;
    }

    impl_freelist(impl_freelist const&)            = delete;
    impl_freelist& operator=(impl_freelist const&) = delete;

    Impl* pop() noexcept
    {
        if (!head_)
            return nullptr;
        auto* p      = head_;
        head_        = static_cast<Impl*>(p->next_free_);
        p->next_free_ = nullptr;
        return p;
    }

    void push(Impl* p) noexcept
    {
        p->next_free_ = head_;
        head_          = p;
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_IMPL_REF_HPP
