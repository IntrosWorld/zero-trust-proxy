#include "control/transport.h"
#include "http/request_parser.h"
#include <cerrno>
#include <charconv>
#include <memory>
#include <netdb.h>
#include <poll.h>
#include <stdexcept>
namespace zt {
void Stream::deadline(int ms) {
  deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
}
void Stream::wait(short events) {
  for (;;) {
    if (cancel_ && cancel_->load())
      throw std::runtime_error("request cancelled");
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline_ - std::chrono::steady_clock::now())
                    .count();
    if (left <= 0)
      throw std::runtime_error("network deadline");
    pollfd p{fd_.get(), events, 0};
    int n = poll(&p, 1, std::min<int64_t>(left, 50));
    if (n > 0)
      return;
    if (n < 0 && errno != EINTR)
      throw std::runtime_error("poll failed");
  }
}
Stream::Stream(const std::string &host, int port, int ms, const std::atomic<bool> *cancel)
    : cancel_(cancel) {
  deadline(ms);
  addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  addrinfo *raw = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &raw))
    throw std::runtime_error("control DNS failed");
  std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> owned(raw, freeaddrinfo);
  for (auto *a = raw; a; a = a->ai_next) {
    fd_ = Fd(socket(a->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!fd_)
      continue;
    int rc = connect(fd_.get(), a->ai_addr, a->ai_addrlen);
    if (rc != 0 && errno != EINPROGRESS)
      continue;
    if (rc != 0)
      wait(POLLOUT);
    int error = 0;
    socklen_t size = sizeof(error);
    if (!getsockopt(fd_.get(), SOL_SOCKET, SO_ERROR, &error, &size) && !error)
      return;
  }
  throw std::runtime_error("control connection failed");
}
void Stream::write(const std::string &data) {
  size_t offset = 0;
  while (offset < data.size()) {
    wait(POLLOUT);
    auto n = send(fd_.get(), data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
    if (n > 0)
      offset += n;
    else if (n == 0 || (errno != EAGAIN && errno != EINTR))
      throw std::runtime_error("control write failed");
  }
}
std::string Stream::read() {
  char bytes[8192];
  for (;;) {
    wait(POLLIN);
    auto n = recv(fd_.get(), bytes, sizeof(bytes), 0);
    if (n >= 0)
      return std::string(bytes, n);
    if (errno != EAGAIN && errno != EINTR)
      throw std::runtime_error("control read failed");
  }
}
HttpResponse parse_response(const std::string &raw) {
  auto end = raw.find("\r\n\r\n"), line = raw.find("\r\n");
  if (end == raw.npos || end > 16384 || line == raw.npos || line < 13 ||
      !raw.starts_with("HTTP/1.1 "))
    throw std::runtime_error("invalid HTTP response");
  int status = 0;
  auto [p, ec] = std::from_chars(raw.data() + 9, raw.data() + 12, status);
  if (ec != std::errc() || p != raw.data() + 12 || raw[12] != ' ')
    throw std::runtime_error("invalid status");
  HttpResponse r{status, {}, raw.substr(end + 4)};
  for (size_t pos = line + 2; pos < end;) {
    auto next = raw.find("\r\n", pos), colon = raw.find(':', pos);
    if (colon == raw.npos || colon >= next)
      throw std::runtime_error("invalid HTTP header");
    auto key = lower(raw.substr(pos, colon - pos));
    size_t begin = colon + 1;
    while (begin < next && raw[begin] == ' ')
      ++begin;
    if (!r.headers.emplace(key, raw.substr(begin, next - begin)).second)
      throw std::runtime_error("duplicate HTTP header");
    pos = next + 2;
  }
  if (r.headers.contains("transfer-encoding"))
    throw std::runtime_error("chunked control responses unsupported");
  if (!r.headers.contains("content-length"))
    throw std::runtime_error("content-length required");
  size_t size = 0;
  const auto &text = r.headers.at("content-length");
  auto result = std::from_chars(text.data(), text.data() + text.size(), size);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size() ||
      size != r.body.size() || size > 1024 * 1024)
    throw std::runtime_error("invalid control body length");
  return r;
}
HttpResponse http_request(const std::string &url, const std::string &method,
                          const std::string &token, const std::string &etag,
                          const std::string &body, int timeout_ms,
                          const std::atomic<bool> *cancel) {
  if (!url.starts_with("http://") || url.find_first_of("\r\n") != url.npos ||
      token.find_first_of("\r\n") != token.npos || etag.find_first_of("\r\n") != etag.npos)
    throw std::runtime_error("invalid control URL/header");
  auto slash = url.find('/', 7);
  auto authority = url.substr(7, slash == url.npos ? url.npos : slash - 7);
  auto colon = authority.rfind(':');
  int port = 80;
  std::string host = authority;
  if (colon != authority.npos) {
    port = std::stoi(authority.substr(colon + 1));
    host = authority.substr(0, colon);
  }
  if (port < 1 || port > 65535 || host.empty())
    throw std::runtime_error("invalid control endpoint");
  Stream stream(host, port, timeout_ms, cancel);
  std::string request =
      method + " " + (slash == url.npos ? "/" : url.substr(slash)) +
      " HTTP/1.1\r\nHost: " + authority + "\r\nConnection: close\r\nAuthorization: Bearer " +
      token +
      "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
      "\r\n";
  if (!etag.empty())
    request += "If-None-Match: " + etag + "\r\n";
  stream.write(request + "\r\n" + body);
  std::string raw;
  for (;;) {
    auto chunk = stream.read();
    if (chunk.empty())
      break;
    raw += chunk;
    if (raw.size() > 1024 * 1024 + 16384)
      throw std::runtime_error("control response too large");
  }
  return parse_response(raw);
}
} // namespace zt
