// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#pragma once

// #define LINUX_USE_IO_URING

#include "linux_epoll.h"
// #include "pal_linux_io_uring.h"

namespace verona::rt::io
{
#if defined(__linux__)
#  define PLATFORM_SUPPORTS_IO
  template<typename T>
  using Result = POSIXResult<T>;
#  if defined(LINUX_USE_IO_URING)
  // TODO: io_uring
#  else
  using Event = LinuxEpollEvent;
  using Poller = LinuxEpollPoller;
  using TCP = LinuxTCP;
#  endif
#else
  // unsuported platforms
#endif
}
