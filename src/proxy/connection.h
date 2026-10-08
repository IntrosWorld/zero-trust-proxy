#pragma once
#include "http/request_parser.h"
#include "net/socket.h"
#include "policy/policy.h"
#include "policy/rate_limit.h"
#include "proxy/relay.h"
#include <memory>
namespace zt {
enum class State { headers, resolving, connecting, hello, relay, error };
struct Connection {
  uint64_t id;
  Fd client, upstream;
  std::string client_ip, sni, resolved_ip;
  std::shared_ptr<const Config> config;
  State state = State::headers;
  RequestParser parser;
  Request request;
  std::unique_ptr<BodyFramer> body;
  Buffer up, down;
  std::string hello;
  std::vector<Address> addresses;
  size_t next_address = 0;
  Decision decision;
  int status = 0;
  bool client_eof = false, upstream_eof = false, client_shutdown = false, upstream_shutdown = false,
       dead = false, tunnel_accepted = false;
  uint64_t bytes_up = 0, bytes_down = 0;
  Clock::time_point started = Clock::now(), activity = started, phase = started;
  Connection(uint64_t tag, Fd fd, std::string ip, std::shared_ptr<const Config> conf)
      : id(tag), client(std::move(fd)), client_ip(std::move(ip)), config(std::move(conf)) {}
};
} // namespace zt
