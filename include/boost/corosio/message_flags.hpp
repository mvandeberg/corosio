//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_MESSAGE_FLAGS_HPP
#define BOOST_COROSIO_MESSAGE_FLAGS_HPP

namespace boost::corosio {

/* Flags for datagram send/recv operations.

   Platform-agnostic flag values that are mapped to native
   constants (MSG_PEEK, MSG_OOB, MSG_DONTROUTE) at the
   syscall boundary in the reactor implementation.
*/
enum class message_flags : int
{
    none         = 0,
    peek         = 1,
    out_of_band  = 2,
    do_not_route = 4
};

inline constexpr message_flags
operator|(message_flags a, message_flags b) noexcept
{
    return static_cast<message_flags>(
        static_cast<int>(a) | static_cast<int>(b));
}

inline constexpr message_flags
operator&(message_flags a, message_flags b) noexcept
{
    return static_cast<message_flags>(
        static_cast<int>(a) & static_cast<int>(b));
}

inline constexpr message_flags
operator~(message_flags a) noexcept
{
    return static_cast<message_flags>(~static_cast<int>(a));
}

} // namespace boost::corosio

#endif // BOOST_COROSIO_MESSAGE_FLAGS_HPP
