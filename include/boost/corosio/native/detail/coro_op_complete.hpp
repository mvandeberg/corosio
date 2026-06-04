//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_CORO_OP_COMPLETE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_CORO_OP_COMPLETE_HPP

#include <boost/corosio/detail/dispatch_coro.hpp>
#include <boost/corosio/native/detail/coro_op.hpp>

#include <memory>

/*
    Shared completion-tail helpers for proactor ops. Every IOCP and io_uring
    I/O handler ends the same way once its backend-specific result has been
    decoded into ec_out/bytes_out:

      1. disarm the stop_callback,
      2. on the shutdown-drain path (owner == nullptr) just break the
         impl_ptr keepalive cycle and return without resuming,
      3. otherwise resume the coroutine on its executor, dropping the
         keepalive only after the continuation has been handed off.

    The *decode* step (raw DWORD/res -> {ec, bytes, eof, canceled}) stays
    backend-specific because the raw encodings differ; in Phase 3 it is
    formalized as `Traits::decode_result`. These two helpers capture the
    backend-agnostic prologue and resume tail so the per-op handlers shrink to
    "drain-or-decode, then resume".
*/

namespace boost::corosio::detail {

/** Completion prologue shared by every proactor handler.

    Disarms the stop_callback, then detects the shutdown-drain path.

    @param owner The scheduler pointer (nullptr during shutdown drain).
    @param self  The completing op.
    @return True if this was a shutdown drain — the caller must `return`
            immediately without decoding or resuming. On that path the
            impl_ptr keepalive is dropped here (which may destroy the impl,
            and with it the op storage).
*/
inline bool
coro_drain_if_shutdown(void* owner, coro_op* self) noexcept
{
    self->stop_cb.reset();
    if (owner == nullptr)
    {
        auto suicide = std::move(self->impl_ptr);
        return true;
    }
    return false;
}

/** Resume tail shared by every proactor handler.

    Resumes the op's coroutine on its executor and then drops the impl_ptr
    keepalive. The keepalive is moved into a local that is released *after*
    `resume()` returns, matching the existing io_uring ordering: the impl (and
    therefore this op's storage) may be destroyed as the local goes out of
    scope, so nothing may touch `*self` after the resume.

    @pre `self->ec_out`/`bytes_out` have already been written by the
         backend's decode step.
*/
inline void
coro_resume(coro_op* self) noexcept
{
    self->cont_op.cont.h = self->h;
    auto next = dispatch_coro(self->ex, self->cont_op.cont);
    auto suicide = std::move(self->impl_ptr);
    next.resume();
    // suicide drops here; may destroy impl + self.
}

} // namespace boost::corosio::detail

#endif
