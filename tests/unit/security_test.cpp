#include "policy/host.h"
#include "policy/policy.h"
#include "policy/rate_limit.h"
#include "proxy/relay.h"
#include "tls/sni.h"
#include <fstream>
#include <gtest/gtest.h>
using namespace zt;
TEST(Address, BlockedRanges) {
  for (auto ip : {"0.0.0.0",
                  "0.255.255.255",
                  "10.0.0.0",
                  "10.255.255.255",
                  "100.64.0.0",
                  "100.127.255.255",
                  "127.0.0.1",
                  "169.254.169.254",
                  "172.16.0.0",
                  "172.31.255.255",
                  "192.168.0.0",
                  "192.168.255.255",
                  "192.0.0.8",
                  "192.0.2.1",
                  "192.88.99.1",
                  "198.18.0.0",
                  "198.19.255.255",
                  "198.51.100.1",
                  "203.0.113.1",
                  "224.0.0.0",
                  "239.255.255.255",
                  "240.0.0.0",
                  "255.255.255.255",
                  "::",
                  "::1",
                  "::ffff:127.0.0.1",
                  "::ffff:10.0.0.1",
                  "fc00::1",
                  "fdff::1",
                  "fe80::1",
                  "ff00::1",
                  "2001:db8::1",
                  "2002:0a00:0001::1",
                  "64:ff9b::a00:1",
                  "2001::1",
                  "3fff::1"})
    EXPECT_FALSE(Address::parse(ip, 443).public_ip()) << ip;
}
TEST(Address, PublicBoundaries) {
  for (auto ip : {"9.255.255.255", "11.0.0.0", "100.63.255.255", "100.128.0.0", "172.15.255.255",
                  "172.32.0.0", "192.167.255.255", "192.169.0.0", "198.17.255.255", "198.20.0.0",
                  "8.8.8.8", "2606:4700:4700::1111", "::ffff:8.8.8.8"})
    EXPECT_TRUE(Address::parse(ip, 443).public_ip()) << ip;
}
TEST(Host, SyntaxAndWildcard) {
  EXPECT_TRUE(matches({"*.example.com"}, "a.example.com"));
  EXPECT_TRUE(matches({"*.example.com"}, "a.b.example.com"));
  EXPECT_FALSE(matches({"*.example.com"}, "example.com"));
  EXPECT_FALSE(matches({"*.example.com"}, "evilexample.com"));
  for (auto s : {"", "bad..test", "a.", "-bad.test", "a_bad.test", "a/b.test", "123.4.5.999%lo"})
    EXPECT_FALSE(valid_host(s));
  EXPECT_TRUE(suspicious_host("a.xn--test.com"));
}
TEST(Policy, OrderAndPrivate) {
  Config c;
  c.allowlist = {"good.test", "*.example.com", "xn--ok.test"};
  c.denylist = {"bad.example.com"};
  Request r;
  r.tunnel = true;
  r.port = 443;
  r.host = "good.test";
  EXPECT_TRUE(precheck(c, r).allow);
  r.host = "bad.example.com";
  EXPECT_EQ(precheck(c, r).rule, "denylisted");
  r.port = 22;
  EXPECT_EQ(precheck(c, r).rule, "port");
  r.port = 443;
  r.host = "1.2.3.4";
  EXPECT_EQ(precheck(c, r).rule, "ip-literal");
  r.host = "xn--bad.test";
  EXPECT_EQ(precheck(c, r).rule, "suspicious-host");
  r.host = "xn--ok.test";
  EXPECT_TRUE(precheck(c, r).allow);
  r.host = "good.test";
  r.tunnel = false;
  EXPECT_EQ(precheck(c, r).rule, "tls-required");
  c.http_allowlist = {"good.test"};
  EXPECT_TRUE(precheck(c, r).allow);
  EXPECT_EQ(
      address_check(c, r, {Address::parse("8.8.8.8", 443), Address::parse("127.0.0.1", 443)}).rule,
      "private-ip");
}
TEST(Auth, CompareAndTokens) {
  EXPECT_TRUE(constant_time_equal("abc", "abc"));
  EXPECT_FALSE(constant_time_equal("abc", "ab"));
  EXPECT_FALSE(constant_time_equal("abc", "abd"));
  Config c;
  c.require_auth = true;
  c.tokens = {"first", "second"};
  EXPECT_TRUE(authenticated(c, "Bearer second"));
  EXPECT_FALSE(authenticated(c, "Bearer wrong"));
  EXPECT_FALSE(authenticated(c, ""));
}
TEST(Rate, RefillAndClock) {
  RateLimiter r;
  auto now = Clock::now();
  EXPECT_TRUE(r.allow("a", 1, 2, now));
  EXPECT_TRUE(r.allow("a", 1, 2, now));
  EXPECT_FALSE(r.allow("a", 1, 2, now));
  EXPECT_FALSE(r.allow("a", 1, 2, now - std::chrono::seconds(1)));
  EXPECT_TRUE(r.allow("a", 1, 2, now + std::chrono::seconds(1)));
  EXPECT_TRUE(r.allow("b", 1, 2, now));
}
TEST(Buffer, Compaction) {
  Buffer b;
  b.append(std::string(65000, 'a'));
  b.consume(64000);
  b.append(std::string(64000, 'b'));
  EXPECT_EQ(b.size(), 65000U);
  EXPECT_EQ(b.data()[999], 'a');
  EXPECT_EQ(b.data()[1000], 'b');
  EXPECT_THROW(b.append(std::string(1000, 'x')), std::runtime_error);
}
static std::string be(size_t n, size_t count) {
  std::string s(count, '\0');
  for (size_t i = 0; i < count; ++i) {
    s[count - i - 1] = n & 255;
    n >>= 8;
  }
  return s;
}
static std::string hello(const std::string &hostname, bool include = true) {
  std::string body = "\x03\x03";
  body += std::string(32, '\0');
  body += be(0, 1) + be(2, 2) + be(0x1301, 2) + be(1, 1) + be(0, 1);
  if (include) {
    auto names = be(0, 1) + be(hostname.size(), 2) + hostname;
    auto extension = be(names.size(), 2) + names;
    auto extensions = be(0, 2) + be(extension.size(), 2) + extension;
    body += be(extensions.size(), 2) + extensions;
  }
  auto handshake = be(1, 1) + be(body.size(), 3) + body;
  return be(22, 1) + be(0x0301, 2) + be(handshake.size(), 2) + handshake;
}
TEST(Sni, TruncationsAndCases) {
  auto data = hello("allowed.test");
  for (size_t i = 0; i < data.size(); ++i)
    EXPECT_EQ(extract_sni(std::string_view(data).substr(0, i)).status, SniStatus::need_more_data)
        << i;
  EXPECT_EQ(extract_sni(data).hostname, "allowed.test");
  EXPECT_EQ(extract_sni(hello("", false)).status, SniStatus::no_sni);
  EXPECT_EQ(extract_sni("SSH-2.0").status, SniStatus::not_tls);
  EXPECT_EQ(extract_sni(hello("bad..test")).status, SniStatus::malformed);
  data[3] = 0x7f;
  EXPECT_EQ(extract_sni(data).status, SniStatus::malformed);
}
TEST(Sni, MultipleRecords) {
  auto data = hello("allowed.test");
  auto handshake = data.substr(5);
  auto first = handshake.substr(0, 20), second = handshake.substr(20);
  auto records = be(22, 1) + be(0x0303, 2) + be(first.size(), 2) + first + be(22, 1) +
                 be(0x0303, 2) + be(second.size(), 2) + second;
  EXPECT_EQ(extract_sni(records).hostname, "allowed.test");
}
TEST(Sni, RecordedOpenSSL) {
  std::ifstream in(ZT_SOURCE_DIR "/tests/fuzz/corpus/sni/openssl-clienthello.bin",
                   std::ios::binary);
  ASSERT_TRUE(in.good());
  std::string bytes((std::istreambuf_iterator<char>(in)), {});
  EXPECT_EQ(extract_sni(bytes).hostname, "allowed.test");
  for (size_t i = 0; i < bytes.size(); ++i)
    EXPECT_EQ(extract_sni(std::string_view(bytes).substr(0, i)).status, SniStatus::need_more_data);
}

TEST(Rate, IdleCleanupPreservesSlowLimits) {
  RateLimiter limiter;
  auto start = Clock::now();
  EXPECT_TRUE(limiter.allow("slow", 1.0 / 3600, 1, start));
  limiter.prune(start + std::chrono::seconds(601));
  EXPECT_FALSE(limiter.allow("slow", 1.0 / 3600, 1, start + std::chrono::seconds(601)));
  EXPECT_TRUE(limiter.allow("slow", 1.0 / 3600, 1, start + std::chrono::seconds(3601)));
}
