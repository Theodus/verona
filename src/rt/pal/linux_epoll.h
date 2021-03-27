// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once
#if defined(__linux__) && !defined(LINUX_USE_IO_URING)

#  include "../ds/mpscq.h"
#  include "../test/systematic.h"
#  include "posix.h"
#  include "snmalloc.h"

#  include <sys/epoll.h>

namespace verona::rt::io
{
  using namespace snmalloc;

  class Cown;

  class LinuxEpollEvent
  {
    friend class LinuxEpollPoller;
    friend class LinuxTCP;

    struct epoll_event ev;

    LinuxEpollEvent(int fd, Cown* cown, uint32_t flags)
    {
      memset(&ev, 0, sizeof(ev));
      ev.data.fd = fd;
      ev.data.ptr = cown;
      ev.events = flags;
    }

    Cown*& cown()
    {
      return (Cown*&)ev.data.ptr;
    }

    int& fd()
    {
      return ev.data.fd;
    }
  };

  class LinuxEpollPoller
  {
    class Msg
    {
      friend MPSCQ<Msg>;
      friend LinuxEpollPoller;

      std::atomic<Msg*> next{nullptr};
      MPSCQ<Msg>* destination;
      LinuxEpollEvent event;

      Msg(MPSCQ<Msg>* destination_, LinuxEpollEvent event_)
      : destination(destination_), event(event_)
      {}

      static Msg*
      create(Alloc* alloc, MPSCQ<Msg>* destination_, LinuxEpollEvent event_)
      {
        return new (alloc->alloc<sizeof(Msg)>()) Msg(destination_, event_);
      }

      static constexpr size_t size()
      {
        return sizeof(Msg);
      }
    };

    MPSCQ<Msg> q;
    std::atomic<size_t> event_count = 0;
    int epfd;

  public:
    LinuxEpollPoller()
    {
      auto* alloc = ThreadAlloc::get_noncachable();
      q.init(Msg::create(alloc, nullptr, LinuxEpollEvent(0, nullptr, 0)));
      epfd = epoll_create1(0);
    }

    ~LinuxEpollPoller()
    {
      auto* stub = q.destroy();
      ThreadAlloc::get_noncachable()->dealloc<sizeof(*stub)>(stub);
    }

    void subscribe(LinuxEpollEvent& event)
    {
      assert(event.cown() != nullptr);
      auto res = epoll_ctl(epfd, EPOLL_CTL_ADD, event.fd(), &event.ev);
      if (res != 0)
      {
        Systematic::cout() << "error: epoll_ctl(EPOLL_CTL_ADD) "
                           << strerror(errno) << " (cown " << event.cown()
                           << ")" << std::endl;
      }
    }

    void resubscribe(LinuxEpollEvent& event)
    {
      assert(event.cown() != nullptr);
      auto res = epoll_ctl(epfd, EPOLL_CTL_MOD, event.fd(), &event.ev);
      if (res != 0)
      {
        Systematic::cout() << "error: epoll_ctl(EPOLL_CTL_MOD) "
                           << strerror(errno) << " (cown " << event.cown()
                           << ")" << std::endl;
      }
    }

    void unsubscribe(LinuxEpollEvent& event)
    {
      auto res = epoll_ctl(epfd, EPOLL_CTL_DEL, event.fd(), nullptr);
      if (res != 0)
      {
        Systematic::cout() << "error: epoll_ctl(EPOLL_CTL_DEL) "
                           << strerror(errno) << " (cown " << event.cown()
                           << ")" << std::endl;
      }
    }
  };

  class LinuxTCP
  {
  public:
    static POSIXResult<LinuxEpollEvent>
    listen(const char* host, const char* port, size_t backlog = 8192)
    {
      auto res = tcp_socket_listen(host, port, backlog);
      if (!res.ok())
        return res.forward_err<LinuxEpollEvent>();

      return LinuxEpollEvent(*res, nullptr, 0);
    }

  private:
  };
}

#endif
