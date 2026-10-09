#include "config/config.h"
#include "config/json.h"
#include "control/sha256.h"
#include "control/transport.h"
#include <gtest/gtest.h>
TEST(Control, HashAndPolicyValidation) {
  EXPECT_EQ(zt::sha256("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  auto base = zt::parse_config(R"({"workers":1,"allowlist":["example.com"]})");
  auto next =
      zt::parse_policy(R"({"policy_version":1,"allowlist":["EXAMPLE.COM"],"rate":2})", *base);
  EXPECT_EQ(next->allowlist[0], "example.com");
  EXPECT_EQ(next->policy_sha256, zt::sha256(zt::policy_json(*next)));
  EXPECT_THROW(zt::parse_policy(R"({"policy_version":2,"workers":2})", *base), std::runtime_error);
  zt::ConfigStore store(base);
  EXPECT_TRUE(store.store(next, true));
  EXPECT_FALSE(store.store(next, true));
  EXPECT_THROW(store.store(base, true), std::runtime_error);
  auto conflict = zt::parse_policy(R"({"policy_version":1,"rate":3})", *base);
  EXPECT_THROW(store.store(conflict, true), std::runtime_error);
}
TEST(Control, ResponseFramingAndETag) {
  auto r =
      zt::parse_response("HTTP/1.1 304 Not Modified\r\nETag: \"abc\"\r\nContent-Length: 0\r\n\r\n");
  EXPECT_EQ(r.status, 304);
  EXPECT_EQ(r.headers.at("etag"), "\"abc\"");
  EXPECT_THROW(zt::parse_response("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nabc"),
               std::runtime_error);
  EXPECT_THROW(
      zt::parse_response("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n"),
      std::runtime_error);
  EXPECT_THROW(zt::parse_response("HTTP/1.1 \r\n\r\n"), std::runtime_error);
}

#include "policy/resp.h"
TEST(Redis, PartialResponsesAndErrors) {
  auto wire = zt::resp_command({"SET", "a", "hello\r\nworld"});
  for (size_t n = 0; n < wire.size(); ++n) {
    size_t used = 99;
    EXPECT_FALSE(zt::resp_parse(std::string_view(wire).substr(0, n), used));
    EXPECT_EQ(used, 0U);
  }
  size_t used = 0;
  auto parsed = zt::resp_parse(wire, used);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->elements[2].text, "hello\r\nworld");
  EXPECT_EQ(used, wire.size());
  EXPECT_EQ(zt::resp_parse("-NOSCRIPT unknown\r\n", used)->type, '-');
  EXPECT_EQ(zt::resp_parse(":123\r\n", used)->number, 123);
  EXPECT_THROW(zt::resp_parse("$999999999\r\n", used), std::runtime_error);
  EXPECT_THROW(zt::resp_parse("$1\r\nxZZ", used), std::runtime_error);
}
