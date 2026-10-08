#include "config/config.h"
#include "http/request_parser.h"
#include "net/addr.h"
#include "policy/host.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <variant>
namespace zt {
namespace {
struct Json {
  using Array = std::vector<Json>;
  using Object = std::map<std::string, Json>;
  std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value;
  template <class T> const T &as() const {
    auto p = std::get_if<T>(&value);
    if (!p)
      throw std::runtime_error("incorrect JSON value type");
    return *p;
  }
};
class Reader {
  std::string s;
  size_t p = 0;
  void ws() {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r'))
      ++p;
  }
  char get() {
    if (p >= s.size())
      throw std::runtime_error("truncated JSON");
    return s[p++];
  }
  unsigned hex4() {
    unsigned value = 0;
    for (int i = 0; i < 4; ++i) {
      char c = get();
      unsigned digit;
      if (c >= '0' && c <= '9')
        digit = c - '0';
      else if (c >= 'a' && c <= 'f')
        digit = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
        digit = c - 'A' + 10;
      else
        throw std::runtime_error("invalid JSON unicode escape");
      value = (value << 4) | digit;
    }
    return value;
  }
  void unicode(std::string &out) {
    unsigned cp = hex4();
    if (cp >= 0xd800 && cp <= 0xdbff) {
      if (get() != '\\' || get() != 'u')
        throw std::runtime_error("missing low surrogate");
      unsigned lo = hex4();
      if (lo < 0xdc00 || lo > 0xdfff)
        throw std::runtime_error("invalid low surrogate");
      cp = 0x10000 + ((cp - 0xd800) << 10) + lo - 0xdc00;
    } else if (cp >= 0xdc00 && cp <= 0xdfff)
      throw std::runtime_error("unpaired low surrogate");
    if (cp < 0x80)
      out += static_cast<char>(cp);
    else if (cp < 0x800) {
      out += static_cast<char>(0xc0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 63));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xe0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 63));
      out += static_cast<char>(0x80 | (cp & 63));
    } else {
      out += static_cast<char>(0xf0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 63));
      out += static_cast<char>(0x80 | ((cp >> 6) & 63));
      out += static_cast<char>(0x80 | (cp & 63));
    }
  }
  std::string string() {
    if (get() != '"')
      throw std::runtime_error("expected JSON string");
    std::string out;
    for (;;) {
      unsigned char c = get();
      if (c == '"')
        return out;
      if (c < 32)
        throw std::runtime_error("control character in JSON");
      if (c == '\\') {
        c = get();
        if (c == 'n')
          out += '\n';
        else if (c == 'r')
          out += '\r';
        else if (c == 't')
          out += '\t';
        else if (c == 'b')
          out += '\b';
        else if (c == 'f')
          out += '\f';
        else if (c == 'u')
          unicode(out);
        else if (c == '"' || c == '\\' || c == '/')
          out += c;
        else
          throw std::runtime_error("invalid JSON escape");
      } else
        out += c;
    }
  }
  Json value(int depth = 0) {
    if (depth > 16)
      throw std::runtime_error("JSON nesting limit");
    ws();
    if (p >= s.size())
      throw std::runtime_error("missing JSON value");
    char c = s[p];
    if (c == '"')
      return Json{string()};
    if (c == '{' || c == '[') {
      ++p;
      ws();
      Json::Object obj;
      Json::Array arr;
      char end = c == '{' ? '}' : ']';
      if (p < s.size() && s[p] == end) {
        ++p;
        return c == '{' ? Json{obj} : Json{arr};
      }
      for (;;) {
        ws();
        if (c == '{') {
          auto key = string();
          ws();
          if (get() != ':')
            throw std::runtime_error("expected colon");
          if (!obj.emplace(key, value(depth + 1)).second)
            throw std::runtime_error("duplicate JSON key");
        } else
          arr.push_back(value(depth + 1));
        ws();
        char sep = get();
        if (sep == end)
          break;
        if (sep != ',')
          throw std::runtime_error("expected comma");
      }
      return c == '{' ? Json{obj} : Json{arr};
    }
    for (auto [literal, v] : {std::pair{"true", true}, std::pair{"false", false}})
      if (s.compare(p, std::char_traits<char>::length(literal), literal) == 0) {
        p += std::char_traits<char>::length(literal);
        return Json{v};
      }
    if (s.compare(p, 4, "null") == 0) {
      p += 4;
      return Json{nullptr};
    }
    size_t start = p;
    if (s[p] == '-')
      ++p;
    if (p >= s.size() || s[p] < '0' || s[p] > '9')
      throw std::runtime_error("invalid JSON number");
    if (s[p] == '0')
      ++p;
    else
      while (p < s.size() && s[p] >= '0' && s[p] <= '9')
        ++p;
    if (p < s.size() && s[p] == '.') {
      ++p;
      size_t begin = p;
      while (p < s.size() && s[p] >= '0' && s[p] <= '9')
        ++p;
      if (p == begin)
        throw std::runtime_error("invalid fraction");
    }
    if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
      ++p;
      if (p < s.size() && (s[p] == '+' || s[p] == '-'))
        ++p;
      size_t begin = p;
      while (p < s.size() && s[p] >= '0' && s[p] <= '9')
        ++p;
      if (p == begin)
        throw std::runtime_error("invalid exponent");
    }
    double n = std::stod(s.substr(start, p - start));
    if (!std::isfinite(n))
      throw std::runtime_error("nonfinite JSON number");
    return Json{n};
  }

