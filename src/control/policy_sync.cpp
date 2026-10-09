#include "control/policy_sync.h"
#include "config/json.h"
#include "control/transport.h"
#include "log/access_log.h"
#include <cstdlib>
#include <iostream>
#include <random>
namespace zt {
PolicySync::PolicySync(ConfigStore &store, Metrics &metrics) : store_(store), metrics_(metrics) {
  if (!store_.load()->policy_url.empty())
    thread_ = std::thread([this] { run(); });
}
PolicySync::~PolicySync() {
  stop_.store(true);
  cv_.notify_all();
  if (thread_.joinable())
    thread_.join();
}
void PolicySync::run() {
  std::mt19937 random(std::random_device{}());
  std::string etag;
  const char *pod = std::getenv("HOSTNAME");
  while (!stop_.load()) {
    auto cfg = store_.load();
    try {
      auto token = read_secret(cfg->policy_token_file);
      auto heartbeat = [&] {
        auto current = store_.load();
        auto response =
            http_request(cfg->policy_url + "/v1/heartbeat", "POST", token, "",
                         "{\"pod\":" + json_string(pod ? pod : "local") +
                             ",\"policy_version\":" + std::to_string(current->policy_version) +
                             ",\"ready\":" + (metrics_.ready.load() ? "true" : "false") + "}",
                         2000, &stop_);
        if (response.status != 200)
          throw std::runtime_error("heartbeat rejected");
      };
      heartbeat();
      auto response = http_request(
          cfg->policy_url + "/v1/policy?wait=" + std::to_string(cfg->policy_wait_seconds), "GET",
          token, etag, "", (cfg->policy_wait_seconds + 3) * 1000, &stop_);
      if (response.status == 200) {
        try {
          auto obj = Reader(response.body).read().as<Json::Object>();
          auto fresh = parse_policy(json_dump(obj.at("policy")), *store_.load());
          if (obj.at("version").as<double>() != fresh->policy_version ||
              obj.at("sha256").as<std::string>() != fresh->policy_sha256)
            throw std::runtime_error("policy digest/version mismatch");
          auto expected = "\"" + fresh->policy_sha256 + "\"";
          if (response.headers.at("etag") != expected)
            throw std::runtime_error("policy ETag mismatch");
          if (store_.store(fresh, true))
            ++metrics_.policy_reload_ok;
          etag = expected;
          heartbeat();
        } catch (const std::exception &) {
          ++metrics_.policy_reload_rejected;
          throw;
        }
      } else if (response.status != 304 || etag.empty())
        throw std::runtime_error("policy fetch rejected");
    } catch (const std::exception &e) {
      if (!stop_.load()) {
        ++metrics_.policy_sync_errors;
        std::cerr << "policy sync: " << e.what() << '\n';
      }
    }
    std::unique_lock lock(mutex_);
    cv_.wait_for(lock,
                 std::chrono::milliseconds(cfg->policy_poll_ms +
                                           random() % std::max(1, cfg->policy_poll_ms / 4)),
                 [&] { return stop_.load(); });
  }
}
} // namespace zt
