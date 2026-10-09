#include "admin/server.h"
#include "net/event_loop.h"
#include <array>
#include <cerrno>
#include <chrono>
#include <unordered_map>
namespace zt {
AdminServer::AdminServer(ConfigStore &configs, Metrics &metrics)
    : metrics_(metrics), configs_(configs),
      listener_(listener(configs.load()->admin, configs.load()->admin_port, false)),
      thread_([this] { run(); }) {}
AdminServer::~AdminServer() {
  stop_.store(true);
  thread_.join();
}
void AdminServer::run() {
  EventLoop loop;
  loop.add(listener_.get(), 0, EPOLLIN);
  struct Peer {
    Fd fd;
    std::string input, output;
    size_t sent = 0;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  };
  std::unordered_map<uint64_t, Peer> peers;
  uint64_t next = 1;
  std::array<epoll_event, 64> events{};
  while (!stop_.load()) {
    int n = loop.wait(events, 100);
    for (int i = 0; i < n; ++i) {
      auto id = events[i].data.u64;
      if (id == 0) {
        for (int k = 0; k < 32; ++k) {
          Fd c(accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
          if (!c)
            break;
          if (peers.size() >= 128)
            continue;
          loop.add(c.get(), next, EPOLLIN);
          peers.emplace(next++, Peer{std::move(c), {}, {}});
        }
        continue;
      }
      auto it = peers.find(id);
      if (it == peers.end())
        continue;
      auto &p = it->second;
      bool closed = false;
      if (p.output.empty()) {
        char b[2048];
        auto got = recv(p.fd.get(), b, sizeof(b), 0);
        if (got > 0) {
          p.input.append(b, got);
          if (p.input.size() > 4096)
            closed = true;
          else if (p.input.find("\r\n\r\n") != p.input.npos) {
            std::string body;
            int status = 200;
            std::string type = "text/plain; version=0.0.4";
            if (p.input.starts_with("GET /healthz HTTP/1.1\r\n"))
              body = "ok\n";
            else if (p.input.starts_with("GET /readyz HTTP/1.1\r\n")) {
              status = metrics_.ready.load() ? 200 : 503;
              body = status == 200 ? "ready\n" : "not ready\n";
            } else if (p.input.starts_with("GET /metrics HTTP/1.1\r\n")) {
              auto cfg = configs_.load();
              auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
              body = metrics_.render() + "# TYPE zt_policy_version gauge\nzt_policy_version " +
                     std::to_string(cfg->policy_version) +
                     "\n# TYPE zt_policy_age_seconds gauge\nzt_policy_age_seconds " +
                     std::to_string(std::max<int64_t>(0, now - cfg->policy_loaded_at)) + "\n";
            } else if (p.input.starts_with("GET /policy HTTP/1.1\r\n")) {
              auto cfg = configs_.load();
              type = "application/json";
              body = "{\"version\":" + std::to_string(cfg->policy_version) + ",\"sha256\":\"" +
                     cfg->policy_sha256 +
                     "\",\"loaded_at\":" + std::to_string(cfg->policy_loaded_at) + "}";
            } else {
              status = 404;
              body = "not found\n";
            }
            p.output = "HTTP/1.1 " + std::to_string(status) +
                       " Response\r\nConnection: close\r\nContent-Type: " + type +
                       "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
            loop.modify(p.fd.get(), id, EPOLLOUT);
          }
        } else if (got == 0 || (errno != EAGAIN && errno != EINTR))
          closed = true;
      }
      if (!p.output.empty()) {
        auto sent =
            send(p.fd.get(), p.output.data() + p.sent, p.output.size() - p.sent, MSG_NOSIGNAL);
        if (sent > 0)
          p.sent += sent;
        else if (sent < 0 && errno != EAGAIN && errno != EINTR)
          closed = true;
        if (p.sent == p.output.size())
          closed = true;
      }
      if (closed) {
        loop.remove(p.fd.get());
        peers.erase(it);
      }
    }
    auto now = std::chrono::steady_clock::now();
    for (auto it = peers.begin(); it != peers.end();) {
      if (now - it->second.start > std::chrono::seconds(3)) {
        loop.remove(it->second.fd.get());
        it = peers.erase(it);
      } else
        ++it;
    }
  }
}
} // namespace zt
