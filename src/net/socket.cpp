#include "net/socket.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <unistd.h>
namespace zt {
Fd::~Fd() {
  reset();
}
Fd &Fd::operator=(Fd &&other) noexcept {
  if (this != &other)
    reset(std::exchange(other.fd_, -1));
  return *this;
}
void Fd::reset(int fd) {
  if (fd_ >= 0)
    ::close(fd_);
  fd_ = fd;
}
void check(int result, const char *operation) {
  if (result < 0)
    throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}
Fd listener(const std::string &address, int port, bool reuse) {
  sockaddr_storage storage{};
  bool v6 = address.find(':') != std::string::npos;
  int family = v6 ? AF_INET6 : AF_INET;
  Fd fd(::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  check(fd.get(), "socket");
  int one = 1;
  check(setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)), "reuseaddr");
  if (reuse)
    check(setsockopt(fd.get(), SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)), "reuseport");
  if (v6) {
    auto *a = reinterpret_cast<sockaddr_in6 *>(&storage);
    a->sin6_family = AF_INET6;
    a->sin6_port = htons(port);
    if (inet_pton(AF_INET6, address.c_str(), &a->sin6_addr) != 1)
      throw std::runtime_error("invalid IPv6 bind address");
    check(setsockopt(fd.get(), IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one)), "v6only");
  } else {
    auto *a = reinterpret_cast<sockaddr_in *>(&storage);
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    if (inet_pton(AF_INET, address.c_str(), &a->sin_addr) != 1)
      throw std::runtime_error("invalid IPv4 bind address");
  }
  check(bind(fd.get(), reinterpret_cast<sockaddr *>(&storage),
             v6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in)),
        "bind");
  check(listen(fd.get(), 1024), "listen");
  return fd;
}
std::string peer_ip(const sockaddr_storage &address) {
  char buffer[INET6_ADDRSTRLEN]{};
  const void *data =
      address.ss_family == AF_INET
          ? static_cast<const void *>(&reinterpret_cast<const sockaddr_in *>(&address)->sin_addr)
          : static_cast<const void *>(&reinterpret_cast<const sockaddr_in6 *>(&address)->sin6_addr);
  if (!inet_ntop(address.ss_family, data, buffer, sizeof(buffer)))
    return "unknown";
  return buffer;
}
} // namespace zt
