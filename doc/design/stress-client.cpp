// Stress client for hello-timeout server.
//
// Opens N concurrent TCP connections, sends a short HTTP-shaped
// request on each, reads the response, and repeats for a number
// of iterations. Designed to reproduce a residual UAF in the
// corosio scheduler queue at >= 1024 concurrent connections.
//
// Build:
//   g++ -std=c++20 -O2 -pthread -o stress-client stress-client.cpp
// Run:
//   ./stress-client 127.0.0.1 18888 1024 50

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr char REQUEST[] =
    "GET / HTTP/1.0\r\nHost: x\r\n\r\n";

std::atomic<int> g_errors{0};
std::atomic<int> g_success{0};

void
session(char const* host, int port, int iters)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        ++g_errors;
        return;
    }
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, host, &sa.sin_addr) != 1)
    {
        ::close(fd);
        ++g_errors;
        return;
    }

    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0)
    {
        ::close(fd);
        ++g_errors;
        return;
    }

    char buf[4096];
    for (int i = 0; i < iters; ++i)
    {
        ssize_t wn = ::send(fd, REQUEST, sizeof(REQUEST) - 1, MSG_NOSIGNAL);
        if (wn != sizeof(REQUEST) - 1)
        {
            ++g_errors;
            break;
        }
        ssize_t rn = ::recv(fd, buf, sizeof(buf), 0);
        if (rn <= 0)
        {
            ++g_errors;
            break;
        }
    }
    ::close(fd);
    ++g_success;
}

} // namespace

int
main(int argc, char** argv)
{
    if (argc != 5)
    {
        std::fprintf(stderr,
            "Usage: %s <HOST-IPV4> <PORT> <CONNECTIONS> <ITERS>\n",
            argv[0]);
        return 1;
    }

    char const* host = argv[1];
    int port         = std::atoi(argv[2]);
    int conns        = std::atoi(argv[3]);
    int iters        = std::atoi(argv[4]);

    std::vector<std::thread> threads;
    threads.reserve(conns);

    auto t0 = std::chrono::steady_clock::now();

    for (int i = 0; i < conns; ++i)
        threads.emplace_back(session, host, port, iters);

    for (auto& t : threads)
        t.join();

    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)
                  .count();

    std::printf("conns=%d iters=%d success=%d errors=%d elapsed=%lldms\n",
        conns, iters, g_success.load(), g_errors.load(),
        static_cast<long long>(ms));

    return 0;
}
