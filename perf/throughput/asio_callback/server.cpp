//
// server.cpp
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
//   - io_service.post(h)                -> asio::post(io_context, h)
//   - asio::ip::address::from_string    -> asio::ip::make_address
//   - asio::thread (boost::thread)      -> std::thread
// The handler_allocator hooks were dropped: modern asio routes handler
// allocation through asio::associated_allocator, so the pre-1.66
// asio_handler_allocate ADL hooks are no-ops. Behaviour is unchanged.
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <list>
#include <thread>

namespace asio = boost::asio;
using boost::system::error_code;

class session
{
public:
  session(asio::io_context& ioc, size_t block_size)
    : io_context_(ioc),
      strand_(ioc.get_executor()),
      socket_(ioc),
      block_size_(block_size),
      read_data_(new char[block_size]),
      read_data_length_(0),
      write_data_(new char[block_size]),
      unsent_count_(0),
      op_count_(0)
  {
  }

  ~session()
  {
    delete[] read_data_;
    delete[] write_data_;
  }

  asio::ip::tcp::socket& socket()
  {
    return socket_;
  }

  void start()
  {
    error_code set_option_err;
    asio::ip::tcp::no_delay no_delay(true);
    socket_.set_option(no_delay, set_option_err);
    if (!set_option_err)
    {
      ++op_count_;
      socket_.async_read_some(asio::buffer(read_data_, block_size_),
          asio::bind_executor(strand_,
            boost::bind(&session::handle_read, this,
              asio::placeholders::error,
              asio::placeholders::bytes_transferred)));
    }
    else
    {
      asio::post(io_context_, boost::bind(&session::destroy, this));
    }
  }

  void handle_read(const error_code& err, size_t length)
  {
    --op_count_;

    if (!err)
    {
      read_data_length_ = length;
      ++unsent_count_;
      if (unsent_count_ == 1)
      {
        op_count_ += 2;
        std::swap(read_data_, write_data_);
        async_write(socket_, asio::buffer(write_data_, read_data_length_),
            asio::bind_executor(strand_,
              boost::bind(&session::handle_write, this,
                asio::placeholders::error)));
        socket_.async_read_some(asio::buffer(read_data_, block_size_),
            asio::bind_executor(strand_,
              boost::bind(&session::handle_read, this,
                asio::placeholders::error,
                asio::placeholders::bytes_transferred)));
      }
    }

    if (op_count_ == 0)
      asio::post(io_context_, boost::bind(&session::destroy, this));
  }

  void handle_write(const error_code& err)
  {
    --op_count_;

    if (!err)
    {
      --unsent_count_;
      if (unsent_count_ == 1)
      {
        op_count_ += 2;
        std::swap(read_data_, write_data_);
        async_write(socket_, asio::buffer(write_data_, read_data_length_),
            asio::bind_executor(strand_,
              boost::bind(&session::handle_write, this,
                asio::placeholders::error)));
        socket_.async_read_some(asio::buffer(read_data_, block_size_),
            asio::bind_executor(strand_,
              boost::bind(&session::handle_read, this,
                asio::placeholders::error,
                asio::placeholders::bytes_transferred)));
      }
    }

    if (op_count_ == 0)
      asio::post(io_context_, boost::bind(&session::destroy, this));
  }

  static void destroy(session* s)
  {
    delete s;
  }

private:
  asio::io_context& io_context_;
  asio::strand<asio::io_context::executor_type> strand_;
  asio::ip::tcp::socket socket_;
  size_t block_size_;
  char* read_data_;
  size_t read_data_length_;
  char* write_data_;
  int unsent_count_;
  int op_count_;
};

class server
{
public:
  server(asio::io_context& ioc, const asio::ip::tcp::endpoint& endpoint,
      size_t block_size)
    : io_context_(ioc),
      acceptor_(ioc),
      block_size_(block_size)
  {
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(1));
    acceptor_.bind(endpoint);
    acceptor_.listen();

    session* new_session = new session(io_context_, block_size_);
    acceptor_.async_accept(new_session->socket(),
        boost::bind(&server::handle_accept, this, new_session,
          asio::placeholders::error));
  }

  void handle_accept(session* new_session, const error_code& err)
  {
    if (!err)
    {
      new_session->start();
      new_session = new session(io_context_, block_size_);
      acceptor_.async_accept(new_session->socket(),
          boost::bind(&server::handle_accept, this, new_session,
            asio::placeholders::error));
    }
    else
    {
      delete new_session;
    }
  }

private:
  asio::io_context& io_context_;
  asio::ip::tcp::acceptor acceptor_;
  size_t block_size_;
};

int main(int argc, char* argv[])
{
  try
  {
    if (argc != 5)
    {
      std::cerr << "Usage: server <address> <port> <threads> <blocksize>\n";
      return 1;
    }

    asio::ip::address address = asio::ip::make_address(argv[1]);
    short port = static_cast<short>(std::atoi(argv[2]));
    int thread_count = std::atoi(argv[3]);
    size_t block_size = std::atoi(argv[4]);

    asio::io_context ioc;

    server s(ioc, asio::ip::tcp::endpoint(address, port), block_size);

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
