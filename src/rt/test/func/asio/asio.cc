#include "../../../verona.h"
#include "../../log.h"

#include <asio.h>

int main()
{
  static constexpr size_t cores = 2;

  auto* alloc = snmalloc::ThreadAlloc::get();
#ifdef USE_SYSTEMATIC_TESTING
  Systematic::enable_logging();
#else
  // UNUSED(seed);
#endif
  auto& sched = verona::rt::Scheduler::get();
  sched.set_fair(true);
  sched.init(cores);

  auto* io_backend = ponyint_asio_backend_init();
  ponyint_asio_init(0);
  // ponyint_asio_start();
  // ponyint_asio_stop();

  ponyint_asio_backend_final(io_backend);

  sched.run();

  return 0;
}
