#include "../../../verona.h"
#include "../../log.h"

#include <uv.h>

namespace verona::rt::io
{
  class Request
  {
    template<typename T>
    friend class verona::rt::MPSCQ;
    friend class EventLoop;

    std::atomic<Request*> next = nullptr;
    uv_handle_type handle_type;
    verona::rt::Cown* owner;
    uint64_t timeout = 0;
    bool oneshot = false;

    size_t size() const
    {
      return sizeof(Request);
    }

  public:
    static Request* timer(
      snmalloc::Alloc* alloc,
      verona::rt::Cown* owner,
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
  };

  class EventLoop
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
      auto* self = (EventLoop*)handle->data;
      auto* last = self->requests.peek_back();
      Request* req;
      do
      {
        req = self->requests.dequeue(alloc);
        if (req == nullptr)
          return;

        switch (req->handle_type)
        {
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
                  rt::Cown::acquire(p);
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
    static EventLoop* start()
    {
      auto* event_loop =
        new (snmalloc::ThreadAlloc::get()->alloc(sizeof(EventLoop))) EventLoop;

      int err = 0;
      auto* idle =
        (uv_idle_t*)snmalloc::ThreadAlloc::get()->alloc(sizeof(uv_idle_t));
      std::thread io_thread([&] {
        auto* alloc = snmalloc::ThreadAlloc::get();
        event_loop->requests.init((Request*)alloc->alloc(sizeof(Request)));
        event_loop->loop = (uv_loop_t*)alloc->alloc(sizeof(uv_loop_t));

        err = uv_loop_init(event_loop->loop);
        if (err < 0)
        {
          std::cerr << "loop init: " << uv_strerror(-err) << std::endl;
          event_loop->notify_running(true);
          return;
        }

        err = uv_async_init(
          event_loop->loop, &event_loop->request_notify, handle_requests);
        if (err < 0)
        {
          std::cerr << "notify init: " << uv_strerror(-err) << std::endl;
          event_loop->notify_running(true);
          return;
        }

        err = uv_idle_init(event_loop->loop, idle);
        if (err < 0)
        {
          std::cerr << "idle init: " << uv_strerror(-err) << std::endl;
          event_loop->notify_running(true);
          return;
        }
        idle->data = event_loop;
        err = uv_idle_start(idle, [](auto* idle) {
          ((EventLoop*)idle->data)->notify_running(true);
          uv_idle_stop(idle);
          snmalloc::ThreadAlloc::get()->dealloc(idle, sizeof(uv_idle_t));
        });
        if (err < 0)
        {
          std::cerr << "idle start: " << uv_strerror(-err) << std::endl;
          event_loop->notify_running(true);
          return;
        }

        event_loop->request_notify.data = event_loop;
        uv_run(event_loop->loop, UV_RUN_DEFAULT);

        uv_loop_delete(event_loop->loop);
        alloc->dealloc(event_loop->loop, sizeof(*event_loop->loop));
        auto* stub = event_loop->requests.destroy();
        alloc->dealloc(stub, sizeof(*stub));
      });

      event_loop->await_running(true);
      io_thread.detach();
      event_loop->io_thread = std::move(io_thread);
      if (err != 0)
        event_loop = nullptr;

      return event_loop;
    }

    void wait()
    {
      await_running(false);
    }

    void send(Request* req)
    {
      Scheduler::add_external_event_source();
      requests.enqueue(req);
      uv_async_send(&request_notify);
    }
  };
}

struct TimerNotify : public verona::rt::VCown<TimerNotify>
{};

struct IOEventCB : public verona::rt::VBehaviour<IOEventCB>
{
  std::string name;

public:
  IOEventCB(std::string name_) : name(name_) {}

  void f()
  {
    std::cout << "I/O: " << name << std::endl;
  }
};

int main()
{
  static constexpr size_t cores = 4;

  auto* alloc = snmalloc::ThreadAlloc::get();
#ifdef USE_SYSTEMATIC_TESTING
  Systematic::enable_logging();
#else
  // UNUSED(seed);
#endif
  auto& sched = verona::rt::Scheduler::get();
  sched.set_fair(true);
  sched.init(cores);

  auto* event_loop = verona::rt::io::EventLoop::start();
  if (event_loop == nullptr)
  {
    std::cerr << "failed to start I/O thread" << std::endl;
    return 1;
  }

  auto* timer_notify = new TimerNotify;
  // Deschedule, for promise
  timer_notify->wake();
  event_loop->send(
    verona::rt::io::Request::timer(alloc, timer_notify, 1'000, true));

  verona::rt::Cown::schedule<IOEventCB>(timer_notify, "timer1");

  sched.run();
  // TODO: shutdown when all schedulerthreads have zero event_sources
  event_loop->wait();
  return 0;
}
