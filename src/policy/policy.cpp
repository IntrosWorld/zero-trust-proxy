#include "policy/policy.h"
#include "policy/host.h"
#include <algorithm>
namespace zt {
bool authenticated(const Config &c, const std::string &authorization) {
  if (!c.require_auth)
    return true;
  bool matched = false;
  for (const auto &token : c.tokens)
    matched = constant_time_equal(authorization, "Bearer " + token) | matched;
  return matched;
}
Decision precheck(const Config &c, const Request &r) {
  if (std::find(c.allowed_ports.begin(), c.allowed_ports.end(), r.port) == c.allowed_ports.end())
    return {false, 403, "port", "destination port not allowed"};
  bool literal = ip_literal(r.host);
  bool exact = std::find(c.allowlist.begin(), c.allowlist.end(), r.host) != c.allowlist.end();
  if (literal && !exact)
    return {false, 403, "ip-literal", "raw IP requires exact allowlist entry"};
  if (!literal && (!valid_host(r.host) || (suspicious_host(r.host) && !exact)))
    return {false, 403, "suspicious-host", "invalid or non-exempt IDN hostname"};
  if (matches(c.denylist, r.host))
    return {false, 403, "denylisted", "destination denied"};
  if (!matches(c.allowlist, r.host))
    return {false, 403, "not-allowlisted", "destination not allowed"};
  if (!r.tunnel && !matches(c.http_allowlist, r.host))
    return {false, 403, "tls-required", "use HTTPS CONNECT"};
  return {};
}
Decision address_check(const Config &c, const Request &r, const std::vector<Address> &addresses) {
  if (addresses.empty())
    return {false, 502, "dns", "no DNS addresses"};
  bool test = c.test_mode && std::find(c.test_private_hosts.begin(), c.test_private_hosts.end(),
                                       r.host) != c.test_private_hosts.end();
  for (const auto &a : addresses)
    if (!a.public_ip() && !test)
      return {false, 403, "private-ip", "DNS contains a non-public address"};
  return {};
}
} // namespace zt
