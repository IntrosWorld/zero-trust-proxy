#include "tls/sni.h"
#include "http/request_parser.h"
#include "policy/host.h"
#include <cstdint>
namespace zt {
namespace {
struct Cursor { std::string_view s; size_t p=0; bool good=true; size_t left() const {return s.size()-p;} unsigned read(size_t n) { if(n>left()) {good=false;return 0;} unsigned v=0; while(n--) v=(v<<8)|static_cast<unsigned char>(s[p++]); return v; } std::string_view take(size_t n) { if(n>left()) {good=false;return {};} auto v=s.substr(p,n); p+=n; return v; } };
SniResult bad() { return {SniStatus::malformed,{}}; }
}
SniResult extract_sni(std::string_view bytes) {
  if(bytes.empty()) return {SniStatus::need_more_data,{}};
  if(static_cast<unsigned char>(bytes[0])!=22) return {SniStatus::not_tls,{}};
  if(bytes.size()>16384) return bad();
  std::string handshake; size_t pos=0; size_t needed=0;
  while(pos<bytes.size()) {
    if(bytes.size()-pos<5) return {SniStatus::need_more_data,{}};
    Cursor record{bytes.substr(pos)};
    if(record.read(1)!=22 || record.read(1)!=3 || record.read(1)>4) return bad();
    size_t len=record.read(2); if(len==0 || len>16379) return bad();
    if(record.left()<len) return {SniStatus::need_more_data,{}};
    handshake.append(record.take(len)); pos+=5+len;
    if(handshake.size()>=4) { Cursor h{handshake}; if(h.read(1)!=1) return bad(); needed=h.read(3)+4; if(needed>16379) return bad();
  if(handshake.size()>=needed) break; }
  }
  if(!needed || handshake.size()<needed) return {SniStatus::need_more_data,{}};
  Cursor h{std::string_view(handshake).substr(4,needed-4)};
  if(h.read(1)!=3 || h.read(1)>4) return bad();
  h.take(32); auto session=h.read(1); if(session>32) return bad(); h.take(session);
  auto cipher=h.read(2); if(cipher<2 || cipher%2) return bad(); h.take(cipher);
  auto compression=h.read(1); if(compression==0) return bad(); h.take(compression);
  if(!h.good) return bad();
  if(!h.left()) return {SniStatus::no_sni,{}};
  auto length=h.read(2); if(!h.good || length!=h.left()) return bad();
  bool seen=false; std::string host;
  while(h.left()) {
    if(h.left()<4) return bad();
    auto type=h.read(2); auto n=h.read(2); auto data=h.take(n); if(!h.good) return bad();
    if(type==0) {
      if(seen) return bad();
      seen=true; Cursor names{data}; auto list=names.read(2); if(!names.good || list!=names.left() || list==0) return bad();
      while(names.left()) { if(names.left()<3) return bad();
    auto kind=names.read(1); auto size=names.read(2); auto value=names.take(size); if(!names.good || value.empty()) return bad();
  if(kind==0) { if(!host.empty()) return bad(); host=lower(value); if(!valid_host(host)) return bad(); } }
    }
  }
  return host.empty() ? SniResult{SniStatus::no_sni,{}} : SniResult{SniStatus::ok,host};
}
}
