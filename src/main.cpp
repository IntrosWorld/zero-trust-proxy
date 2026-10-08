#include "config/config.h"
#include "proxy/worker.h"
#include "admin/server.h"
#include <csignal>
#include <iostream>
#include <vector>
#include <pthread.h>
int main(int argc,char** argv){
  try{
    std::string path="config/zt-proxy.json";bool allow_test=false,check_only=false;
    for(int i=1;i<argc;++i){std::string arg=argv[i];if(arg=="--config" && i+1<argc)path=argv[++i];else if(arg=="--allow-test-config")allow_test=true;else if(arg=="--check-config")check_only=true;else if(arg=="--version"){std::cout<<"zt-proxy 1.0.0\n";return 0;}else if(arg=="--help"){std::cout<<"Usage: zt-proxy [--config FILE] [--check-config] [--allow-test-config]\n";return 0;}else throw std::runtime_error("unknown argument: "+arg);}
    auto config=zt::load_config(path,allow_test);if(check_only){std::cout<<"configuration valid\n";return 0;}
    sigset_t signals;sigemptyset(&signals);sigaddset(&signals,SIGINT);sigaddset(&signals,SIGTERM);sigaddset(&signals,SIGHUP);pthread_sigmask(SIG_BLOCK,&signals,nullptr);std::signal(SIGPIPE,SIG_IGN);
    zt::ConfigStore store(config);zt::Metrics metrics;zt::AccessLog log(config->log_path,metrics);zt::Resolver resolver(config->resolver_threads);zt::AdminServer admin(*config,metrics);
    std::vector<std::unique_ptr<zt::Worker>> workers;for(int i=0;i<config->workers;++i)workers.push_back(std::make_unique<zt::Worker>(store,resolver,metrics,log));for(auto& w:workers)w->start();
    std::cerr<<"zt-proxy listening on "<<config->listen<<':'<<config->port<<" with "<<workers.size()<<" workers\n";
    for(;;){int signal=0;sigwait(&signals,&signal);if(signal!=SIGHUP)break;try{auto fresh=zt::load_config(path,allow_test);if(!zt::same_runtime(*config,*fresh))throw std::runtime_error("listener, workers, resolver, log and test-mode changes require restart");store.store(fresh);std::cerr<<"configuration reloaded\n";}catch(const std::exception& e){std::cerr<<"reload rejected: "<<e.what()<<'\n';}}
    for(auto& w:workers)w->stop();
    for(auto& w:workers)w->join();
    std::cerr<<"zt-proxy stopped\n";
  }catch(const std::exception& e){std::cerr<<"zt-proxy: "<<e.what()<<'\n';return 1;}
}
