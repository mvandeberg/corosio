//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_NATIVE_POSIX_DESCRIPTOR_HPP
#define BOOST_COROSIO_NATIVE_NATIVE_POSIX_DESCRIPTOR_HPP

#include <boost/corosio/posix_descriptor.hpp>
#include <boost/corosio/backend.hpp>

#if BOOST_COROSIO_POSIX || defined(BOOST_COROSIO_MRDOCS)

#ifndef BOOST_COROSIO_MRDOCS
#if BOOST_COROSIO_HAS_EPOLL
#include <boost/corosio/native/detail/epoll/epoll_types.hpp>
#endif

#if BOOST_COROSIO_HAS_SELECT
#include <boost/corosio/native/detail/select/select_types.hpp>
#endif

#if BOOST_COROSIO_HAS_KQUEUE
#include <boost/corosio/native/detail/kqueue/kqueue_types.hpp>
#endif

#if BOOST_COROSIO_HAS_URING
// uring_types.hpp does not declare the descriptor: it was added
// after that header, in its own pair of files.
#include <boost/corosio/native/detail/uring/uring_descriptor_service.hpp>
#endif
#endif // !BOOST_COROSIO_MRDOCS

namespace boost::corosio {

/** An adopted POSIX descriptor with devirtualized I/O operations.

    This class template inherits from @ref posix_descriptor and
    shadows the async operations (`read_some`, `write_some`, `wait`)
    with versions that call the backend implementation directly,
    allowing the compiler to inline through the entire call chain.

    Non-async operations (`assign`, `release`, `close`, `cancel`)
    remain unchanged and dispatch through the compiled library.

    A `native_posix_descriptor` IS-A `posix_descriptor` and can be
    passed to any function expecting `posix_descriptor&` or
    `io_stream&`, in which case virtual dispatch is used
    transparently.

    @tparam Backend A backend tag value (e.g., `epoll`) whose type
        provides the concrete implementation types.

    @par Thread Safety
    Same as @ref posix_descriptor.

    @see posix_descriptor, epoll_t, kqueue_t
*/
template<auto Backend>
class native_posix_descriptor : public posix_descriptor
{
    using backend_type = decltype(Backend);
    using impl_type    = typename backend_type::descriptor_type;
    using service_type = typename backend_type::descriptor_service_type;

    impl_type& get_impl() noexcept
    {
        return *static_cast<impl_type*>(h_.get());
    }

    template<class MutableBufferSequence>
    struct native_read_awaitable
    {
        native_posix_descriptor& self_;
        MutableBufferSequence buffers_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable std::size_t bytes_transferred_ = 0;

        native_read_awaitable(
            native_posix_descriptor& self,
            MutableBufferSequence buffers) noexcept
            : self_(self)
            , buffers_(std::move(buffers))
        {
        }

        bool await_ready() const noexcept
        {
            // A pre-set ec_ means the initiator failed before
            // dispatch (e.g. a closed object).
            return static_cast<bool>(ec_) || token_.stop_requested();
        }

        [[nodiscard]] capy::io_result<std::size_t> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {make_error_code(std::errc::operation_canceled), 0};
            return {ec_, bytes_transferred_};
        }

        auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
            -> std::coroutine_handle<>
        {
            token_ = env->stop_token;
            return self_.get_impl().read_some(
                h, env->executor, buffers_, token_, &ec_, &bytes_transferred_);
        }
    };

    template<class ConstBufferSequence>
    struct native_write_awaitable
    {
        native_posix_descriptor& self_;
        ConstBufferSequence buffers_;
        std::stop_token token_;
        mutable std::error_code ec_;
        mutable std::size_t bytes_transferred_ = 0;

        native_write_awaitable(
            native_posix_descriptor& self, ConstBufferSequence buffers) noexcept
            : self_(self)
            , buffers_(std::move(buffers))
        {
        }

        bool await_ready() const noexcept
        {
            // A pre-set ec_ means the initiator failed before
            // dispatch (e.g. a closed object).
            return static_cast<bool>(ec_) || token_.stop_requested();
        }

        [[nodiscard]] capy::io_result<std::size_t> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {make_error_code(std::errc::operation_canceled), 0};
            return {ec_, bytes_transferred_};
        }

        auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
            -> std::coroutine_handle<>
        {
            token_ = env->stop_token;
            return self_.get_impl().write_some(
                h, env->executor, buffers_, token_, &ec_, &bytes_transferred_);
        }
    };

    struct native_wait_awaitable
    {
        native_posix_descriptor& self_;
        wait_type w_;
        std::stop_token token_;
        mutable std::error_code ec_;

        native_wait_awaitable(
            native_posix_descriptor& self, wait_type w) noexcept
            : self_(self)
            , w_(w)
        {
        }

        bool await_ready() const noexcept
        {
            // A pre-set ec_ means the initiator failed before
            // dispatch (e.g. a closed object).
            return static_cast<bool>(ec_) || token_.stop_requested();
        }

        [[nodiscard]] capy::io_result<> await_resume() const noexcept
        {
            if (token_.stop_requested())
                return {make_error_code(std::errc::operation_canceled)};
            return {ec_};
        }

        auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
            -> std::coroutine_handle<>
        {
            token_ = env->stop_token;
            return self_.get_impl().wait(h, env->executor, w_, token_, &ec_);
        }
    };

public:
    /** Construct a native descriptor from an execution context.

        @param ctx The execution context that will own this object.
    */
    explicit native_posix_descriptor(capy::execution_context& ctx)
        : io_object(create_handle<service_type>(ctx))
    {
    }

    /** Construct a native descriptor from an executor.

        @param ex The executor whose context will own this object.
    */
    template<class Ex>
        requires(!std::same_as<
                    std::remove_cvref_t<Ex>,
                    native_posix_descriptor>) &&
        capy::Executor<Ex>
    explicit native_posix_descriptor(Ex const& ex)
        : native_posix_descriptor(ex.context())
    {
    }

    /// Move construct.
    native_posix_descriptor(native_posix_descriptor&&) noexcept = default;

    /// Move assign.
    native_posix_descriptor&
    operator=(native_posix_descriptor&&) noexcept = default;

    native_posix_descriptor(native_posix_descriptor const&)            = delete;
    native_posix_descriptor& operator=(native_posix_descriptor const&) = delete;

    /** Asynchronously read data from the descriptor.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref io_stream::read_some.

        @param buffers The buffer sequence to read into.

        @return An awaitable yielding `(error_code, std::size_t)`.
    */
    template<capy::MutableBufferSequence MB>
    [[nodiscard]] auto read_some(MB const& buffers)
    {
        return native_read_awaitable<MB>(*this, buffers);
    }

    /** Asynchronously write data to the descriptor.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref io_stream::write_some.

        @param buffers The buffer sequence to write from.

        @return An awaitable yielding `(error_code, std::size_t)`.
    */
    template<capy::ConstBufferSequence CB>
    [[nodiscard]] auto write_some(CB const& buffers)
    {
        return native_write_awaitable<CB>(*this, buffers);
    }

    /** Wait for readiness without transferring bytes.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref posix_descriptor::wait.

        @param w The direction to wait on.

        @return An awaitable yielding `io_result<>`.
    */
    [[nodiscard]] auto wait(wait_type w)
    {
        return native_wait_awaitable(*this, w);
    }
};

} // namespace boost::corosio

#endif // BOOST_COROSIO_POSIX || BOOST_COROSIO_MRDOCS

#endif // BOOST_COROSIO_NATIVE_NATIVE_POSIX_DESCRIPTOR_HPP
