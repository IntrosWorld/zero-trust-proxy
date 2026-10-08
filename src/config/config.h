#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <atomic>
namespace zt {
struct Config {
  std::string listen="127.0.0.1", admin="127.0.0.1", log_path="access.jsonl";
  int port=3128, admin_port=9100, workers=1, resolver_threads=2;
  int max_connections=4096, max_per_client=256;
  int header_timeout_ms=10000, connect_timeout_ms=5000, dns_timeout_ms=5000, idle_timeout_ms=60000, tunnel_lifetime_ms=3600000, drain_timeout_ms=10000;
  double rate=100, burst=200;
  bool require_auth=false, test_mode=false;
  std::vector<std::string> allowlist, denylist, http_allowlist, tokens;
  std::vector<int> allowed_ports{80,443};
  std::map<std::string,std::vector<std::string>> test_dns;
  std::vector<std::string> test_private_hosts;
};
std::shared_ptr<const Config> load_config(const std::string& path, bool allow_test=false);
bool same_runtime(const Config& a, const Config& b);
class ConfigStore {
  std::shared_ptr<const Config> value_;
public:
  explicit ConfigStore(std::shared_ptr<const Config> value):value_(std::move(value)){}
  std::shared_ptr<const Config> load() const {return std::atomic_load(&value_);}
  void store(std::shared_ptr<const Config> value){std::atomic_store(&value_,std::move(value));}
};
}
