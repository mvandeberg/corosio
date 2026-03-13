//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "benchmarks.hpp"

#include <boost/corosio/io_context.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "../../common/native_includes.hpp"

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace corosio_bench {
namespace {

capy::task<>
increment_task(int64_t& counter)
{
    ++counter;
    co_return;
}

capy::task<>
atomic_increment_task(std::atomic<int64_t>& counter)
{
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

template<auto Backend>
void
bench_single_threaded_post(bench::state& state)
{
    corosio::native_io_context<Backend> ioc;
    auto ex                  = ioc.get_executor();
    int64_t counter          = 0;
    int constexpr batch_size = 1000;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(state.duration());

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int i = 0; i < batch_size; ++i)
            capy::run_async(ex)(increment_task(counter));

        ioc.poll();
        ioc.restart();
    }

    ioc.run();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

template<auto Backend>
void
bench_multithreaded_scaling(bench::state& state)
{
    int max_threads = static_cast<int>(state.range(0));

    corosio::native_io_context<Backend> ioc;
    auto ex = ioc.get_executor();
    std::atomic<bool> running{true};
    std::atomic<int64_t> counter{0};

    int constexpr batch_size = 100000;

    for (int i = 0; i < batch_size; ++i)
        capy::run_async(ex)(atomic_increment_task(counter));

    perf::stopwatch sw;

    std::thread feeder([&]() {
        auto deadline = std::chrono::steady_clock::now() +
            std::chrono::duration<double>(state.duration());

        while (std::chrono::steady_clock::now() < deadline)
        {
            for (int i = 0; i < batch_size; ++i)
                capy::run_async(ex)(atomic_increment_task(counter));
            std::this_thread::yield();
        }
        running.store(false, std::memory_order_relaxed);
    });

    std::vector<std::thread> runners;
    for (int t = 0; t < max_threads; ++t)
        runners.emplace_back([&ioc, &running]() {
            while (running.load(std::memory_order_relaxed))
            {
                ioc.poll();
                ioc.restart();
            }
            ioc.run();
        });

    feeder.join();
    for (auto& t : runners)
        t.join();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter.load());
    state.counters["threads"] = max_threads;
}

template<auto Backend>
void
bench_interleaved_post_run(bench::state& state)
{
    int handlers_per_iteration = 100;

    corosio::native_io_context<Backend> ioc;
    auto ex         = ioc.get_executor();
    int64_t counter = 0;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(state.duration());

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int i = 0; i < handlers_per_iteration; ++i)
            capy::run_async(ex)(increment_task(counter));

        ioc.poll();
        ioc.restart();
    }

    ioc.run();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

template<auto Backend>
void
bench_concurrent_post_run(bench::state& state)
{
    int num_threads = static_cast<int>(state.range(0));

    corosio::native_io_context<Backend> ioc;
    auto ex = ioc.get_executor();
    std::atomic<bool> running{true};
    std::atomic<int64_t> counter{0};

    int constexpr batch_size = 10000;

    perf::stopwatch sw;

    std::vector<std::thread> workers;
    for (int t = 0; t < num_threads; ++t)
    {
        workers.emplace_back([&]() {
            while (running.load(std::memory_order_relaxed))
            {
                for (int i = 0; i < batch_size; ++i)
                    capy::run_async(ex)(atomic_increment_task(counter));
                ioc.poll();
                ioc.restart();
            }
            ioc.run();
        });
    }

    std::thread timer([&]() {
        std::this_thread::sleep_for(
            std::chrono::duration<double>(state.duration()));
        running.store(false, std::memory_order_relaxed);
    });

    timer.join();
    for (auto& t : workers)
        t.join();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter.load());
    state.counters["threads"] = num_threads;
}

// -----------------------------------------------------------------
// Config-variant benchmarks
// -----------------------------------------------------------------

#if BOOST_COROSIO_HAS_EPOLL

// Compile-time: recycle_post_nodes disabled
void
bench_no_recycle_ct(bench::state& state)
{
    constexpr corosio::epoll_config cfg{.recycle_post_nodes = false};
    corosio::native_io_context<corosio::epoll_t<cfg>{}> ioc;
    auto ex                  = ioc.get_executor();
    int64_t counter          = 0;
    int constexpr batch_size = 1000;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(state.duration());

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int i = 0; i < batch_size; ++i)
            capy::run_async(ex)(increment_task(counter));
        ioc.poll();
        ioc.restart();
    }
    ioc.run();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

// Runtime: recycle_post_nodes disabled via io_context_options
void
bench_no_recycle_rt(bench::state& state)
{
    corosio::io_context ioc(corosio::io_context_options{
        .recycle_post_nodes = false});
    auto ex                  = ioc.get_executor();
    int64_t counter          = 0;
    int constexpr batch_size = 1000;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(state.duration());

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int i = 0; i < batch_size; ++i)
            capy::run_async(ex)(increment_task(counter));
        ioc.poll();
        ioc.restart();
    }
    ioc.run();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

