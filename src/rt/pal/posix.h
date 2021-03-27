// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

#if defined(__unix__)

#  include <cassert>
#  include <cstdio>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <optional>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <type_traits>
#  include <unistd.h>

namespace verona::rt::io
{
  static inline bool error_would_block(int err)
  {
    return (err == EWOULDBLOCK) || (err == EAGAIN);
  }

  template<typename T>
  class POSIXResult
  {
    std::optional<T> value = {};
    int err = 0;

    POSIXResult(T v, int e) : value(v), err(e) {}

  public:
    template<
      typename U = T,
      typename = std::enable_if_t<!std::is_same_v<U, int>>>
    POSIXResult(U v) : value(v)
    {}

    template<
      typename U = T,
      typename = std::enable_if_t<!std::is_same_v<U, int>>>
    POSIXResult(int e) : err(e)
    {}

    static POSIXResult<T> ok(T v)
    {
      return POSIXResult<T>(v, 0);
    }

    static POSIXResult<T> error(int e)
    {
      return POSIXResult<T>({}, e);
    }

    T operator*()
    {
      return *value;
    }

    bool ok() const
    {
      return value.has_value();
    }

    const char* message() const
    {
      return strerror(err);
    }

    bool would_block() const
    {
      return error_would_block(err);
    }

    template<typename U>
    POSIXResult<U> forward_err()
    {
      if (!ok())
        abort();

      return POSIXResult<U>(err);
    }
  };

  static inline POSIXResult<bool> fd_close(int fd)
  {
    auto res = close(fd);
    if (res == -1)
      return errno;

    return true;
  }

  static inline void fd_set_nonblocking(int fd)
  {
    int flags;
    flags = fcntl(fd, F_GETFL, 0);
    assert(flags >= 0);
    flags = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    assert(flags >= 0);
  }

  static inline struct addrinfo*
  tcp_address_info(const char* host, const char* port)
  {
    // TODO: map any to loopback
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_ADDRCONFIG;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if ((host != nullptr) && (host[0] == '\0'))
      host = nullptr;

    struct addrinfo* info;
    int res = getaddrinfo(host, port, &hints, &info);
    if (res != 0)
      return nullptr;

    return info;
  }

  static inline int tcp_open(struct addrinfo* info = nullptr)
  {
    int domain = AF_INET;
    int type = SOCK_STREAM;
    int protocol = 0;
    if (info != nullptr)
    {
      domain = info->ai_family;
      type = info->ai_socktype;
      protocol = info->ai_protocol;
    }
    int sock = socket(domain, type | SOCK_NONBLOCK, protocol);
    if (sock == -1)
      return -1;

    int opt_val = 1;
    int res =
      setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val));
    if (res == -1)
      return -1;

    return sock;
  }

  static inline POSIXResult<int> tcp_connect(const char* host, const char* port)
  {
    auto* info = tcp_address_info(host, port);
    if (info == nullptr)
      return POSIXResult<int>::error(errno);

    // TODO: Happy Eyeballs
    int sock = -1;
    for (auto* p = info; p != nullptr; p = p->ai_next)
    {
      sock = tcp_open(p);
      if (sock == -1)
        continue;

      auto res = ::connect(sock, p->ai_addr, p->ai_addrlen);
      if ((res == 0) || (errno == EINPROGRESS))
        break;

      res = ::close(sock);
      assert(res == 0);

      sock = -1;
    }

    freeaddrinfo(info);
    return POSIXResult<int>::ok(sock);
  }

  static inline POSIXResult<int>
  tcp_socket_listen(const char* host, const char* port, size_t backlog)
  {
    auto* info = tcp_address_info(host, port);
    if (info == nullptr)
      return POSIXResult<int>::error(errno);

    int sock = -1;
    struct addrinfo* addr = info;
    for (; addr != nullptr; addr = addr->ai_next)
    {
      sock = tcp_open(addr);
      if (sock != -1)
        break;
    }
    if (sock == -1)
      return POSIXResult<int>::error(errno);

    int res = bind(sock, addr->ai_addr, addr->ai_addrlen);
    if (res == -1)
      return POSIXResult<int>::error(errno);

    res = ::listen(sock, backlog);
    if (res == -1)
      return POSIXResult<int>::error(errno);

    freeaddrinfo(info);
    return POSIXResult<int>::ok(sock);
  }

  static inline POSIXResult<int> tcp_accept(int listener_fd)
  {
    int sock = accept(listener_fd, nullptr, nullptr);
    if (sock == -1)
      return POSIXResult<int>::error(errno);

    fd_set_nonblocking(sock);
    return POSIXResult<int>::ok(sock);
  }

  static inline POSIXResult<size_t> tcp_read(int sock, char* buf, size_t len)
  {
    auto res = recv(sock, buf, len, 0);
    if (res == -1)
      return errno;

    return (size_t)res;
  }

  static POSIXResult<size_t> write(int sock, const char* buf, size_t len)
  {
    auto res = send(sock, buf, len, MSG_NOSIGNAL);
    if (res == -1)
      return errno;

    return (size_t)res;
  }
}

#endif
