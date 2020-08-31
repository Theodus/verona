#include "../../../verona.h"
#include "../../log.h"

#include <uv.h>

namespace verona::rt::io
{
  class Request
  {
    template<typename T>
    friend class verona::rt::MPSCQ;
    friend class UVBackend;

    std::atomic<Request*> next = nullptr;
    uv_handle_type handle_type;
    Cown* owner = nullptr;
    uv_any_handle* handle = nullptr;
    uint64_t timeout = 0;
    bool oneshot = false;

    size_t size() const
    {
      return sizeof(Request);
    }

  public:
    static Request* stop(snmalloc::Alloc* alloc)
    {
      auto* req = (Request*)alloc->alloc(sizeof(Request));
      req->handle_type = UV_ASYNC;
      return req;
    }

    static Request* timer_start(
      snmalloc::Alloc* alloc,
      Cown* owner,
      uint64_t timeout,
      bool repeat = false)
    {
      auto* req = (Request*)alloc->alloc(sizeof(Request));
      req->handle_type = UV_TIMER;
      req->owner = owner;
      req->timeout = timeout;
      req->oneshot = !repeat;
      return req;
    }

    static Request* timer_stop(snmalloc::Alloc* alloc, uv_timer_t* handle)
    {
      auto* req = (Request*)alloc->alloc(sizeof(Request));
      req->handle_type = UV_TIMER;
      req->handle = (uv_any_handle*)handle;
      return req;
    }
  };

  class UVBackend
  {
    friend void request(Request*);

    std::thread io_thread;
    uv_loop_t* loop = nullptr;
    verona::rt::MPSCQ<Request> requests;
    uv_async_t request_notify;

    bool running = false;
    std::mutex running_m{};
    std::condition_variable running_cv{};

    static void handle_requests(uv_async_t* handle)
    {
      auto* alloc = snmalloc::ThreadAlloc::get();
      auto* self = (UVBackend*)handle->data;
      auto* last = self->requests.peek_back();
      Request* req;
      do
      {
        req = self->requests.dequeue(alloc);
        if (req == nullptr)
          return;

        switch (req->handle_type)
        {
          case UV_ASYNC:
          {
            uv_stop(self->loop);
            break;
          }
          case UV_TIMER:
          {
            auto* timer = (uv_timer_t*)alloc->alloc(sizeof(uv_timer_t));
            uv_timer_init(self->loop, timer);
            timer->data = req->owner;
            uv_timer_start(
              timer,
              [](auto* h) {
                auto* p = (Cown*)h->data;
                if (p != nullptr)
                {
                  Cown::acquire(p);
                  p->schedule();
                }

                std::cout << "timeout to " << p << std::endl;
              },
              req->timeout,
              (req->oneshot) ? 0 : req->timeout);
            break;
          }
          default:
            std::cout << "unexpected handle type: "
                      << uv_handle_type_name(req->handle_type) << std::endl;
        }
      } while (req != last);
    }

    void notify_running(bool is_running)
    {
      running_m.lock();
      running = is_running;
      running_m.unlock();
      running_cv.notify_all();
    }

    void await_running(bool is_running)
    {
      std::unique_lock<std::mutex> lock(running_m);
      running_cv.wait(lock, [&] { return running == is_running; });
    }

  public:
    void start()
    {
      int err = 0;
      auto* idle =
        (uv_idle_t*)snmalloc::ThreadAlloc::get()->alloc(sizeof(uv_idle_t));
      io_thread = std::thread([&, self = this] {
        auto* alloc = snmalloc::ThreadAlloc::get();
        requests.init((Request*)alloc->alloc(sizeof(Request)));
        loop = (uv_loop_t*)alloc->alloc(sizeof(uv_loop_t));

        err = uv_loop_init(loop);
        if (err < 0)
        {
          std::cerr << "loop init: " << uv_strerror(-err) << std::endl;
          abort();
        }

        err = uv_async_init(loop, &request_notify, handle_requests);
        if (err < 0)
        {
          std::cerr << "notify init: " << uv_strerror(-err) << std::endl;
          abort();
        }

        err = uv_idle_init(loop, idle);
        if (err < 0)
        {
          std::cerr << "idle init: " << uv_strerror(-err) << std::endl;
          abort();
        }
        idle->data = self;
        err = uv_idle_start(idle, [](auto* idle) {
          ((UVBackend*)idle->data)->notify_running(true);
          uv_idle_stop(idle);
          snmalloc::ThreadAlloc::get()->dealloc(idle, sizeof(uv_idle_t));
        });
        if (err < 0)
        {
          std::cerr << "idle start: " << uv_strerror(-err) << std::endl;
          abort();
        }

        request_notify.data = self;
        uv_run(loop, UV_RUN_DEFAULT);

        uv_loop_delete(loop);
        alloc->dealloc(loop, sizeof(*loop));
        auto* stub = requests.destroy();
        alloc->dealloc(stub, sizeof(*stub));
      });

      await_running(true);
    }

    void stop()
    {
      send(Request::stop(ThreadAlloc::get()));
      io_thread.join();
      uv_loop_close(loop);
    }

    void wait()
    {
      await_running(false);
    }

    void send(Request* req)
    {
      requests.enqueue(req);
      uv_async_send(&request_notify);
    }
  };

  static UVBackend& io_thread()
  {
    static UVBackend loop;
    return loop;
  }

  struct Timer : public verona::rt::VCown<Timer>
  {
    uv_timer_t* handle;

    ~Timer()
    {
      std::cout << "timer done" << std::endl;
      io_thread().send(
        Request::timer_stop(snmalloc::ThreadAlloc::get(), handle));
    }

    void notified(verona::rt::Object*)
    {
      mark_io_ready(false);
    }
  };

  Timer* start_timer(size_t timeout, bool repeat)
  {
    auto* alloc = snmalloc::ThreadAlloc::get();
    auto* timer = new Timer;
    timer->mark_io();
    // Deschedule, for promise
    timer->wake();
    Scheduler::add_external_event_source();
    io_thread().send(Request::timer_start(alloc, timer, timeout, repeat));
    return timer;
  }
}

struct TimerNotify : public verona::rt::VBehaviour<TimerNotify>
{
  verona::rt::io::Timer* timer;
  const char* name;
  size_t count;

public:
  TimerNotify(verona::rt::io::Timer* timer_, const char* name_, size_t count_)
  : timer(timer_), name(name_), count(count_)
  {}

  void f()
  {
    std::cout << "timeout: " << name << ", " << count << std::endl;
    if (count > 1)
      verona::rt::Cown::schedule<TimerNotify, verona::rt::YesTransfer>(
        timer, timer, name, count - 1);
    else
      verona::rt::Cown::release(snmalloc::ThreadAlloc::get(), timer);
  }
};

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

  auto& event_loop = verona::rt::io::io_thread();
  event_loop.start();
  std::cerr << "started" << std::endl;

  auto* timer = verona::rt::io::start_timer(600, true);

  verona::rt::Cown::schedule<TimerNotify>(timer, timer, "timer1", (size_t)3);

  sched.run();
  // TODO: shutdown when all schedulerthreads have zero event_sources
  event_loop.wait();
  return 0;
}
