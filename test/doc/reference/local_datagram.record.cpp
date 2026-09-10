//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Reference example injected into include/boost/corosio/local_datagram.hpp's
// documentation for local_datagram, by
// doc/addons/extensions/reference-snippets.lua. The tagged region is what the
// reference renders; scaffolding stays outside the tags.
//
// local_datagram's whole header is wrapped in #if BOOST_COROSIO_POSIX
// (Windows has no AF_UNIX SOCK_DGRAM support), so the region that names the
// type is guarded the same way, following local_datagram_socket.record.cpp.

#include "../doc_warnings.hpp"

#include <boost/corosio/detail/platform.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/local_datagram.hpp>
#include <boost/corosio/local_datagram_socket.hpp>

namespace corosio = boost::corosio;

namespace {

#if BOOST_COROSIO_POSIX
// tag::open_with_protocol[]
void
open_with_protocol(corosio::io_context& ctx)
{
    corosio::local_datagram_socket sock(ctx);
    if (auto ec = sock.open(corosio::local_datagram{}))
        return;
}
// end::open_with_protocol[]
#endif

} // namespace