// Compile-time: high inline budget
void
bench_high_inline_budget(bench::state& state)
{
    constexpr corosio::epoll_config cfg{.inline_budget_max = 64};
    corosio::native_io_context<corosio::epoll_t<cfg>{}> ioc;
    auto ex                  = ioc.get_executor();
    int64_t counter          = 0;
    int constexpr batch_size = 1000;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(state.duration());

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int i = 0; i < batch_size; ++i)
            capy::run_async(ex)(increment_task(counter));
        ioc.poll();
        ioc.restart();
    }
    ioc.run();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

// Compile-time: large event buffer
void
bench_large_event_buffer(bench::state& state)
{
    constexpr corosio::epoll_config cfg{.max_events_per_poll = 512};
    corosio::native_io_context<corosio::epoll_t<cfg>{}> ioc;
    auto ex                  = ioc.get_executor();
    int64_t counter          = 0;
    int constexpr batch_size = 1000;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(state.duration());

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int i = 0; i < batch_size; ++i)
            capy::run_async(ex)(increment_task(counter));
        ioc.poll();
        ioc.restart();
    }
    ioc.run();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

#elif BOOST_COROSIO_HAS_KQUEUE

// Compile-time: recycle_post_nodes disabled
void
bench_no_recycle_ct(bench::state& state)
{
    constexpr corosio::kqueue_config cfg{.recycle_post_nodes = false};
    corosio::native_io_context<corosio::kqueue_t<cfg>{}> ioc;
    auto ex                  = ioc.get_executor();
    int64_t counter          = 0;
    int constexpr batch_size = 1000;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(state.duration());

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int i = 0; i < batch_size; ++i)
            capy::run_async(ex)(increment_task(counter));
        ioc.poll();
        ioc.restart();
    }
    ioc.run();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

// Runtime: recycle_post_nodes disabled via io_context_options
void
bench_no_recycle_rt(bench::state& state)
{
    corosio::io_context ioc(corosio::io_context_options{
        .recycle_post_nodes = false});
    auto ex                  = ioc.get_executor();
    int64_t counter          = 0;
    int constexpr batch_size = 1000;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(state.duration());

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int i = 0; i < batch_size; ++i)
            capy::run_async(ex)(increment_task(counter));
        ioc.poll();
        ioc.restart();
    }
    ioc.run();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

// Compile-time: high inline budget
void
bench_high_inline_budget(bench::state& state)
{
    constexpr corosio::kqueue_config cfg{.inline_budget_max = 64};
    corosio::native_io_context<corosio::kqueue_t<cfg>{}> ioc;
    auto ex                  = ioc.get_executor();
    int64_t counter          = 0;
    int constexpr batch_size = 1000;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(state.duration());

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int i = 0; i < batch_size; ++i)
            capy::run_async(ex)(increment_task(counter));
        ioc.poll();
        ioc.restart();
    }
    ioc.run();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

// Compile-time: large event buffer
void
bench_large_event_buffer(bench::state& state)
{
    constexpr corosio::kqueue_config cfg{.max_events_per_poll = 512};
    corosio::native_io_context<corosio::kqueue_t<cfg>{}> ioc;
    auto ex                  = ioc.get_executor();
    int64_t counter          = 0;
    int constexpr batch_size = 1000;

    perf::stopwatch sw;
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(state.duration());

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int i = 0; i < batch_size; ++i)
            capy::run_async(ex)(increment_task(counter));
        ioc.poll();
        ioc.restart();
    }
    ioc.run();

    state.set_elapsed(sw.elapsed_seconds());
    state.add_items(counter);
}

#endif // BOOST_COROSIO_HAS_EPOLL / BOOST_COROSIO_HAS_KQUEUE

} // anonymous namespace

template<auto Backend>
bench::benchmark_suite
make_io_context_suite()
{
    using F = bench::bench_flags;
    auto suite = bench::benchmark_suite("io_context", F::is_microbenchmark)
        .set_warmup([] {
            corosio::native_io_context<Backend> ioc;
            auto ex         = ioc.get_executor();
            int64_t counter = 0;
            for (int i = 0; i < 1000; ++i)
                capy::run_async(ex)(increment_task(counter));
            ioc.run();
        })
        .add("single_threaded", bench_single_threaded_post<Backend>)
        .add("multithreaded", bench_multithreaded_scaling<Backend>)
            .args({8})
        .add("interleaved", bench_interleaved_post_run<Backend>)
        .add("concurrent", bench_concurrent_post_run<Backend>)
            .args({4});

#if BOOST_COROSIO_HAS_EPOLL || BOOST_COROSIO_HAS_KQUEUE
    suite
        .add("no_recycle_ct", bench_no_recycle_ct)
        .add("no_recycle_rt", bench_no_recycle_rt)
        .add("high_inline_budget", bench_high_inline_budget)
        .add("large_event_buffer", bench_large_event_buffer);
#endif

    return suite;
}

} // namespace corosio_bench

COROSIO_SUITE_INSTANTIATE(corosio_bench::make_io_context_suite)
