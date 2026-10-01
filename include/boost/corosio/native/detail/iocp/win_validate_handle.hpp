//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_VALIDATE_HANDLE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_VALIDATE_HANDLE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/native/detail/iocp/win_windows.hpp>

#include <cstring>
#include <system_error>

/* The adopt-time gates for Windows handles -- the counterpart of
   validate_fd.hpp. A gate only inspects the handle. None performs I/O
   or waits on it, so an auto-reset event or a semaphore keeps its
   signal across a gate, accepted or rejected.

   Synchronous-mode handles are rejected by querying the file mode
   rather than by letting CreateIoCompletionPort fail: whether it
   refuses such a handle is undocumented, and an overlapped ReadFile on
   one silently runs to completion inside the call.
*/

namespace boost::corosio::detail {

/// The adopting type; the gates differ per type.
enum class handle_kind
{
    stream_file,
    random_access_file,
    stream_handle,
    random_access_handle
};

namespace win_nt {

using ntstatus = LONG;

struct io_status_block
{
    union
    {
        ntstatus Status;
        void* Pointer;
    };
    ULONG_PTR Information;
};

using query_information_file_fn =
    ntstatus(NTAPI*)(HANDLE, io_status_block*, void*, ULONG, int);
using query_object_fn = ntstatus(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);

inline constexpr int file_mode_information    = 16;
inline constexpr int object_basic_information = 0;
inline constexpr int object_type_information  = 2;

inline constexpr ULONG file_synchronous_io_alert    = 0x10;
inline constexpr ULONG file_synchronous_io_nonalert = 0x20;

struct object_basic_info
{
    ULONG Attributes;
    ACCESS_MASK GrantedAccess;
    ULONG HandleCount;
    ULONG PointerCount;
    ULONG Reserved[10];
};

struct unicode_string
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

// The two-step cast through void(*)() is the sanctioned FARPROC
// conversion; a direct cast trips -Wcast-function-type.
template<class Fn>
Fn
ntdll_proc(char const* name) noexcept
{
    if (HMODULE h = ::GetModuleHandleW(L"ntdll.dll"))
        return reinterpret_cast<Fn>(
            reinterpret_cast<void (*)()>(::GetProcAddress(h, name)));
    return nullptr;
}

inline query_information_file_fn
query_information_file() noexcept
{
    static query_information_file_fn const fn =
        ntdll_proc<query_information_file_fn>("NtQueryInformationFile");
    return fn;
}

inline query_object_fn
query_object() noexcept
{
    static query_object_fn const fn =
        ntdll_proc<query_object_fn>("NtQueryObject");
    return fn;
}

} // namespace win_nt

/** Validate a handle for adoption by an overlapped type.

    @param h The handle to inspect. Never read, written or waited on.
    @param kind The adopting type.

    @return `bad_file_descriptor` for a null, invalid or closed
        handle; `operation_not_supported` for a console, a
        synchronous-mode handle, a directory, a disk handle adopted
        by `win_stream_handle`, or a pipe adopted by a file type;
        otherwise an empty code. A missing `ntdll` entry point fails
        closed with `operation_not_supported`.
*/
inline std::error_code
validate_overlapped_handle(HANDLE h, handle_kind kind) noexcept
{
    auto const not_supported =
        std::make_error_code(std::errc::operation_not_supported);

    if (h == nullptr || h == INVALID_HANDLE_VALUE)
        return std::make_error_code(std::errc::bad_file_descriptor);

    ::SetLastError(NO_ERROR);
    DWORD const type = ::GetFileType(h);
    if (type == FILE_TYPE_UNKNOWN && ::GetLastError() != NO_ERROR)
        return std::make_error_code(std::errc::bad_file_descriptor);

    DWORD console_mode = 0;
    if (type == FILE_TYPE_CHAR && ::GetConsoleMode(h, &console_mode))
        return not_supported;

    auto const query = win_nt::query_information_file();
    if (!query)
        return not_supported;
    win_nt::io_status_block iosb{};
    ULONG mode = 0;
    if (query(h, &iosb, &mode, sizeof(mode), win_nt::file_mode_information) <
        0)
        return not_supported;
    if (mode &
        (win_nt::file_synchronous_io_alert |
         win_nt::file_synchronous_io_nonalert))
        return not_supported;

    // A failed query (volumes, some devices) means "not a directory".
    FILE_BASIC_INFO basic{};
    if (::GetFileInformationByHandleEx(
            h, FileBasicInfo, &basic, sizeof(basic)) &&
        (basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return not_supported;

    switch (kind)
    {
    case handle_kind::stream_handle:
        if (type == FILE_TYPE_DISK)
            return not_supported;
        break;
    case handle_kind::stream_file:
    case handle_kind::random_access_file:
        if (type == FILE_TYPE_PIPE)
            return not_supported;
        break;
    case handle_kind::random_access_handle:
        break;
    }
    return {};
}

/** Validate a handle for adoption by `win_object_handle`.

    @return `bad_file_descriptor` for a null, invalid or closed
        handle; `operation_not_supported` for a mutex, a handle
        without `SYNCHRONIZE` access, or a missing `ntdll` entry
        point; otherwise an empty code.
*/
inline std::error_code
validate_object_handle(HANDLE h) noexcept
{
    auto const not_supported =
        std::make_error_code(std::errc::operation_not_supported);

    if (h == nullptr || h == INVALID_HANDLE_VALUE)
        return std::make_error_code(std::errc::bad_file_descriptor);

    auto const query = win_nt::query_object();
    if (!query)
        return not_supported;

    win_nt::object_basic_info basic{};
    if (query(
            h, win_nt::object_basic_information, &basic, sizeof(basic),
            nullptr) < 0)
        return std::make_error_code(std::errc::bad_file_descriptor);
    if (!(basic.GrantedAccess & SYNCHRONIZE))
        return not_supported;

    // A satisfied mutex wait acquires the mutex on a pool thread the
    // resuming coroutine does not own.
    alignas(8) unsigned char buf[1024];
    if (query(h, win_nt::object_type_information, buf, sizeof(buf), nullptr) <
        0)
        return not_supported;
    win_nt::unicode_string name;
    std::memcpy(&name, buf, sizeof(name));
    static constexpr wchar_t mutant[] = L"Mutant";
    if (name.Buffer && name.Length == sizeof(mutant) - sizeof(wchar_t) &&
        std::memcmp(name.Buffer, mutant, name.Length) == 0)
        return not_supported;

    return {};
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif
