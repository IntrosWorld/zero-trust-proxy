#include "net/event_loop.h"
#include <array>
#include <unordered_map>
#include <string>
#include <iostream>
#include <csignal>
#include <cerrno>
int main(int argc, char** argv) {
  try {
    if (argc != 3 || std::string(argv[1]) != "--echo") { std::cerr << "Usage: zt-proxy --echo PORT\n"; return 1; }
    std::signal(SIGPIPE, SIG_IGN);
    auto server = zt::listener("127.0.0.1", std::stoi(argv[2]));
    zt::EventLoop loop; loop.add(server.get(), server.get(), EPOLLIN);
    struct Peer { zt::Fd fd; std::string pending; };
    std::unordered_map<int, Peer> peers;
    std::array<epoll_event, 128> events{};
    while (true) {
      int n = loop.wait(events);
      for (int i = 0; i < n; ++i) {
        int fd = events[i].data.u64;
        if (fd == server.get()) {
          int c = accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
          if (c >= 0) { peers.emplace(c, Peer{zt::Fd(c), {}}); loop.add(c, c, EPOLLIN); }
          continue;
        }
        auto& peer = peers.at(fd); char buffer[4096]; bool closed = false;
        if (events[i].events & EPOLLIN) { auto got = recv(fd, buffer, sizeof(buffer), 0); if (got > 0) peer.pending.append(buffer, got); else if (got == 0 || (errno != EAGAIN && errno != EINTR)) closed = true; }
        if (!peer.pending.empty()) { auto sent = send(fd, peer.pending.data(), peer.pending.size(), MSG_NOSIGNAL); if (sent > 0) peer.pending.erase(0, sent); }
        if (closed) { loop.remove(fd); peers.erase(fd); } else loop.modify(fd, fd, EPOLLIN | (peer.pending.empty() ? 0U : EPOLLOUT));
      }
    }
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
