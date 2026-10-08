#include "net/event_loop.h"
#include <cerrno>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
namespace zt {
EventLoop::EventLoop() : fd_(epoll_create1(EPOLL_CLOEXEC)) {
  check(fd_.get(), "epoll_create");
}
void EventLoop::add(int fd, uint64_t tag, uint32_t events) {
  epoll_event ev{};
  ev.events = events;
  ev.data.u64 = tag;
  check(epoll_ctl(fd_.get(), EPOLL_CTL_ADD, fd, &ev), "epoll_add");
}
void EventLoop::modify(int fd, uint64_t tag, uint32_t events) {
  epoll_event ev{};
  ev.events = events;
  ev.data.u64 = tag;
  check(epoll_ctl(fd_.get(), EPOLL_CTL_MOD, fd, &ev), "epoll_mod");
}
void EventLoop::remove(int fd) {
  epoll_ctl(fd_.get(), EPOLL_CTL_DEL, fd, nullptr);
}
int EventLoop::wait(std::span<epoll_event> events, int timeout) {
  int n = epoll_wait(fd_.get(), events.data(), events.size(), timeout);
  if (n < 0 && errno == EINTR)
    return 0;
  check(n, "epoll_wait");
  return n;
}
Fd make_timer() {
  Fd fd(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
  check(fd.get(), "timerfd");
  itimerspec t{};
  t.it_value.tv_nsec = 100000000;
  t.it_interval = t.it_value;
  check(timerfd_settime(fd.get(), 0, &t, nullptr), "timerfd_settime");
  return fd;
}
Fd make_wakeup() {
  Fd fd(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
  check(fd.get(), "eventfd");
  return fd;
}
} // namespace zt
