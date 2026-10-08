#pragma once
#include <string>
#include <sys/socket.h>
namespace zt {
struct Address {
  sockaddr_storage storage{};
  socklen_t length = 0;
  std::string ip() const;
  bool public_ip() const;
  bool loopback() const;
  static Address parse(const std::string &ip, int port);
};
bool ip_literal(const std::string &host);
} // namespace zt
