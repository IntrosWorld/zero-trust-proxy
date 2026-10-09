#include "admin/server.h"
#include "config/config.h"
#include "config/json.h"
#include "control/policy_sync.h"
#include "proxy/worker.h"
#include <csignal>
#include <fstream>
#include <iostream>
#include <pthread.h>
#include <sstream>
#include <vector>
int main(int argc, char **argv) {
  try {
    std::string path = "config/zt-proxy.json";
    bool allow_test = false, check_only = false;
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "--validate-policy" && i + 1 < argc) {
        std::ifstream f(argv[++i]);
        std::ostringstream data;
        data << f.rdbuf();
        if (!f)
          throw std::runtime_error("cannot read policy");
        zt::Config base;
        auto c = zt::parse_policy(data.str(), base);
        std::cout << "{\"policy\":" << zt::policy_json(*c) << ",\"version\":" << c->policy_version
                  << ",\"sha256\":\"" << c->policy_sha256 << "\"}\n";
        return 0;
      } else if (arg == "--config" && i + 1 < argc)
        path = argv[++i];
      else if (arg == "--allow-test-config")
        allow_test = true;
      else if (arg == "--check-config")
        check_only = true;
      else if (arg == "--version") {
        std::cout << "zt-proxy 1.0.0\n";
        return 0;
      } else if (arg == "--help") {
        std::cout << "Usage: zt-proxy [--config FILE] [--check-config] [--allow-test-config]\n";
        return 0;
      } else
        throw std::runtime_error("unknown argument: " + arg);
    }
    auto config = zt::load_config(path, allow_test);
    if (check_only) {
      std::cout << "configuration valid\n";
      return 0;
    }
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    std::signal(SIGPIPE, SIG_IGN);
    zt::ConfigStore store(config);
    zt::Metrics metrics;
    zt::AccessLog log(config->log_path, metrics);
    zt::Resolver resolver(config->resolver_threads);
    std::unique_ptr<zt::RedisLimiter> redis;
    if (!config->redis_host.empty())
      redis = std::make_unique<zt::RedisLimiter>(config->redis_threads, metrics);
    zt::AdminServer admin(store, metrics);
    zt::PolicySync sync(store, metrics);
    std::vector<std::unique_ptr<zt::Worker>> workers;
    for (int i = 0; i < config->workers; ++i)
      workers.push_back(std::make_unique<zt::Worker>(store, resolver, metrics, log, redis.get()));
    for (auto &w : workers)
      w->start();
    metrics.ready.store(true);
    std::cerr << "zt-proxy listening on " << config->listen << ':' << config->port << " with "
              << workers.size() << " workers\n";
    for (;;) {
      int signal = 0;
      sigwait(&signals, &signal);
      if (signal != SIGHUP)
        break;
      try {
        auto fresh = zt::load_config(path, allow_test);
        if (!zt::same_runtime(*config, *fresh))
          throw std::runtime_error(
              "listener, workers, resolver, log and test-mode changes require restart");
        store.store(fresh, !config->policy_url.empty());
        ++metrics.policy_reload_ok;
        std::cerr << "configuration reloaded\n";
      } catch (const std::exception &e) {
        ++metrics.policy_reload_rejected;
        std::cerr << "reload rejected: " << e.what() << '\n';
      }
    }
    metrics.ready.store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(store.load()->shutdown_delay_ms));
    for (auto &w : workers)
      w->stop();
    for (auto &w : workers)
      w->join();
    std::cerr << "zt-proxy stopped\n";
  } catch (const std::exception &e) {
    std::cerr << "zt-proxy: " << e.what() << '\n';
    return 1;
  }
}
