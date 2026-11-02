#include "src/core/net/transport/http/strict_url.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string_view>

namespace {

using libchiaking::net::http::StrictUrl;
using Strictness = StrictUrl::Strictness;

class StrictUrlRead : public ::testing::Test {
 protected:
  static auto accepted(std::string_view url, Strictness strictness) -> bool {
    return StrictUrl::read(url, strictness).has_value();
  }

  static auto both_accept(std::string_view url) -> testing::AssertionResult {
    if (!accepted(url, Strictness::request_target))
      return testing::AssertionFailure() << "a request target refused " << url;
    if (!accepted(url, Strictness::shown_link))
      return testing::AssertionFailure() << "a shown link refused " << url;
    return testing::AssertionSuccess();
  }

  static auto both_reject(std::string_view url) -> testing::AssertionResult {
    if (accepted(url, Strictness::request_target))
      return testing::AssertionFailure() << "a request target accepted " << url;
    if (accepted(url, Strictness::shown_link))
      return testing::AssertionFailure() << "a shown link accepted " << url;
    return testing::AssertionSuccess();
  }

  static auto only_shown_link_accepts(std::string_view url)
      -> testing::AssertionResult {
    if (accepted(url, Strictness::request_target))
      return testing::AssertionFailure() << "a request target accepted " << url;
    if (!accepted(url, Strictness::shown_link))
      return testing::AssertionFailure() << "a shown link refused " << url;
    return testing::AssertionSuccess();
  }
};

TEST_F(StrictUrlRead, SplitsSchemeHostPortAndTailWithoutRepair) {
  const auto plain = StrictUrl::read("HTTPS://Api.Example.COM:8443/a/./b?x=1#f",
                                     Strictness::request_target);
  ASSERT_TRUE(plain);
  EXPECT_EQ(plain->scheme(), "HTTPS");
  EXPECT_EQ(plain->authority(), "Api.Example.COM:8443");
  EXPECT_EQ(plain->host(), "Api.Example.COM");
  EXPECT_EQ(plain->port(), 8443U);
  EXPECT_EQ(plain->tail(), "/a/./b?x=1#f");
}

TEST_F(StrictUrlRead, LeavesTheTailAndPortEmptyForABareHost) {
  const auto bare =
      StrictUrl::read("https://host.example", Strictness::request_target);
  ASSERT_TRUE(bare);
  EXPECT_EQ(bare->host(), "host.example");
  EXPECT_FALSE(bare->port());
  EXPECT_EQ(bare->authority(), "host.example");
  EXPECT_TRUE(bare->tail().empty());
}

TEST_F(StrictUrlRead, KeepsTheBracketsOfAnIpv6Literal) {
  const auto literal =
      StrictUrl::read("https://[2001:db8::1]:443/p", Strictness::shown_link);
  ASSERT_TRUE(literal);
  EXPECT_EQ(literal->host(), "[2001:db8::1]");
  EXPECT_EQ(literal->port(), 443U);
  EXPECT_EQ(literal->authority(), "[2001:db8::1]:443");
  EXPECT_EQ(literal->tail(), "/p");
}

TEST_F(StrictUrlRead, EndsTheHostAtAQuery) {
  const auto mapped =
      StrictUrl::read("http://[::ffff:1.2.3.4]?q", Strictness::request_target);
  ASSERT_TRUE(mapped);
  EXPECT_EQ(mapped->host(), "[::ffff:1.2.3.4]");
  EXPECT_FALSE(mapped->port());
  EXPECT_EQ(mapped->tail(), "?q");
}

TEST_F(StrictUrlRead, EndsTheHostAtAFragment) {
  const auto fragment =
      StrictUrl::read("gaikai://local#x", Strictness::request_target);
  ASSERT_TRUE(fragment);
  EXPECT_EQ(fragment->scheme(), "gaikai");
  EXPECT_EQ(fragment->host(), "local");
  EXPECT_EQ(fragment->tail(), "#x");
}

TEST_F(StrictUrlRead, AcceptsMinimalAuthoritiesAtBothLevels) {
  EXPECT_TRUE(both_accept("https://a"));
  EXPECT_TRUE(both_accept("https://a-b.c9:65535/"));
  EXPECT_TRUE(both_accept("https://host:00080/"));
  EXPECT_TRUE(both_accept("https://[::]"));
  EXPECT_TRUE(both_accept("https://[1:2]:1"));
}

TEST_F(StrictUrlRead, RejectsAtBothLevelsWhatIsNotAnAuthority) {
  for (const std::string_view url : {
           "",
           "host.example/path",
           "://host.example",
           "https://",
           "https:///path",
           "https://?query",
           "https://#fragment",
           "https://user@host.example/",
           "https://user:secret@host.example/",
           "https://@host.example/",
           "https://host.example:/",
           "https://host.example:",
           "https://host.example:port/",
           "https://host.example:80a/",
           "https://host.example:+80/",
           "https://host.example:-1/",
           "https://host.example: 80/",
           "https://host.example:65536/",
           "https://host.example:99999999999999999999/",
           "https://host.example:80:81/",
           "https://host.example::80/",
           "https://:80/",
           "https://[::1/",
           "https://[]/",
           "https://[]:80/",
           "https://[1234]/",
           "https://[::g]/",
           "https://[::1%25eth0]/",
           "https://[::1]x/",
           "https://[::1]80/",
           "https://[::1]:/",
           "https://[::1]:x/",
           "https://[::1]:65536/",
           "https://[::1]:80:81/",
           "https://ho%73t.example/",
           "https://%68ost/",
       })
    EXPECT_TRUE(both_reject(url));
}

TEST_F(StrictUrlRead, HoldsRequestTargetsStricterThanShownLinks) {
  for (const std::string_view url : {
           "https://host_name.example/",
           "https://-host.example/",
           "https://host.example-/",
           "https://.host.example/",
           "https://host.example./",
           "https://host..example/",
           "https://host\\example/",
           "https://host example/",
           "https://h\xC3\xB6st.example/",
           "https://host!/",
           "https://]/",
           "https://a[b]/",
           "https://host.example:0/",
           "https://[::1]:0/",
           "https://[:]/",
           "https://[:]:80/",
       })
    EXPECT_TRUE(only_shown_link_accepts(url));
}

TEST_F(StrictUrlRead, ShownLinksKeepAZeroPort) {
  const auto zero =
      StrictUrl::read("http://host.example:0/x", Strictness::shown_link);
  ASSERT_TRUE(zero);
  EXPECT_EQ(zero->port(), 0U);
  EXPECT_EQ(zero->host(), "host.example");
}

TEST_F(StrictUrlRead, ShownLinksKeepALooseHostVerbatim) {
  const auto loose =
      StrictUrl::read("http://host_name.example", Strictness::shown_link);
  ASSERT_TRUE(loose);
  EXPECT_EQ(loose->host(), "host_name.example");
  EXPECT_TRUE(loose->tail().empty());
}

}  // namespace
