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
  class LinuxEpollPoller;

  class LinuxEpollEvent
  {
    epoll_event _ev;
    LinuxEpollPoller* _poller;

  public:
    LinuxEpollEvent(LinuxEpollPoller* poller, int fd) : _poller(poller)
    {
      memset(&_ev, 0, sizeof(_ev));
      _ev.data.fd = fd;
    }

    LinuxEpollPoller* poller()
    {
      return _poller;
    }

    Cown*& cown()
    {
      return (Cown*&)_ev.data.ptr;
    }

    int& fd()
    {
      return _ev.data.fd;
    }

    uint32_t& flags()
    {
      return _ev.events;
    }

    epoll_event& epoll_event()
    {
      return _ev;
    }

    void subscribe(Alloc* alloc, Cown* cown);
    void resubscribe(Alloc* alloc, Cown* cown);
    void unsubscribe(Alloc* alloc);
  };

  class LinuxEpollPoller
  {
  public:
    class Msg
    {
      friend MPSCQ<Msg>;
      friend LinuxEpollPoller;
      friend LinuxEpollEvent;

      enum class Kind : uint8_t
      {
        Sub,
        Resub,
        Unsub
      };

      std::atomic<Msg*> next{nullptr};
      LinuxEpollEvent event;
      Kind kind;

      Msg(LinuxEpollEvent event_, Kind kind_) : event(event_), kind(kind_) {}

      static Msg* create(Alloc* alloc, LinuxEpollEvent event_, Kind kind_)
      {
        return new (alloc->alloc<sizeof(Msg)>()) Msg(event_, kind_);
      }

      static constexpr size_t size()
      {
        return sizeof(Msg);
      }
    };

  private:
    MPSCQ<Msg> q;
    std::atomic<size_t> event_count = 0;
    int epfd;

  public:
    LinuxEpollPoller()
    {
      auto* alloc = ThreadAlloc::get_noncachable();
      q.init(Msg::create(alloc, LinuxEpollEvent(nullptr, 0), (Msg::Kind)0));
      epfd = epoll_create1(0);
    }

    ~LinuxEpollPoller()
    {
      auto* stub = q.destroy();
      ThreadAlloc::get_noncachable()->dealloc<sizeof(*stub)>(stub);
    }

    void send(Alloc* alloc, LinuxEpollEvent event, Msg::Kind kind)
    {
      q.enqueue(Msg::create(alloc, event, kind));
    }

    void handle_messages(Alloc* alloc)
    {
      while (true)
      {
        Msg* msg = q.dequeue(alloc);
        if (msg == nullptr)
          break;

        auto& event = msg->event;
        switch (msg->kind)
        {
          case Msg::Kind::Sub:
          {
            auto res =
              epoll_ctl(epfd, EPOLL_CTL_ADD, event.fd(), &event.epoll_event());
            if (res != 0)
              Systematic::cout() << "error: epoll_ctl(EPOLL_CTL_ADD) "
                                 << strerror(errno) << std::endl;
          }
          case Msg::Kind::Resub:
          {
            auto res =
              epoll_ctl(epfd, EPOLL_CTL_MOD, event.fd(), &event.epoll_event());
            if (res != 0)
              Systematic::cout() << "error: epoll_ctl(EPOLL_CTL_MOD) "
                                 << strerror(errno) << std::endl;
          }
          case Msg::Kind::Unsub:
          {
            auto res = epoll_ctl(epfd, EPOLL_CTL_DEL, event.fd(), nullptr);
            if (res != 0)
              Systematic::cout() << "error: epoll_ctl(EPOLL_CTL_DEL) "
                                 << strerror(errno) << std::endl;
          }
          default:
            assert(false);
        }
      }
    }
  };

  void LinuxEpollEvent::subscribe(Alloc* alloc, Cown* cown)
  {
    this->cown() = cown;
    this->_poller->send(alloc, *this, LinuxEpollPoller::Msg::Kind::Sub);
  }

  void LinuxEpollEvent::resubscribe(Alloc* alloc, Cown* cown)
  {
    this->cown() = cown;
    this->_poller->send(alloc, *this, LinuxEpollPoller::Msg::Kind::Resub);
  }

  void LinuxEpollEvent::unsubscribe(Alloc* alloc)
  {
    this->_poller->send(alloc, *this, LinuxEpollPoller::Msg::Kind::Unsub);
  }

  class LinuxTCP
  {
  public:
    static POSIXResult<int>
    listen(const char* host, const char* port, size_t backlog = 8192)
    {
      return tcp_socket_listen(host, port, backlog);
    }

  private:
  };
}

#endif
