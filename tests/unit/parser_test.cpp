#include "http/request_parser.h"
#include <gtest/gtest.h>
using namespace zt;
TEST(Parser, EverySplit) {
  std::string s =
      "POST http://example.com/a HTTP/1.1\r\nHost: example.com\r\nContent-Length: 4\r\n\r\ndata";
  for (size_t i = 0; i < s.size(); ++i) {
    RequestParser p;
    p.feed(std::string_view(s).substr(0, i));
    EXPECT_EQ(p.feed(std::string_view(s).substr(i)), ParseStatus::complete);
    EXPECT_EQ(p.request.host, "example.com");
  }
}
TEST(Parser, Bytewise) {
  RequestParser p;
  std::string s = "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n";
  for (char c : s)
    p.feed(std::string_view(&c, 1));
  EXPECT_TRUE(p.request.tunnel);
  EXPECT_EQ(p.feed(""), ParseStatus::complete);
}
TEST(Parser, RejectsAmbiguity) {
  for (auto h :
       {"Content-Length: 1\r\nTransfer-Encoding: chunked\r\n",
        "Content-Length: 1\r\nContent-Length: 1\r\n", "Transfer-Encoding: gzip, chunked\r\n",
        "Host: evil.test\r\n", "Connection: content-length\r\n", "Bad : x\r\n", " x: y\r\n",
        "Content-Length: +1\r\n"}) {
    RequestParser p;
    EXPECT_EQ(p.feed(std::string("POST http://example.com/ HTTP/1.1\r\nHost: example.com\r\n") + h +
                     "\r\n"),
              ParseStatus::error);
  }
}
TEST(Parser, LimitsAndOrigin) {
  RequestParser p;
  EXPECT_EQ(p.feed(std::string(8193, 'a')), ParseStatus::error);
  EXPECT_EQ(p.error_code, 431);
  RequestParser q;
  EXPECT_EQ(q.feed("GET / HTTP/1.1\r\nHost: x\r\n\r\n"), ParseStatus::error);
}
TEST(Parser, StripsHopHeaders) {
  RequestParser p;
  ASSERT_EQ(p.feed("GET http://a.test/ HTTP/1.1\r\nHost: a.test\r\nConnection: "
                   "x-secret\r\nX-Secret: value\r\nProxy-Authorization: Bearer secret\r\n\r\n"),
            ParseStatus::complete);
  auto out = p.request.forward_headers();
  EXPECT_EQ(out.find("secret"), out.npos);
  EXPECT_NE(out.find("GET / HTTP/1.1"), out.npos);
}
TEST(Body, ChunkedAndPipeline) {
  Request r;
  r.chunked = true;
  BodyFramer b(r);
  for (char c : std::string("4\r\ndata\r\n0\r\n\r\n"))
    EXPECT_TRUE(b.consume(std::string_view(&c, 1)));
  EXPECT_TRUE(b.done());
  EXPECT_FALSE(b.consume("GET"));
  BodyFramer bad(r);
  EXPECT_FALSE(bad.consume("z\r\n"));
}
TEST(Body, Length) {
  Request r;
  r.content_length = 4;
  BodyFramer b(r);
  EXPECT_TRUE(b.consume("data"));
  EXPECT_TRUE(b.done());
  EXPECT_FALSE(b.consume("x"));
}
