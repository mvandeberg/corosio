//
// client.cpp
// ~~~~~~~~~~
//
// Copyright (c) 2003-2008 Christopher M. Kohlhoff (chris at kohlhoff dot com)
// Modifications copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Imported from Christopher M. Kohlhoff's classic asio throughput benchmark
// (the "asio_from_chenshuo" variant chenshuo packaged in his recipes), via
// the evpp benchmark tree. Modernized for Boost.Asio >= 1.66:
//   - asio::io_service                  -> asio::io_context
//   - asio::io_service::strand          -> asio::strand<io_context::executor_type>
//   - strand.wrap(h)                    -> asio::bind_executor(strand, h)
//   - strand.post(h)                    -> asio::post(strand, h)
//   - asio::deadline_timer              -> asio::steady_timer
//   - boost::posix_time::seconds        -> std::chrono::seconds
//   - asio::ip::address::from_string    -> asio::ip::make_address
//   - asio::ip::tcp::resolver iterator  -> single endpoint constructed from
//                                          IP literal (callers pass an IP)
//   - asio::detail::mutex (private)     -> std::mutex
//   - asio::thread (boost::thread)      -> std::thread
// The handler_allocator hooks were dropped: modern asio routes handler
// allocation through asio::associated_allocator, so the pre-1.66
// asio_handler_allocate ADL hooks are no-ops. Behaviour is unchanged.
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>
#include <boost/mem_fn.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <list>
#include <mutex>
#include <thread>

namespace asio = boost::asio;
using boost::system::error_code;

class stats
{
public:
  stats(int timeout)
    : mutex_(),
      total_bytes_written_(0),
      total_bytes_read_(0),
      timeout_(timeout)
  {
  }

  void add(size_t bytes_written, size_t bytes_read)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    total_bytes_written_ += bytes_written;
    total_bytes_read_ += bytes_read;
  }

  void print()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::cout << total_bytes_written_ << " total bytes written\n";
    std::cout << total_bytes_read_ << " total bytes read\n";
    std::cout << static_cast<double>(total_bytes_read_) / (timeout_ * 1024 * 1024) << " MiB/s throughput\n";
  }

private:
  std::mutex mutex_;
  size_t total_bytes_written_;
  size_t total_bytes_read_;
  int timeout_;
};

class session
{
public:
  session(asio::io_context& ioc, size_t block_size, stats& s)
    : strand_(ioc.get_executor()),
      socket_(ioc),
      block_size_(block_size),
      read_data_(new char[block_size]),
      read_data_length_(0),
      write_data_(new char[block_size]),
      unwritten_count_(0),
      bytes_written_(0),
      bytes_read_(0),
      stats_(s)
  {
    for (size_t i = 0; i < block_size_; ++i)
      write_data_[i] = static_cast<char>(i % 128);
  }

  ~session()
  {
    stats_.add(bytes_written_, bytes_read_);

    delete[] read_data_;
    delete[] write_data_;
  }

  void start(asio::ip::tcp::endpoint const& endpoint)
  {
    socket_.async_connect(endpoint,
        asio::bind_executor(strand_,
          boost::bind(&session::handle_connect, this,
              asio::placeholders::error)));
  }

  void stop()
  {
    asio::post(strand_, boost::bind(&session::close_socket, this));
  }

private:
  void handle_connect(const error_code& err)
  {
    if (!err)
    {
      error_code set_option_err;
      asio::ip::tcp::no_delay no_delay(true);
      socket_.set_option(no_delay, set_option_err);
      if (!set_option_err)
      {
        ++unwritten_count_;
        async_write(socket_, asio::buffer(write_data_, block_size_),
            asio::bind_executor(strand_,
              boost::bind(&session::handle_write, this,
                asio::placeholders::error,
                asio::placeholders::bytes_transferred)));
        socket_.async_read_some(asio::buffer(read_data_, block_size_),
            asio::bind_executor(strand_,
              boost::bind(&session::handle_read, this,
                asio::placeholders::error,
                asio::placeholders::bytes_transferred)));
      }
    }
  }

