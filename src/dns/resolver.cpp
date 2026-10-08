#include "dns/resolver.h"
#include "net/event_loop.h"
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
namespace zt {
Mailbox::Mailbox():wakeup(make_wakeup()){}
Resolver::Resolver(int threads) { for(int i=0;i<threads;++i) threads_.emplace_back([this]{run();}); }
Resolver::~Resolver() { { std::lock_guard lock(mutex_); stop_=true; jobs_.clear(); } cv_.notify_all(); for(auto& t:threads_) t.join(); }
bool Resolver::submit(uint64_t id,const std::string& host,int port,std::shared_ptr<const Config> config,const std::shared_ptr<Mailbox>& box) { { std::lock_guard lock(mutex_); if(jobs_.size()>=4096 || stop_) return false; jobs_.push_back({id,host,port,std::move(config),box}); } cv_.notify_one(); return true; }
void Resolver::run() {
  for(;;) {
    Job j; { std::unique_lock lock(mutex_); cv_.wait(lock,[&]{return stop_ || !jobs_.empty();}); if(stop_) return; j=std::move(jobs_.front()); jobs_.pop_front(); }
    auto start=std::chrono::steady_clock::now(); DnsResult result{j.id,{},0};
    auto fake=j.config->test_dns.find(j.host);
    if(j.config->test_mode && fake!=j.config->test_dns.end()) { for(const auto& ip:fake->second) result.addresses.push_back(Address::parse(ip,j.port)); }
    else {
      addrinfo hints{}; hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM; addrinfo* raw=nullptr;
      if(getaddrinfo(j.host.c_str(),std::to_string(j.port).c_str(),&hints,&raw)==0) {
        std::unique_ptr<addrinfo,decltype(&freeaddrinfo)> owned(raw,freeaddrinfo);
        for(auto* a=raw;a;a=a->ai_next) if((a->ai_family==AF_INET || a->ai_family==AF_INET6) && a->ai_addrlen<=sizeof(sockaddr_storage)) { Address out; out.length=a->ai_addrlen; std::memcpy(&out.storage,a->ai_addr,a->ai_addrlen); result.addresses.push_back(out); }
      }
    }
    result.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    if(auto box=j.box.lock()) { { std::lock_guard lock(box->mutex); if(box->results.size()>=8192) continue; box->results.push_back(std::move(result)); } uint64_t one=1; auto ignored=write(box->wakeup.get(),&one,sizeof(one)); (void)ignored; }
  }
}
}