public:
  explicit Reader(std::string text) : s(std::move(text)) {}
  Json read() {
    auto out = value();
    ws();
    if (p != s.size())
      throw std::runtime_error("trailing JSON data");
    return out;
  }
};
} // namespace
std::shared_ptr<const Config> load_config(const std::string &path, bool allow_test) {
  std::ifstream f(path);
  if (!f)
    throw std::runtime_error("cannot read config: " + path);
  std::ostringstream buffer;
  buffer << f.rdbuf();
  if (buffer.str().size() > 1024 * 1024)
    throw std::runtime_error("config too large");
  auto root = Reader(buffer.str()).read();
  auto obj = root.as<Json::Object>();
  auto c = std::make_shared<Config>();
  c->workers = std::max(1U, std::thread::hardware_concurrency());
  auto take = [&](const char *key) -> const Json * {
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &it->second;
  };
  std::set<std::string> known;
  auto str = [&](const char *k, std::string &out) {
    known.insert(k);
    if (auto j = take(k))
      out = j->as<std::string>();
    if (out.empty() || out.find('\0') != out.npos)
      throw std::runtime_error("empty or NUL configuration string");
  };
  auto integer = [&](const char *k, int &out, int lo, int hi) {
    known.insert(k);
    if (auto j = take(k)) {
      double n = j->as<double>();
      if (n < lo || n > hi || std::floor(n) != n)
        throw std::runtime_error(std::string("out of range: ") + k);
      out = n;
    }
  };
  auto real = [&](const char *k, double &out) {
    known.insert(k);
    if (auto j = take(k)) {
      out = j->as<double>();
      if (out <= 0 || out > 1e9)
        throw std::runtime_error(std::string("out of range: ") + k);
    }
  };
  auto boolean = [&](const char *k, bool &out) {
    known.insert(k);
    if (auto j = take(k))
      out = j->as<bool>();
  };
  auto strings = [&](const char *k, std::vector<std::string> &out) {
    known.insert(k);
    if (auto j = take(k)) {
      out.clear();
      for (const auto &v : j->as<Json::Array>())
        out.push_back(v.as<std::string>());
    }
  };
  str("listen", c->listen);
  str("admin", c->admin);
  str("log_path", c->log_path);
  integer("port", c->port, 1024, 65535);
  integer("admin_port", c->admin_port, 1024, 65535);
  integer("workers", c->workers, 1, 256);
  integer("resolver_threads", c->resolver_threads, 1, 64);
  integer("max_connections", c->max_connections, 1, 1000000);
  integer("max_per_client", c->max_per_client, 1, 1000000);
  integer("header_timeout_ms", c->header_timeout_ms, 50, 3600000);
  integer("connect_timeout_ms", c->connect_timeout_ms, 50, 3600000);
  integer("dns_timeout_ms", c->dns_timeout_ms, 50, 3600000);
  integer("idle_timeout_ms", c->idle_timeout_ms, 50, 86400000);
  integer("tunnel_lifetime_ms", c->tunnel_lifetime_ms, 100, 86400000);
  integer("drain_timeout_ms", c->drain_timeout_ms, 50, 3600000);
  real("rate", c->rate);
  real("burst", c->burst);
  boolean("require_auth", c->require_auth);
  boolean("test_mode", c->test_mode);
  strings("allowlist", c->allowlist);
  strings("denylist", c->denylist);
  strings("http_allowlist", c->http_allowlist);
  strings("tokens", c->tokens);
  strings("test_private_hosts", c->test_private_hosts);
  known.insert("allowed_ports");
  if (auto j = take("allowed_ports")) {
    c->allowed_ports.clear();
    for (const auto &v : j->as<Json::Array>()) {
      double n = v.as<double>();
      if (n < 1 || n > 65535 || std::floor(n) != n)
        throw std::runtime_error("invalid allowed port");
      c->allowed_ports.push_back(n);
    }
  }
  known.insert("test_dns");
  if (auto j = take("test_dns"))
    for (const auto &[host, ips] : j->as<Json::Object>()) {
      auto &out = c->test_dns[host];
      for (const auto &ip : ips.as<Json::Array>()) {
        const auto &text = ip.as<std::string>();
        (void)Address::parse(text, 80);
        out.push_back(text);
      }
    }
  for (const auto &[key, v] : obj) {
    (void)v;
    if (!known.contains(key))
      throw std::runtime_error("unknown config key: " + key);
  }
  for (auto *list : {&c->allowlist, &c->denylist, &c->http_allowlist, &c->test_private_hosts}) {
    for (auto &host : *list) {
      host = lower(host);
      auto value = host.starts_with("*.") ? host.substr(2) : host;
      if (!valid_host(value) && !ip_literal(value))
        throw std::runtime_error("invalid host pattern: " + host);
    }
  }
  auto bind = Address::parse(c->listen, c->port);
  auto admin = Address::parse(c->admin, c->admin_port);
  if (!admin.loopback())
    throw std::runtime_error("admin must bind a loopback address");
  if (!bind.loopback() && !c->require_auth)
    throw std::runtime_error("non-loopback listener requires authentication");
  if (c->require_auth && c->tokens.empty())
    throw std::runtime_error("authentication requires tokens");
  for (const auto &t : c->tokens)
    if (t.empty() || t.size() > 1024 ||
        std::any_of(t.begin(), t.end(), [](unsigned char ch) { return ch < 33 || ch > 126; }))
      throw std::runtime_error("invalid auth token");
  if (c->test_mode && (!allow_test || !bind.loopback()))
    throw std::runtime_error("test_mode requires --allow-test-config and loopback listener");
  if (!c->test_mode && (!c->test_dns.empty() || !c->test_private_hosts.empty()))
    throw std::runtime_error("test hooks require test_mode");
  if (c->burst < 1)
    throw std::runtime_error("burst must be at least one");
  return c;
}
bool same_runtime(const Config &a, const Config &b) {
  return a.listen == b.listen && a.port == b.port && a.admin == b.admin &&
         a.admin_port == b.admin_port && a.workers == b.workers &&
         a.resolver_threads == b.resolver_threads && a.log_path == b.log_path &&
         a.test_mode == b.test_mode;
}
} // namespace zt
