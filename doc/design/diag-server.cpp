// Diagnostic version of hello-timeout that logs why accept_loop ends.

#include <boost/corosio/cancel.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>
#include <boost/corosio/timer.hpp>
#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/task.hpp>
#include <boost/capy/write.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

namespace corosio = boost::corosio;
namespace capy    = boost::capy;

namespace {

constexpr std::string_view RESPONSE =
    "HTTP/1.1 200 OK\nContent-Length: 12\n\nHello world!";

constexpr auto TIMEOUT = std::chrono::seconds(5);

std::atomic<int> g_session_count{0};
std::atomic<int> g_session_finished{0};

template<class T>
T parse_arg(char const* arg)
{
    T value;
    char const* last = arg + std::strlen(arg);
    if (auto [ptr, ec] = std::from_chars(arg, last, value); ec != std::errc{})
    {
        std::cerr << "Failed to parse '" << arg << "'\n";
        std::exit(1);
    }
    return value;
}

capy::task<>
session(corosio::io_context& ioc, corosio::tcp_socket sock)
{
    ++g_session_count;
    corosio::timer deadline(ioc);
    char data[1024];
    for (;;)
    {
        auto [ec, n] = co_await corosio::cancel_after(
            sock.read_some(capy::mutable_buffer(data, sizeof(data))),
            deadline, TIMEOUT);
        if (ec)
            break;

        auto [wec, wn] = co_await corosio::cancel_after(
            capy::write(sock, capy::const_buffer(RESPONSE.data(), RESPONSE.size())),
            deadline, TIMEOUT);
        if (wec)
            break;
    }
    sock.close();
    ++g_session_finished;
}

capy::task<>
accept_loop(corosio::io_context& ioc, corosio::tcp_acceptor& acc)
{
    for (;;)
    {
        corosio::tcp_socket peer(ioc);
        auto [ec] = co_await acc.accept(peer);
        if (ec)
        {
            std::cerr << "ACCEPT FAILED: " << ec.message() << " (" << ec.value() << ")"
                      << " sessions started=" << g_session_count.load()
                      << " finished=" << g_session_finished.load() << "\n";
            break;
        }

        capy::run_async(ioc.get_executor())(session(ioc, std::move(peer)));
    }
    std::cerr << "accept_loop exiting\n";
}

} // namespace

int
main(int argc, char* argv[])
{
    if (argc != 4)
    {
        std::cerr << "Usage: " << argv[0]
                  << " <HOST-IPV4> <PORT> <NUM-THREADS>\n";
        return 1;
    }

    corosio::ipv4_address host(argv[1]);
    auto port        = parse_arg<std::uint16_t>(argv[2]);
    auto num_threads = parse_arg<unsigned>(argv[3]);
    if (num_threads == 0)
    {
        std::cerr << "NUM-THREADS must be >= 1\n";
        return 1;
    }

    corosio::io_context ioc(num_threads);
    corosio::tcp_acceptor acc(ioc, corosio::endpoint(host, port));

    capy::run_async(ioc.get_executor())(accept_loop(ioc, acc));

    std::vector<std::thread> workers;
    workers.reserve(num_threads - 1);
    for (unsigned i = 1; i < num_threads; ++i)
        workers.emplace_back([&ioc] { ioc.run(); });

    ioc.run();

    for (auto& t : workers)
        t.join();

    std::cerr << "EXITING: started=" << g_session_count.load()
              << " finished=" << g_session_finished.load() << "\n";
    return 0;
}
