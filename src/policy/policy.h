#pragma once
#include "config/config.h"
#include "http/request_parser.h"
#include "net/addr.h"
namespace zt {
struct Decision {
  bool allow = true;
  int status = 200;
  std::string rule = "allow", reason = "allowed";
};
Decision precheck(const Config &c, const Request &r);
Decision address_check(const Config &c, const Request &r, const std::vector<Address> &addresses);
bool authenticated(const Config &c, const std::string &authorization);
} // namespace zt
