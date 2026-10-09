#include "config/config.h"
#include "config/json.h"
#include "control/sha256.h"
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
std::shared_ptr<const Config> load_config(const std::string &path, bool allow_test) {
  std::ifstream f(path);
  if (!f)
    throw std::runtime_error("cannot read config: " + path);
  std::ostringstream buffer;
  buffer << f.rdbuf();
  if (buffer.str().size() > 1024 * 1024)
    throw std::runtime_error("config too large");
  return parse_config(buffer.str(), allow_test);
}
std::shared_ptr<const Config> parse_config(const std::string &text, bool allow_test,
                                           const Config *base) {
  if (text.size() > 1024 * 1024)
    throw std::runtime_error("config too large");
  auto obj = Reader(text).read().as<Json::Object>();
  auto c = std::make_shared<Config>();
  if (base)
    *c = *base;
  else
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
  auto optional_string = [&](const char *k, std::string &out) {
    known.insert(k);
    if (auto j = take(k))
      out = j->as<std::string>();
    if (out.find('\0') != out.npos || out.size() > 4096)
      throw std::runtime_error("invalid configuration string");
  };
  optional_string("tokens_file", c->tokens_file);
  optional_string("policy_url", c->policy_url);
  optional_string("policy_token_file", c->policy_token_file);
  optional_string("redis_host", c->redis_host);
  optional_string("redis_password_file", c->redis_password_file);
  integer("policy_version", c->policy_version, 0, 2147483647);
  integer("policy_poll_ms", c->policy_poll_ms, 50, 60000);
  integer("policy_wait_seconds", c->policy_wait_seconds, 0, 30);
  integer("redis_port", c->redis_port, 1, 65535);
  integer("redis_threads", c->redis_threads, 1, 16);
  integer("redis_timeout_ms", c->redis_timeout_ms, 10, 5000);
  boolean("redis_fail_closed", c->redis_fail_closed);
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
  integer("shutdown_delay_ms", c->shutdown_delay_ms, 0, 60000);
  boolean("admin_allow_remote", c->admin_allow_remote);
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
  if (!base && !c->tokens_file.empty())
    c->tokens = {read_secret(c->tokens_file)};
  if (!c->policy_url.empty() &&
      (c->policy_token_file.empty() || !c->policy_url.starts_with("http://")))
    throw std::runtime_error("policy_url requires http:// and policy_token_file");
  auto bind = Address::parse(c->listen, c->port);
  auto admin = Address::parse(c->admin, c->admin_port);
  if (!admin.loopback() && !(c->admin_allow_remote && c->require_auth))
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
  c->policy_sha256 = sha256(policy_json(*c));
  c->policy_loaded_at = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
  return c;
}
bool same_runtime(const Config &a, const Config &b) {
  return a.listen == b.listen && a.port == b.port && a.admin == b.admin &&
         a.admin_port == b.admin_port && a.workers == b.workers &&
         a.resolver_threads == b.resolver_threads && a.log_path == b.log_path &&
         a.test_mode == b.test_mode && a.admin_allow_remote == b.admin_allow_remote &&
         a.policy_url == b.policy_url && a.policy_token_file == b.policy_token_file &&
         a.redis_host == b.redis_host && a.redis_port == b.redis_port &&
         a.redis_password_file == b.redis_password_file && a.redis_threads == b.redis_threads &&
         a.redis_timeout_ms == b.redis_timeout_ms && a.tokens_file == b.tokens_file;
}
std::string read_secret(const std::string &path) {
  std::ifstream file(path);
  std::string token;
  if (!file || !std::getline(file, token) || token.empty() || token.size() > 1024 ||
      std::any_of(token.begin(), token.end(), [](unsigned char c) { return c < 33 || c > 126; }))
    throw std::runtime_error("cannot read valid secret");
  return token;
}
std::string policy_json(const Config &c) {
  Json::Object obj;
  auto strings = [&](const char *key, const auto &items) {
    Json::Array values;
    for (const auto &v : items)
      values.push_back(Json{v});
    obj[key] = Json{values};
  };
  strings("allowlist", c.allowlist);
  strings("denylist", c.denylist);
  strings("http_allowlist", c.http_allowlist);
  Json::Array ports;
  for (int port : c.allowed_ports)
    ports.push_back(Json{double(port)});
  obj["allowed_ports"] = Json{ports};
  obj["rate"] = Json{c.rate};
  obj["burst"] = Json{c.burst};
  obj["policy_version"] = Json{double(c.policy_version)};
  return json_dump(Json{obj});
}
std::shared_ptr<const Config> parse_policy(const std::string &text, const Config &base) {
  const auto obj = Reader(text).read().as<Json::Object>();
  const std::set<std::string> fields{"allowlist", "denylist", "http_allowlist", "allowed_ports",
                                     "rate",      "burst",    "policy_version"};
  for (const auto &[key, value] : obj) {
    (void)value;
    if (!fields.contains(key))
      throw std::runtime_error("remote runtime field: " + key);
  }
  if (!obj.contains("policy_version"))
    throw std::runtime_error("policy_version required");
  return parse_config(text, base.test_mode, &base);
}
} // namespace zt
