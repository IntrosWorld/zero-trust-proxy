#pragma once
#include "net/socket.h"
#include <cstdint>
#include <span>
#include <sys/epoll.h>
namespace zt {
class EventLoop {
  Fd fd_;

public:
  EventLoop();
  void add(int fd, uint64_t tag, uint32_t events);
  void modify(int fd, uint64_t tag, uint32_t events);
  void remove(int fd);
  int wait(std::span<epoll_event> events, int timeout = -1);
};
Fd make_timer();
Fd make_wakeup();
} // namespace zt
