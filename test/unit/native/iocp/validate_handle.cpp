//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

// Test that header file is self-contained.
#include <boost/corosio/native/detail/iocp/win_validate_handle.hpp>

#include "win_test_handles.hpp"
#include "test_suite.hpp"

namespace boost::corosio::detail {

struct win_validate_handle_test
{
    static constexpr handle_kind all_kinds[] = {
        handle_kind::stream_file, handle_kind::random_access_file,
        handle_kind::stream_handle, handle_kind::random_access_handle};

    static bool rejects(HANDLE h, handle_kind k)
    {
        return validate_overlapped_handle(h, k) ==
            std::errc::operation_not_supported;
    }

    void testOverlappedPipe()
    {
        auto p = test::make_pipe_pair();
        BOOST_TEST(p.server);
        BOOST_TEST(
            !validate_overlapped_handle(p.server.get(), handle_kind::stream_handle));
        BOOST_TEST(!validate_overlapped_handle(
            p.server.get(), handle_kind::random_access_handle));
        // The file types cannot position a pipe.
        BOOST_TEST(rejects(p.server.get(), handle_kind::stream_file));
        BOOST_TEST(rejects(p.server.get(), handle_kind::random_access_file));
    }

    void testSynchronousPipeEndRejected()
    {
        auto p = test::make_pipe_pair();
        for (auto k : all_kinds)
            BOOST_TEST(rejects(p.client.get(), k));
    }

    void testAnonymousPipeRejected()
    {
        HANDLE r = nullptr, w = nullptr;
        BOOST_TEST(::CreatePipe(&r, &w, nullptr, 0));
        test::unique_handle rr(r), ww(w);
        for (auto k : all_kinds)
        {
            BOOST_TEST(rejects(rr.get(), k));
            BOOST_TEST(rejects(ww.get(), k));
        }
    }

    void testDiskFile()
    {
        test::temp_path t("vh_disk");
        auto ov = test::open_file(t.path, true);
        BOOST_TEST(ov);
        BOOST_TEST(rejects(ov.get(), handle_kind::stream_handle));
        BOOST_TEST(!validate_overlapped_handle(
            ov.get(), handle_kind::random_access_handle));
        BOOST_TEST(
            !validate_overlapped_handle(ov.get(), handle_kind::stream_file));
        BOOST_TEST(!validate_overlapped_handle(
            ov.get(), handle_kind::random_access_file));
        ov.reset();

        auto sync = test::open_file(t.path, false);
        BOOST_TEST(sync);
        for (auto k : all_kinds)
            BOOST_TEST(rejects(sync.get(), k));
    }

    void testDirectoryRejected()
    {
        auto d = test::open_directory(std::filesystem::temp_directory_path());
        BOOST_TEST(d);
        for (auto k : all_kinds)
            BOOST_TEST(rejects(d.get(), k));
    }

    void testConsoleRejected()
    {
        test::unique_handle con(::CreateFileW(
            L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
            nullptr));
        if (!con)
            return; // no console attached (CI service session)
        for (auto k : all_kinds)
            BOOST_TEST(rejects(con.get(), k));
    }

    void testInvalidHandles()
    {
        for (auto k : all_kinds)
        {
            BOOST_TEST(
                validate_overlapped_handle(INVALID_HANDLE_VALUE, k) ==
                std::errc::bad_file_descriptor);
            BOOST_TEST(
                validate_overlapped_handle(nullptr, k) ==
                std::errc::bad_file_descriptor);
        }
        HANDLE closed = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ::CloseHandle(closed);
        BOOST_TEST(
            validate_overlapped_handle(closed, handle_kind::stream_handle) ==
            std::errc::bad_file_descriptor);
    }

    void testObjectEventKeepsSignal()
    {
        test::unique_handle ev(::CreateEventW(nullptr, FALSE, TRUE, nullptr));
        BOOST_TEST(!validate_object_handle(ev.get()));
        // The gate must not have consumed the auto-reset signal.
        BOOST_TEST_EQ(::WaitForSingleObject(ev.get(), 0), WAIT_OBJECT_0);
    }

    void testObjectSemaphoreKeepsCount()
    {
        test::unique_handle s(::CreateSemaphoreW(nullptr, 1, 1, nullptr));
        BOOST_TEST(!validate_object_handle(s.get()));
        BOOST_TEST_EQ(::WaitForSingleObject(s.get(), 0), WAIT_OBJECT_0);
    }

    void testObjectMutexRejected()
    {
        test::unique_handle m(::CreateMutexW(nullptr, FALSE, nullptr));
        BOOST_TEST(
            validate_object_handle(m.get()) ==
            std::errc::operation_not_supported);
    }

    void testObjectWithoutSynchronizeRejected()
    {
        test::unique_handle ev(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        HANDLE dup = nullptr;
        BOOST_TEST(::DuplicateHandle(
            ::GetCurrentProcess(), ev.get(), ::GetCurrentProcess(), &dup,
            EVENT_MODIFY_STATE, FALSE, 0));
        test::unique_handle d(dup);
        BOOST_TEST(
            validate_object_handle(d.get()) ==
            std::errc::operation_not_supported);
    }

    void testObjectInvalid()
    {
        BOOST_TEST(
            validate_object_handle(nullptr) == std::errc::bad_file_descriptor);
        BOOST_TEST(
            validate_object_handle(INVALID_HANDLE_VALUE) ==
            std::errc::bad_file_descriptor);
        HANDLE closed = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ::CloseHandle(closed);
        BOOST_TEST(
            validate_object_handle(closed) == std::errc::bad_file_descriptor);
    }

    void run()
    {
        testOverlappedPipe();
        testSynchronousPipeEndRejected();
        testAnonymousPipeRejected();
        testDiskFile();
        testDirectoryRejected();
        testConsoleRejected();
        testInvalidHandles();
        testObjectEventKeepsSignal();
        testObjectSemaphoreKeepsCount();
        testObjectMutexRejected();
        testObjectWithoutSynchronizeRejected();
        testObjectInvalid();
    }
};

TEST_SUITE(win_validate_handle_test, "boost.corosio.win_validate_handle");

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP
