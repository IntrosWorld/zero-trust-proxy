#include "http/request_parser.h"
#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
namespace zt {
std::string lower(std::string_view value) {
  std::string out(value);
  for (char &c : out)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  return out;
}
static std::string trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
    s.remove_suffix(1);
  return std::string(s);
}
static bool token(std::string_view s) {
  if (s.empty())
    return false;
  for (unsigned char c : s)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos))
      return false;
  return true;
}
static bool number(std::string_view s, uint64_t &n, int base = 10) {
  if (s.empty())
    return false;
  auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), n, base);
  return e == std::errc{} && p == s.data() + s.size();
}
static bool authority(std::string_view s, Request &r, bool required) {
  if (s.empty() || s.find_first_of("@/\\?# \t\r\n") != s.npos)
    return false;
  std::string_view port;
  if (s.front() == '[') {
    auto end = s.find(']');
    if (end == s.npos)
      return false;
    r.host = lower(s.substr(1, end - 1));
    if (end + 1 < s.size()) {
      if (s[end + 1] != ':')
        return false;
      port = s.substr(end + 2);
    } else if (required)
      return false;
  } else {
    auto colon = s.find(':');
    if (colon != s.npos) {
      r.host = lower(s.substr(0, colon));
      port = s.substr(colon + 1);
    } else {
      if (required)
        return false;
      r.host = lower(s);
    }
  }
  if (!port.empty()) {
    uint64_t n = 0;
    if (!number(port, n) || n == 0 || n > 65535)
      return false;
    r.port = n;
  } else if (s.back() == ':' || required)
    return false;
  return !r.host.empty();
}
ParseStatus RequestParser::feed(std::string_view bytes) {
  if (status_ != ParseStatus::need_more)
    return status_;
  // Caller reads bounded blocks. A header limit does not count coalesced body bytes.
  if (bytes.size() > 65536) {
    status_ = ParseStatus::error;
    error_code = 431;
    return status_;
  }
  data_.append(bytes);
  auto end = data_.find("\r\n\r\n");
  auto line = data_.find("\r\n");
  if ((line == data_.npos && data_.size() > 8192) || (line != data_.npos && line > 8192) ||
      (end == data_.npos && data_.size() > 16384) || (end != data_.npos && end + 4 > 16384)) {
    status_ = ParseStatus::error;
    error_code = 431;
  } else if (end != data_.npos) {
    remainder = data_.substr(end + 4);
    data_.resize(end + 2);
    parse();
  }
  return status_;
}
void RequestParser::parse() {
  status_ = ParseStatus::error;
  auto end = data_.find("\r\n");
  std::string_view line(data_.data(), end);
  auto a = line.find(' '), b = line.rfind(' ');
  if (a == line.npos || a == b || line.substr(b + 1) != "HTTP/1.1" || !token(line.substr(0, a)))
    return;
  request.method = std::string(line.substr(0, a));
  auto target = line.substr(a + 1, b - a - 1);
  for (unsigned char c : target)
    if (c <= 32 || c >= 127)
      return;
  request.tunnel = request.method == "CONNECT";
  if (request.tunnel) {
    request.port = 443;
    if (!authority(target, request, true))
      return;
  } else {
    if (!target.starts_with("http://"))
      return;
    target.remove_prefix(7);
    auto slash = target.find_first_of("/?");
    if (!authority(target.substr(0, slash), request, false))
      return;
    request.path = slash == target.npos ? "/" : std::string(target.substr(slash));
    if (request.path.front() == '?')
      request.path.insert(0, "/");
    if (request.path.find('#') != std::string::npos || request.path.find('\\') != std::string::npos)
      return;
  }
  bool cl = false, te = false, host = false, auth = false;
  size_t pos = end + 2;
  while (pos < data_.size()) {
    end = data_.find("\r\n", pos);
    if (end == data_.npos)
      return;
    line = std::string_view(data_).substr(pos, end - pos);
    pos = end + 2;
    auto colon = line.find(':');
    if (colon == line.npos || !token(line.substr(0, colon)))
      return;
    auto name = lower(line.substr(0, colon));
    auto value = trim(line.substr(colon + 1));
    for (unsigned char c : value)
      if ((c < 32 && c != '\t') || c == 127)
        return;
    if (request.headers.size() >= 100) {
      error_code = 431;
      return;
    }
    if (name == "content-length") {
      if (cl || !number(value, request.content_length))
        return;
      cl = true;
    }
    if (name == "transfer-encoding") {
      if (te || lower(value) != "chunked")
        return;
      te = true;
      request.chunked = true;
    }
    if (name == "host") {
      if (host)
        return;
      host = true;
      Request h;
      h.port = request.port;
      if (!authority(value, h, false) || h.host != request.host || h.port != request.port)
        return;
    }
    if (name == "proxy-authorization") {
      if (auth)
        return;
      auth = true;
      request.authorization = value;
    }
    if (name == "upgrade" || name == "expect")
      return; // No upgrade or 100-continue deadlocks.
    request.headers.emplace_back(name, value);
  }
  if (!host || (cl && te) || (request.tunnel && (te || request.content_length != 0)))
    return;
  // Removing framing headers via Connection would create ambiguous requests.
  for (const auto &[name, value] : request.headers)
    if (name == "connection") {
      size_t start = 0;
      do {
        auto comma = value.find(',', start);
        auto item = lower(trim(std::string_view(value).substr(
            start, comma == value.npos ? value.size() - start : comma - start)));
        if (!token(item) || item == "host" || item == "content-length" ||
            item == "transfer-encoding")
          return;
        if (comma == value.npos)
          break;
        start = comma + 1;
      } while (true);
    }
  status_ = ParseStatus::complete;
}
std::string Request::forward_headers() const {
  std::set<std::string> strip = {"host",
                                 "proxy-authorization",
                                 "proxy-authenticate",
                                 "proxy-connection",
                                 "connection",
                                 "keep-alive",
                                 "te",
                                 "trailer",
                                 "upgrade"};
  for (const auto &[name, value] : headers)
    if (name == "connection") {
      size_t start = 0;
      do {
        auto comma = value.find(',', start);
        strip.insert(lower(trim(std::string_view(value).substr(
            start, comma == value.npos ? value.size() - start : comma - start))));
        if (comma == value.npos)
          break;
        start = comma + 1;
      } while (true);
    }
  std::string out = method + " " + path + " HTTP/1.1\r\nHost: " +
                    (host.find(':') != host.npos ? "[" + host + "]" : host) + ":" +
                    std::to_string(port) + "\r\n";
  for (const auto &[name, value] : headers)
    if (!strip.contains(name))
      out += name + ": " + value + "\r\n";
  return out + "Connection: close\r\n\r\n";
}
BodyFramer::BodyFramer(const Request &r) : left_(r.content_length), chunked_(r.chunked) {
  if (!chunked_)
    state_ = left_ ? State::data : State::done;
}
bool BodyFramer::consume(std::string_view bytes) {
  for (unsigned char ch : bytes) {
    if (state_ == State::done)
      return false;
    if (state_ == State::data) {
      if (--left_ == 0)
        state_ = chunked_ ? State::cr : State::done;
      continue;
    }
    if (state_ == State::cr) {
      if (ch != '\r')
        return false;
      state_ = State::lf;
      continue;
    }
    if (state_ == State::lf) {
      if (ch != '\n')
        return false;
      state_ = State::size;
      continue;
    }
    line_ += static_cast<char>(ch);
    if (line_.size() > 8192 || (state_ == State::trailers && ++trailer_bytes_ > 16384))
      return false;
    if (ch == '\n') {
      if (line_.size() < 2 || line_[line_.size() - 2] != '\r')
        return false;
      line_.resize(line_.size() - 2);
      if (state_ == State::size) {
        auto semi = line_.find(';');
        auto size = std::string_view(line_).substr(0, semi);
        if (!number(size, left_, 16))
          return false;
        if (semi != line_.npos)
          for (unsigned char c : std::string_view(line_).substr(semi + 1))
            if (c < 32 || c >= 127)
              return false;
        state_ = left_ ? State::data : State::trailers;
      } else {
        if (line_.empty())
          state_ = State::done;
        else {
          auto colon = line_.find(':');
          if (colon == line_.npos || !token(std::string_view(line_).substr(0, colon)))
            return false;
          auto name = lower(std::string_view(line_).substr(0, colon));
          if (name == "content-length" || name == "transfer-encoding" || name == "host" ||
              name == "connection" || name == "proxy-authorization")
            return false;
          for (unsigned char c : line_)
            if ((c < 32 && c != '\t') || c == 127)
              return false;
        }
      }
      line_.clear();
    }
  }
  return true;
}
} // namespace zt
