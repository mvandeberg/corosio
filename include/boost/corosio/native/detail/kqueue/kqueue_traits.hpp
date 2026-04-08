//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_TRAITS_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_TRAITS_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/native/detail/reactor/reactor_descriptor_state.hpp>

#include <system_error>

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

/* kqueue backend traits.

   Captures the platform-specific behavior of the BSD/macOS kqueue backend:
   manual fcntl for O_NONBLOCK/FD_CLOEXEC, mandatory SO_NOSIGPIPE (macOS
   lacks MSG_NOSIGNAL), writev() for writes, and accept()+fcntl for
   accepted connections.
*/

namespace boost::corosio::detail {

class kqueue_scheduler;
struct descriptor_state;  // kqueue's descriptor_state in kqueue_op.hpp

struct kqueue_traits
{
    using scheduler_type    = kqueue_scheduler;
    using desc_state_type   = descriptor_state;

    static constexpr bool needs_write_notification = false;

    struct write_policy
    {
        static ssize_t write(int fd, iovec* iovecs, int count) noexcept
        {
            ssize_t n;
            do
            {
                n = ::writev(fd, iovecs, count);
            }
            while (n < 0 && errno == EINTR);
            return n;
        }
    };

    struct accept_policy
    {
        static int do_accept(int fd, sockaddr_storage& peer) noexcept
        {
            int new_fd;
            do
            {
                socklen_t addrlen = sizeof(peer);
                new_fd = ::accept(
                    fd, reinterpret_cast<sockaddr*>(&peer), &addrlen);
            }
            while (new_fd < 0 && errno == EINTR);

            if (new_fd < 0)
                return new_fd;

            int flags = ::fcntl(new_fd, F_GETFL, 0);
            if (flags == -1 ||
                ::fcntl(new_fd, F_SETFL, flags | O_NONBLOCK) == -1)
            {
                int err = errno;
                ::close(new_fd);
                errno = err;
                return -1;
            }

            if (::fcntl(new_fd, F_SETFD, FD_CLOEXEC) == -1)
            {
                int err = errno;
                ::close(new_fd);
                errno = err;
                return -1;
            }

            int one = 1;
            if (::setsockopt(
                    new_fd, SOL_SOCKET, SO_NOSIGPIPE,
                    &one, sizeof(one)) == -1)
            {
                int err = errno;
                ::close(new_fd);
                errno = err;
                return -1;
            }

            return new_fd;
        }
    };

    /// Create a socket, then apply O_NONBLOCK, FD_CLOEXEC, SO_NOSIGPIPE.
    static int create_socket(int family, int type, int protocol) noexcept
    {
        return ::socket(family, type, protocol);
    }

    /// Set O_NONBLOCK, FD_CLOEXEC, and SO_NOSIGPIPE on a new fd.
    /// Caller is responsible for closing fd on error.
    static std::error_code set_fd_options(int fd) noexcept
    {
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags == -1)
            return make_err(errno);
        if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
            return make_err(errno);
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
            return make_err(errno);

        int one = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0)
            return make_err(errno);

        return {};
    }

    /// Apply protocol-specific options after socket creation.
    /// For IP sockets, sets IPV6_V6ONLY on AF_INET6.
    static std::error_code
    configure_ip_socket(int fd, int family) noexcept
    {
        auto ec = set_fd_options(fd);
        if (ec)
            return ec;

        if (family == AF_INET6)
        {
            int v6only = 1;
            ::setsockopt(
                fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
        }
        return {};
    }

    /// Apply protocol-specific options for acceptor sockets.
    /// For IP acceptors, sets IPV6_V6ONLY=0 (dual-stack).
    static std::error_code
    configure_ip_acceptor(int fd, int family) noexcept
    {
        auto ec = set_fd_options(fd);
        if (ec)
            return ec;

        if (family == AF_INET6)
        {
            int val = 0;
            ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof(val));
        }
        return {};
    }

    /// Apply options for local (unix) sockets.
    static std::error_code
    configure_local_socket(int fd) noexcept
    {
        return set_fd_options(fd);
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE

#endif // BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_TRAITS_HPP
