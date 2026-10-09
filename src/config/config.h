#pragma once
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
namespace zt {
struct Config {
  int policy_version = 0, policy_poll_ms = 1000, policy_wait_seconds = 0;
  int redis_port = 6379, redis_threads = 2, redis_timeout_ms = 200;
  bool redis_fail_closed = false;
  std::string policy_sha256, tokens_file, policy_url, policy_token_file, redis_host,
      redis_password_file;
  int64_t policy_loaded_at = 0;
  std::string listen = "127.0.0.1", admin = "127.0.0.1", log_path = "access.jsonl";
  int port = 3128, admin_port = 9100, workers = 1, resolver_threads = 2;
  int max_connections = 4096, max_per_client = 256;
  int header_timeout_ms = 10000, connect_timeout_ms = 5000, dns_timeout_ms = 5000,
      idle_timeout_ms = 60000, tunnel_lifetime_ms = 3600000, drain_timeout_ms = 10000,
      shutdown_delay_ms = 0;
  double rate = 100, burst = 200;
  bool require_auth = false, test_mode = false, admin_allow_remote = false;
  std::vector<std::string> allowlist, denylist, http_allowlist, tokens;
  std::vector<int> allowed_ports{80, 443};
  std::map<std::string, std::vector<std::string>> test_dns;
  std::vector<std::string> test_private_hosts;
};
std::shared_ptr<const Config> load_config(const std::string &path, bool allow_test = false);
std::shared_ptr<const Config> parse_config(const std::string &text, bool allow_test = false,
                                           const Config *base = nullptr);
std::shared_ptr<const Config> parse_policy(const std::string &text, const Config &base);
std::string policy_json(const Config &config);
std::string read_secret(const std::string &path);
bool same_runtime(const Config &a, const Config &b);
class ConfigStore {
  std::shared_ptr<const Config> value_;
  std::mutex update_mutex_;

public:
  explicit ConfigStore(std::shared_ptr<const Config> value) : value_(std::move(value)) {}
  std::shared_ptr<const Config> load() const {
    return std::atomic_load(&value_);
  }
  bool store(std::shared_ptr<const Config> value, bool monotonic = false) {
    std::lock_guard lock(update_mutex_);
    auto old = load();
    if (!same_runtime(*old, *value))
      throw std::runtime_error("runtime change requires restart");
    if (monotonic && value->policy_version <= old->policy_version) {
      if (value->policy_version == old->policy_version &&
          value->policy_sha256 == old->policy_sha256)
        return false;
      throw std::runtime_error("stale or conflicting policy version");
    }
    std::atomic_store(&value_, std::move(value));
    return true;
  }
};
} // namespace zt
