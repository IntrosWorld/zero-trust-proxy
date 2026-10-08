#pragma once
#include <string>
#include <sys/socket.h>
#include <utility>
namespace zt {
class Fd {
  int fd_ = -1;

public:
  explicit Fd(int fd = -1) : fd_(fd) {}
  ~Fd();
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  Fd(Fd &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  Fd &operator=(Fd &&other) noexcept;
  int get() const {
    return fd_;
  }
  explicit operator bool() const {
    return fd_ >= 0;
  }
  void reset(int fd = -1);
};
Fd listener(const std::string &address, int port, bool reuse = true);
std::string peer_ip(const sockaddr_storage &address);
void check(int result, const char *operation);
} // namespace zt
