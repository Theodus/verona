
// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#include <test/harness.h>
#include <test/opt.h>
#include <verona.h>

#ifdef PLATFORM_SUPPORTS_IO
using namespace verona::rt;

struct Main : public VCown<Main>
{};

struct Init : public VBehaviour<Init>
{
  uint16_t port;

  Init(uint16_t port_) : port(port_) {}

  void f()
  {
    auto* alloc = ThreadAlloc::get_noncachable();
    auto res = io::TCP::listen("", std::to_string(port).c_str());
    if (!res.ok())
    {
      std::cout << "Unable to listen: " << res.message() << std::endl;
      return;
    }
    std::cout << "ok" << std::endl;
    // auto* listener = *res;
    // Cown::schedule<Listen, YesTransfer>(listener, listener, port);
  }
};

void test(uint16_t port, bool increment_port)
{
  static uint16_t port_inc = 0;
  if (increment_port)
  {
    port_inc++;
    port += port_inc;
  }

  std::cout << "port: " << port << std::endl;
  auto* alloc = ThreadAlloc::get();
  auto* entrypoint = new (alloc) Main();
  Cown::schedule<Init, YesTransfer>(entrypoint, port);
}

int main(int argc, char** argv)
{
#  ifndef USE_SYSTEMATIC_TESTING
  std::cout << "This test requires systematic testing" << std::endl;
  return 1;
#  endif

  SystematicTestHarness h(argc, argv);
  const auto port = h.opt.is<uint16_t>("--port", 8080);
  const auto increment_port = h.opt.has("--increment_port");
  h.run(test, port, increment_port);
}
#else
int main()
{
  std::cout << "platform does not support IO" << std::endl;
  return 0;
}
#endif
