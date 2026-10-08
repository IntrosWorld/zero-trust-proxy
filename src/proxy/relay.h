#pragma once
#include <array>
#include <cstring>
#include <string_view>
#include <stdexcept>
namespace zt {
class Buffer {
  std::array<char,65536> bytes_{}; size_t begin_=0,end_=0;
public:
  size_t size() const {return end_-begin_;}
  size_t room() const {return bytes_.size()-size();}
  bool empty() const {return begin_==end_;}
  const char* data() const {return bytes_.data()+begin_;}
  void consume(size_t n) {begin_+=n;if(begin_==end_) begin_=end_=0;}
  void append(std::string_view s) {if(s.size()>room()) throw std::runtime_error("buffer capacity exceeded"); if(end_+s.size()>bytes_.size()) {std::memmove(bytes_.data(),data(),size());end_=size();begin_=0;} std::memcpy(bytes_.data()+end_,s.data(),s.size());end_+=s.size();}
};
}