  void handle_read(const error_code& err, size_t length)
  {
    if (!err)
    {
      bytes_read_ += length;

      read_data_length_ = length;
      ++unwritten_count_;
      if (unwritten_count_ == 1)
      {
        std::swap(read_data_, write_data_);
        async_write(socket_, asio::buffer(write_data_, read_data_length_),
            asio::bind_executor(strand_,
              boost::bind(&session::handle_write, this,
                asio::placeholders::error,
                asio::placeholders::bytes_transferred)));
        socket_.async_read_some(asio::buffer(read_data_, block_size_),
            asio::bind_executor(strand_,
              boost::bind(&session::handle_read, this,
                asio::placeholders::error,
                asio::placeholders::bytes_transferred)));
      }
    }
  }

  void handle_write(const error_code& err, size_t length)
  {
    if (!err && length > 0)
    {
      bytes_written_ += length;

      --unwritten_count_;
      if (unwritten_count_ == 1)
      {
        std::swap(read_data_, write_data_);
        async_write(socket_, asio::buffer(write_data_, read_data_length_),
            asio::bind_executor(strand_,
              boost::bind(&session::handle_write, this,
                asio::placeholders::error,
                asio::placeholders::bytes_transferred)));
        socket_.async_read_some(asio::buffer(read_data_, block_size_),
            asio::bind_executor(strand_,
              boost::bind(&session::handle_read, this,
                asio::placeholders::error,
                asio::placeholders::bytes_transferred)));
      }
    }
  }

  void close_socket()
  {
    socket_.close();
  }

private:
  asio::strand<asio::io_context::executor_type> strand_;
  asio::ip::tcp::socket socket_;
  size_t block_size_;
  char* read_data_;
  size_t read_data_length_;
  char* write_data_;
  int unwritten_count_;
  size_t bytes_written_;
  size_t bytes_read_;
  stats& stats_;
};

class client
{
public:
  client(asio::io_context& ioc,
      const asio::ip::tcp::endpoint& endpoint,
      size_t block_size, size_t session_count, int timeout)
    : io_context_(ioc),
      stop_timer_(ioc),
      sessions_(),
      stats_(timeout)
  {
    stop_timer_.expires_after(std::chrono::seconds(timeout));
    stop_timer_.async_wait(boost::bind(&client::handle_timeout, this));

    for (size_t i = 0; i < session_count; ++i)
    {
      session* new_session = new session(io_context_, block_size, stats_);
      new_session->start(endpoint);
      sessions_.push_back(new_session);
    }
  }

  ~client()
  {
    while (!sessions_.empty())
    {
      delete sessions_.front();
      sessions_.pop_front();
    }

    stats_.print();
  }

  void handle_timeout()
  {
    std::for_each(sessions_.begin(), sessions_.end(),
        boost::mem_fn(&session::stop));
  }

private:
  asio::io_context& io_context_;
  asio::steady_timer stop_timer_;
  std::list<session*> sessions_;
  stats stats_;
};

int main(int argc, char* argv[])
{
  try
  {
    if (argc != 7)
    {
      std::cerr << "Usage: client <host> <port> <threads> <blocksize> ";
      std::cerr << "<sessions> <time>\n";
      return 1;
    }

    const char* host = argv[1];
    short port = static_cast<short>(std::atoi(argv[2]));
    int thread_count = std::atoi(argv[3]);
    size_t block_size = std::atoi(argv[4]);
    size_t session_count = std::atoi(argv[5]);
    int timeout = std::atoi(argv[6]);

    asio::io_context ioc;

    asio::ip::tcp::endpoint endpoint(asio::ip::make_address(host), port);
    client c(ioc, endpoint, block_size, session_count, timeout);

    std::list<std::thread*> threads;
    while (--thread_count > 0)
    {
      std::thread* new_thread = new std::thread([&ioc] { ioc.run(); });
      threads.push_back(new_thread);
    }

    ioc.run();

    while (!threads.empty())
    {
      threads.front()->join();
      delete threads.front();
      threads.pop_front();
    }
  }
  catch (std::exception& e)
  {
    std::cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}
