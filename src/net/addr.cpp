#include "net/addr.h"
#include "net/socket.h"
#include <arpa/inet.h>
#include <cstring>
#include <stdexcept>
namespace zt {
Address Address::parse(const std::string &ip, int port) {
  Address a;
  auto *v4 = reinterpret_cast<sockaddr_in *>(&a.storage);
  if (inet_pton(AF_INET, ip.c_str(), &v4->sin_addr) == 1) {
    v4->sin_family = AF_INET;
    v4->sin_port = htons(port);
    a.length = sizeof(*v4);
    return a;
  }
  auto *v6 = reinterpret_cast<sockaddr_in6 *>(&a.storage);
  if (inet_pton(AF_INET6, ip.c_str(), &v6->sin6_addr) != 1)
    throw std::runtime_error("invalid IP: " + ip);
  v6->sin6_family = AF_INET6;
  v6->sin6_port = htons(port);
  a.length = sizeof(*v6);
  return a;
}
bool ip_literal(const std::string &host) {
  in6_addr a{};
  return inet_pton(AF_INET, host.c_str(), &a) == 1 || inet_pton(AF_INET6, host.c_str(), &a) == 1;
}
std::string Address::ip() const {
  return peer_ip(storage);
}
static bool public4(uint32_t n) {
  struct Range {
    uint32_t network, mask;
  };
  constexpr Range blocked[] = {
      {0x00000000, 0xff000000}, {0x0a000000, 0xff000000}, {0x64400000, 0xffc00000},
      {0x7f000000, 0xff000000}, {0xa9fe0000, 0xffff0000}, {0xac100000, 0xfff00000},
      {0xc0000000, 0xffffff00}, {0xc0000200, 0xffffff00}, {0xc0586300, 0xffffff00},
      {0xc0a80000, 0xffff0000}, {0xc6120000, 0xfffe0000}, {0xc6336400, 0xffffff00},
      {0xcb007100, 0xffffff00}, {0xe0000000, 0xf0000000}, {0xf0000000, 0xf0000000}};
  for (auto r : blocked)
    if ((n & r.mask) == r.network)
      return false;
  return true;
}
bool Address::public_ip() const {
  if (storage.ss_family == AF_INET)
    return public4(ntohl(reinterpret_cast<const sockaddr_in *>(&storage)->sin_addr.s_addr));
  const auto &addr = reinterpret_cast<const sockaddr_in6 *>(&storage)->sin6_addr;
  auto *b = addr.s6_addr;
  if (IN6_IS_ADDR_V4MAPPED(&addr)) {
    uint32_t n;
    std::memcpy(&n, b + 12, 4);
    return public4(ntohl(n));
  }
  // Only global unicast 2000::/3; reject transition, documentation and special-use prefixes.
  if ((b[0] & 0xe0) != 0x20)
    return false;
  if (b[0] == 0x20 && b[1] == 0x02)
    return false; // 6to4 can embed private v4
  if (b[0] == 0x20 && b[1] == 0x01 && b[2] < 2)
    return false; // IETF protocol assignments /23
  if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8)
    return false;
  if (b[0] == 0x3f && (b[1] & 0xf0) == 0xf0)
    return false; // 3fff::/20 documentation
  return true;
}
bool Address::loopback() const {
  if (storage.ss_family == AF_INET)
    return (ntohl(reinterpret_cast<const sockaddr_in *>(&storage)->sin_addr.s_addr) >> 24) == 127;
  const auto &a = reinterpret_cast<const sockaddr_in6 *>(&storage)->sin6_addr;
  return IN6_IS_ADDR_LOOPBACK(&a);
}
} // namespace zt
