#include "proxy/worker.h"
#include "control/sha256.h"
#include "tls/sni.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sstream>
#include <unistd.h>
namespace zt {
namespace {
constexpr uint64_t LISTENER = 1, TIMER = 2, WAKEUP = 3;
// EPOLLIN also signals readable EOF. Do not subscribe to persistent RDHUP
// while a half-closed socket is armed only for a blocked write.
constexpr uint32_t EDGE = EPOLLET | EPOLLONESHOT;
double elapsed(Clock::time_point from) {
  return std::chrono::duration<double>(Clock::now() - from).count();
}
void drain_counter(int fd) {
  uint64_t n;
  while (read(fd, &n, sizeof(n)) > 0) {
  }
}
} // namespace
Worker::Worker(ConfigStore &configs, Resolver &resolver, Metrics &metrics, AccessLog &log,
               RedisLimiter *redis)
    : configs_(configs), resolver_(resolver), metrics_(metrics), log_(log),
      listener_(listener(configs.load()->listen, configs.load()->port)), timer_(make_timer()),
      mailbox_(std::make_shared<Mailbox>()), redis_(redis) {
  loop_.add(listener_.get(), LISTENER, EPOLLIN);
  loop_.add(timer_.get(), TIMER, EPOLLIN);
  loop_.add(mailbox_->wakeup.get(), WAKEUP, EPOLLIN);
}
Worker::~Worker() {
  stop();
  join();
}
void Worker::start() {
  thread_ = std::thread([this] { run(); });
}
void Worker::stop() {
  stopping_.store(true);
  uint64_t one = 1;
  auto ignored = write(mailbox_->wakeup.get(), &one, sizeof(one));
  (void)ignored;
}
void Worker::join() {
  if (thread_.joinable())
    thread_.join();
}
void Worker::accept_clients() {
  for (int budget = 0; budget < 64; ++budget) {
    sockaddr_storage peer{};
    socklen_t n = sizeof(peer);
    Fd fd(accept4(listener_.get(), reinterpret_cast<sockaddr *>(&peer), &n,
                  SOCK_NONBLOCK | SOCK_CLOEXEC));
    if (!fd) {
      if (errno == EINTR)
        continue;
      break;
    }
    auto cfg = configs_.load();
    auto ip = peer_ip(peer);
    uint64_t id = next_id_++;
    auto c = std::make_unique<Connection>(id, std::move(fd), ip, cfg);
    metrics_.connections.fetch_add(1, std::memory_order_relaxed);
    auto count = per_client_.find(ip);
    if (connections_.size() >= static_cast<size_t>(cfg->max_connections) ||
        (count != per_client_.end() && count->second >= static_cast<size_t>(cfg->max_per_client))) {
      c->decision = {false, 503, "connection-limit", "connection limit reached"};
      c->status = 503;
      record(*c);
      continue;
    }
    int one = 1;
    setsockopt(c->client.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    loop_.add(c->client.get(), id * 2, EDGE | EPOLLIN);
    ++per_client_[ip];
    metrics_.active.fetch_add(1, std::memory_order_relaxed);
    connections_.emplace(id, std::move(c));
  }
}
void Worker::fail(Connection &c, int status, const std::string &rule, const std::string &reason) {
  if (c.dead)
    return;
  c.decision = {false, status, rule, reason};
  c.status = status;
  if (c.upstream) {
    loop_.remove(c.upstream.get());
    c.upstream.reset();
  }
  if (c.tunnel_accepted) {
    c.dead = true;
    return;
  }
  c.state = State::error;
  c.phase = Clock::now();
  c.down.consume(c.down.size());
  auto body = rule + ": " + reason + "\n";
  auto response =
      "HTTP/1.1 " + std::to_string(status) +
      " Blocked\r\nConnection: close\r\nContent-Type: text/plain\r\nX-ZT-Block: " + rule + "\r\n";
  if (status == 407)
    response += "Proxy-Authenticate: Bearer realm=\"zt-proxy\"\r\n";
  if (status == 429)
    response += "Retry-After: 1\r\n";
  response += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
  c.down.append(response);
}
void Worker::request_ready(Connection &c) {
  c.request = std::move(c.parser.request);
  metrics_.method(c.request.method);
  if (!authenticated(*c.config, c.request.authorization)) {
    fail(c, 407, "auth", "proxy authentication required");
    return;
  }
  if (redis_) {
    c.state = State::limiting;
    c.phase = Clock::now();
    auto identity = sha256(c.config->require_auth ? c.request.authorization : c.client_ip);
    if (!redis_->submit(c.id, std::move(identity), c.config, mailbox_))
      rate_result(c, false, false);
    return;
  }
  rate_result(c, false, false);
}
void Worker::rate_result(Connection &c, bool available, bool allowed) {
  if (!available) {
    if (redis_) {
      ++metrics_.redis_fallback;
      metrics_.redis_backend.store(false);
    }
    if (redis_ && c.config->redis_fail_closed) {
      fail(c, 503, "rate-backend", "Redis unavailable");
      return;
    }
    allowed = limiter_.allow(c.client_ip, c.config->rate, c.config->burst);
  }
  if (!allowed) {
    fail(c, 429, "rate-limit", "request rate exceeded");
    return;
  }
  policy_ready(c);
}
void Worker::policy_ready(Connection &c) {
  auto d = precheck(*c.config, c.request);
  if (!d.allow) {
    fail(c, d.status, d.rule, d.reason);
    return;
  }
  if (c.request.tunnel) {
    c.hello = std::move(c.parser.remainder);
    if (c.hello.size() > 16384) {
      fail(c, 400, "malformed-tls", "ClientHello too large");
      return;
    }
  } else {
    c.body = std::make_unique<BodyFramer>(c.request);
    if (!c.body->consume(c.parser.remainder)) {
      fail(c, 400, "request-body", "invalid body or pipelined request");
      return;
    }
    c.up.append(c.request.forward_headers());
    c.up.append(c.parser.remainder);
  }
  c.parser = RequestParser{};
  c.request.authorization.clear();
  c.state = State::resolving;
  c.phase = Clock::now();
  if (!resolver_.submit(c.id, c.request.host, c.request.port, c.config, mailbox_))
    fail(c, 503, "dns-overload", "resolver queue full");
}
void Worker::connect_next(Connection &c) {
  if (c.upstream) {
    loop_.remove(c.upstream.get());
    c.upstream.reset();
  }
  while (c.next_address < c.addresses.size()) {
    auto a = c.addresses[c.next_address++];
    c.resolved_ip = a.ip();
    c.upstream = Fd(socket(a.storage.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!c.upstream)
      continue;
    int one = 1;
    setsockopt(c.upstream.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int r = connect(c.upstream.get(), reinterpret_cast<const sockaddr *>(&a.storage), a.length);
    if (r == 0 || errno == EINPROGRESS) {
      c.state = State::connecting;
      c.phase = Clock::now();
      loop_.add(c.upstream.get(), c.id * 2 + 1, EDGE | EPOLLOUT);
      if (r == 0)
        connected(c);
      return;
    }
    c.upstream.reset();
  }
  fail(c, 502, "connect", "upstream connection failed");
}
void Worker::connected(Connection &c) {
  metrics_.connect_count.fetch_add(1, std::memory_order_relaxed);
  metrics_.connect_us.fetch_add(elapsed(c.phase) * 1e6, std::memory_order_relaxed);
  c.phase = Clock::now();
  c.status = 200;
  if (c.request.tunnel) {
    c.down.append("HTTP/1.1 200 Connection Established\r\n\r\n");
    c.tunnel_accepted = true;
    c.state = State::hello;
    hello_ready(c);
  } else
    c.state = State::relay;
}
void Worker::hello_ready(Connection &c) {
  auto result = extract_sni(c.hello);
  if (result.status == SniStatus::need_more_data) {
    if (c.hello.size() >= 16384)
      fail(c, 403, "malformed-tls", "ClientHello exceeds limit");
    return;
  }
  if (result.status == SniStatus::not_tls) {
    fail(c, 403, "not-tls", "CONNECT requires TLS");
    return;
  }
  if (result.status == SniStatus::malformed) {
    fail(c, 403, "malformed-tls", "invalid ClientHello");
    return;
  }
  c.sni = result.hostname;
  if (result.status == SniStatus::ok && c.sni != c.request.host) {
    fail(c, 403, "sni-mismatch", "SNI differs from CONNECT host");
    return;
  }
  c.up.append(c.hello);
  c.hello.clear();
  c.state = State::relay;
  c.phase = Clock::now();
}
void Worker::read_client(Connection &c) {
  std::array<char, 16384> buffer{};
  size_t budget = 262144;
  while (budget && !c.dead && !c.client_eof) {
    size_t amount = std::min(buffer.size(), budget);
    if (c.state == State::error || c.state == State::limiting || c.state == State::resolving ||
        c.state == State::connecting)
      return;
    if (c.state == State::relay) {
      if (!c.up.empty())
        return;
      if (c.body && c.body->done())
        return;
      amount = std::min(amount, c.up.room());
    }
    if (c.state == State::hello)
      amount = std::min(amount, 16384 - c.hello.size());
    if (!amount)
      return;
    auto n = recv(c.client.get(), buffer.data(), amount, 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        c.dead = true;
      return;
    }
    if (n == 0) {
      c.client_eof = true;
      if (c.state == State::headers || c.state == State::hello) {
        fail(c, 400, "incomplete", "incomplete request");
      } else if (c.body && !c.body->done())
        fail(c, 400, "request-body", "truncated body");
      return;
    }
    budget -= n;
    c.activity = Clock::now();
    std::string_view data(buffer.data(), n);
    if (c.state == State::headers) {
      auto status = c.parser.feed(data);
      if (status == ParseStatus::error) {
        fail(c, c.parser.error_code, "request", "malformed request headers");
        return;
      }
      if (status == ParseStatus::complete) {
        request_ready(c);
        return;
      }
    } else if (c.state == State::hello) {
      c.hello.append(data);
      hello_ready(c);
    } else {
      if (c.body && !c.body->consume(data)) {
        fail(c, 400, "request-body", "invalid body framing");
        return;
      }
      c.up.append(data);
      flush(c, true);
    }
  }
}
void Worker::read_upstream(Connection &c) {
  if (c.state != State::relay || c.upstream_eof || !c.down.empty())
    return;
  std::array<char, 16384> buffer{};
  size_t budget = 262144;
  while (budget && !c.dead && c.down.empty()) {
    auto n = recv(c.upstream.get(), buffer.data(), std::min(buffer.size(), budget), 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        c.dead = true;
      return;
    }
    if (n == 0) {
      c.upstream_eof = true;
      return;
    }
    budget -= n;
    c.activity = Clock::now();
    c.down.append(std::string_view(buffer.data(), n));
    flush(c, false);
  }
}
void Worker::flush(Connection &c, bool upstream) {
  auto &buffer = upstream ? c.up : c.down;
  int fd = upstream ? c.upstream.get() : c.client.get();
  if (fd < 0 || (upstream && c.state != State::relay))
    return;
  size_t budget = 262144;
  while (!buffer.empty() && budget) {
    auto n = send(fd, buffer.data(), std::min(buffer.size(), budget), MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        c.dead = true;
      return;
    }
    if (n == 0)
      return;
    buffer.consume(n);
    budget -= n;
    c.activity = Clock::now();
    if (upstream) {
      c.bytes_up += n;
      metrics_.bytes_up.fetch_add(n, std::memory_order_relaxed);
    } else {
      c.bytes_down += n;
      metrics_.bytes_down.fetch_add(n, std::memory_order_relaxed);
    }
  }
}
void Worker::pump(Connection &c) {
  flush(c, false);
  flush(c, true);
  if (c.dead)
    return;
  read_client(c);
  if (c.dead)
    return;
  flush(c, true);
  read_upstream(c);
  flush(c, false);
  if (c.state == State::error) {
    if (c.down.empty())
      c.dead = true;
    return;
  }
  if (c.state != State::relay)
    return;
  if ((c.client_eof || (c.body && c.body->done())) && c.up.empty() && !c.upstream_shutdown) {
    shutdown(c.upstream.get(), SHUT_WR);
    c.upstream_shutdown = true;
  }
  if (c.upstream_eof && c.down.empty() && !c.client_shutdown) {
    shutdown(c.client.get(), SHUT_WR);
    c.client_shutdown = true;
  }
  if (c.upstream_eof && c.down.empty() && (!c.request.tunnel || (c.client_eof && c.up.empty())))
    c.dead = true;
}
void Worker::arm(Connection &c) {
  if (c.dead)
    return;
  uint32_t client = EDGE;
  if (!c.down.empty())
    client |= EPOLLOUT;
  if (!c.client_eof && (c.state == State::headers || c.state == State::hello ||
                        (c.state == State::relay && c.up.empty() && (!c.body || !c.body->done()))))
    client |= EPOLLIN;
  // Leave inactive one-shot descriptors disarmed until their state changes.
  if (client & (EPOLLIN | EPOLLOUT))
    loop_.modify(c.client.get(), c.id * 2, client);
  if (c.upstream) {
    uint32_t upstream = EDGE;
    if (c.state == State::connecting)
      upstream |= EPOLLOUT;
    else if (c.state == State::relay) {
      if (!c.up.empty())
        upstream |= EPOLLOUT;
      if (!c.upstream_eof && c.down.empty())
        upstream |= EPOLLIN;
    }
    if (upstream & (EPOLLIN | EPOLLOUT))
      loop_.modify(c.upstream.get(), c.id * 2 + 1, upstream);
  }
}
void Worker::dns_results() {
  drain_counter(mailbox_->wakeup.get());
  std::deque<DnsResult> results;
  std::deque<RateResult> rates;
  {
    std::lock_guard lock(mailbox_->mutex);
    results.swap(mailbox_->results);
    rates.swap(mailbox_->rate_results);
  }
  for (auto &r : rates) {
    auto it = connections_.find(r.id);
    if (it == connections_.end() || it->second->state != State::limiting)
      continue;
    auto &c = *it->second;
    rate_result(c, r.available, r.allowed);
    pump(c);
    arm(c);
  }
  for (auto &r : results) {
    auto it = connections_.find(r.id);
    if (it == connections_.end() || it->second->state != State::resolving)
      continue;
    auto &c = *it->second;
    metrics_.dns_count.fetch_add(1, std::memory_order_relaxed);
    metrics_.dns_us.fetch_add(r.seconds * 1e6, std::memory_order_relaxed);
    auto d = address_check(*c.config, c.request, r.addresses);
    if (!d.allow)
      fail(c, d.status, d.rule, d.reason);
    else {
      c.addresses = std::move(r.addresses);
      connect_next(c);
    }
    pump(c);
    arm(c);
  }
}
void Worker::tick() {
  drain_counter(timer_.get());
  auto now = Clock::now();
  limiter_.prune(now);
  for (auto &[id, p] : connections_) {
    (void)id;
    auto &c = *p;
    auto phase = std::chrono::duration_cast<std::chrono::milliseconds>(now - c.phase).count();
    auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(now - c.activity).count();
    auto life = std::chrono::duration_cast<std::chrono::milliseconds>(now - c.started).count();
    if (c.state == State::error && phase > 1000)
      c.dead = true;
    else if ((c.state == State::headers || c.state == State::hello) &&
             phase > c.config->header_timeout_ms)
      fail(c, 408, "header-timeout", "header or ClientHello deadline exceeded");
    else if (c.state == State::limiting && phase > c.config->redis_timeout_ms * 4) {
      rate_result(c, false, false);
      pump(c);
      arm(c);
    } else if (c.state == State::resolving && phase > c.config->dns_timeout_ms)
      fail(c, 504, "dns-timeout", "resolution deadline exceeded");
    else if (c.state == State::connecting && phase > c.config->connect_timeout_ms)
      fail(c, 504, "connect-timeout", "connect deadline exceeded");
    else if (idle > c.config->idle_timeout_ms || life > c.config->tunnel_lifetime_ms)
      fail(c, 408, "timeout", "connection deadline exceeded");
    if (!c.dead && c.state == State::error) {
      flush(c, false);
      if (c.down.empty())
        c.dead = true;
      arm(c);
    }
  }
}
void Worker::record(Connection &c) {
  auto duration = elapsed(c.started);
  metrics_.decision(c.decision.rule);
  metrics_.observe(duration);
  auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
  std::ostringstream out;
  out << "{\"timestamp\":" << stamp << ",\"client_ip\":" << json_string(c.client_ip)
      << ",\"method\":" << json_string(c.request.method)
      << ",\"host\":" << json_string(c.request.host) << ",\"port\":" << c.request.port
      << ",\"sni\":" << json_string(c.sni) << ",\"resolved_ip\":" << json_string(c.resolved_ip)
      << ",\"decision\":" << json_string(c.decision.allow ? "allow" : "block")
      << ",\"rule_id\":" << json_string(c.decision.rule)
      << ",\"reason\":" << json_string(c.decision.reason) << ",\"status\":" << c.status
      << ",\"bytes_up\":" << c.bytes_up << ",\"bytes_down\":" << c.bytes_down
      << ",\"duration_ms\":" << duration * 1000 << '}';
  log_.push(out.str());
}
void Worker::reap() {
  for (auto it = connections_.begin(); it != connections_.end();) {
    auto &c = *it->second;
    if (!c.dead) {
      ++it;
      continue;
    }
    loop_.remove(c.client.get());
    if (c.upstream)
      loop_.remove(c.upstream.get());
    record(c);
    metrics_.active.fetch_sub(1, std::memory_order_relaxed);
    auto count = per_client_.find(c.client_ip);
    if (count != per_client_.end() && --count->second == 0)
      per_client_.erase(count);
    it = connections_.erase(it);
  }
}
void Worker::run() {
  try {
    std::array<epoll_event, 128> events{};
    for (;;) {
      if (stopping_.load() && listener_) {
        loop_.remove(listener_.get());
        listener_.reset();
        stop_time_ = Clock::now();
      }
      if (stopping_.load() &&
          (connections_.empty() || elapsed(stop_time_) * 1000 > configs_.load()->drain_timeout_ms))
        break;
      int n = loop_.wait(events, 100);
      for (int i = 0; i < n; ++i) {
        auto tag = events[i].data.u64;
        auto ev = events[i].events;
        if (tag == LISTENER) {
          if (listener_ && !stopping_.load())
            accept_clients();
          continue;
        }
        if (tag == TIMER) {
          tick();
          continue;
        }
        if (tag == WAKEUP) {
          dns_results();
          continue;
        }
        auto it = connections_.find(tag / 2);
        if (it == connections_.end())
          continue;
        auto &c = *it->second;
        if (c.dead)
          continue;
        if ((tag & 1) && c.state == State::connecting) {
          int error = 0;
          socklen_t len = sizeof(error);
          if (getsockopt(c.upstream.get(), SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error)
            connect_next(c);
          else
            connected(c);
        } else if (ev & EPOLLERR) {
          c.dead = true;
          c.decision = {false, 502, "io", "socket error"};
        }
        pump(c);
        arm(c);
      }
      reap();
    }
    for (auto &[id, c] : connections_) {
      (void)id;
      c->dead = true;
      c->decision = {false, 503, "shutdown", "drain deadline reached"};
    }
    reap();
  } catch (const std::exception &e) {
    std::cerr << "worker fatal: " << e.what() << '\n';
    std::terminate();
  }
}
} // namespace zt
