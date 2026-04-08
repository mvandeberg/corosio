# Reactor Backend Collapse — Follow-up Issues

From the parameterized reactor backend refactoring (commit 05d510fc).

## 1. Dead code and duplicate policies in old op files

`epoll_op.hpp`, `select_op.hpp`, `kqueue_op.hpp` each contain:
- `descriptor_state` subclass — **still needed** (schedulers depend on it)
- `write_policy` / `accept_policy` — **duplicated** in traits files (bug risk: editing the old one has no effect)
- Old op type declarations (`epoll_op`, `epoll_connect_op`, etc.) — **dead code**, never instantiated
- Old forward declarations of deleted socket types — dead code

**Fix:** Strip op files to just `descriptor_state`. Remove policies and dead op types.

## 2. kqueue SO_LINGER pre_shutdown hook is missing

The old `kqueue_tcp_service` had per-socket `user_set_linger_` state and
`pre_shutdown`/`pre_destroy` hooks to reset SO_LINGER before close (macOS
kqueue workaround: RST doesn't reliably trigger EV_EOF). The parameterized
`reactor_stream_socket_final` has no per-socket state for this.

All tests pass on Linux (epoll+select). Will fail on macOS (kqueue).

**Fix:** Add kqueue-specific `set_option()` override and linger tracking,
either via traits hook or a kqueue TCP service specialization.

## 3. Service macros should be replaced

`COROSIO_REACTOR_SOCKET_SERVICE` and `COROSIO_REACTOR_ACCEPTOR_SERVICE`
macros hide code from IDE navigation and error traces. Only 6 call sites.

**Fix:** Inline the 4-line pattern or restructure the CRTP constructor access.

## 4. Unconditional protocol-specific methods on CRTP bases

`shutdown()`, `bind()`, `release_socket()` added unconditionally to
`reactor_datagram_socket` and `reactor_stream_socket`. For UDP/TCP
instantiations these are extra non-virtual methods with no semantic meaning.
Low risk (internal detail types) but imprecise.

## 5. reactor_backend.hpp is monolithic (~750 lines)

Defines 9 class templates, 3 helpers, the accept implementation, and the
type bundle in one file. Hard to navigate.

**Fix:** Split into `reactor_socket_finals.hpp`, `reactor_service_finals.hpp`,
`reactor_acceptor_finals.hpp`.
