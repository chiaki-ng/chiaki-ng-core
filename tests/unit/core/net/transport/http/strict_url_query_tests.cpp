#include "src/core/net/transport/http/strict_url.hpp"
#include "src/core/psn/legacy_cloud_stream_endpoints.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace {

using libchiaking::net::http::StrictUrl;
using Fault = StrictUrl::QueryFault;

class StrictUrlQuery : public ::testing::Test {
 protected:
  static constexpr libchiaking::net::http::PercentEncoding::DecodeLimits limits{
      .encoded_bytes = 6144, .decoded_bytes = 2048};
  static constexpr libchiaking::net::http::PercentEncoding::DecodeLimits small{
      .encoded_bytes = 9, .decoded_bytes = 4};

  static auto exact(std::string_view query,
                    libchiaking::net::http::PercentEncoding::DecodeLimits
                        value_limits = limits) {
    return StrictUrl::only_secret_value(query, "code", "error", value_limits);
  }

  static auto first(std::string_view query, std::string_view key,
                    std::size_t capacity) {
    return StrictUrl::first_secret_value(query, key, capacity);
  }

  static auto rejected_as(std::string_view query, Fault expected)
      -> testing::AssertionResult {
    const auto value = exact(query);
    if (value)
      return testing::AssertionFailure()
             << "accepted " << query << " as " << value.value().view();
    if (value.error() != expected)
      return testing::AssertionFailure()
             << query << " failed with "
             << testing::PrintToString(value.error()) << " rather than "
             << testing::PrintToString(expected);
    return testing::AssertionSuccess();
  }
};

TEST_F(StrictUrlQuery, ExactReaderReturnsThePlainValue) {
  const auto plain = exact("code=abc123&cid=1");
  ASSERT_TRUE(plain);
  EXPECT_EQ(plain.value().view(), "abc123");
}

TEST_F(StrictUrlQuery, ExactReaderDecodesEscapesInKeyAndValue) {
  const auto escaped = exact("a=1&c%6Fde=v3.%41%2fb&");
  ASSERT_TRUE(escaped);
  EXPECT_EQ(escaped.value().view(), "v3.A/b");
}

TEST_F(StrictUrlQuery, ExactReaderFindsTheValueAfterEmptyFields) {
  const auto last = exact("x&y=&code=z");
  ASSERT_TRUE(last);
  EXPECT_EQ(last.value().view(), "z");
}

TEST_F(StrictUrlQuery, ExactReaderKeepsAnEqualsSignInsideTheValue) {
  const auto nested = exact("code=a=b");
  ASSERT_TRUE(nested);
  EXPECT_EQ(nested.value().view(), "a=b");
}

TEST_F(StrictUrlQuery, ExactReaderFindsAMissingOrMalformedValueUnusable) {
  for (const std::string_view query :
       {"", "state=1", "code=", "code=a+b", "code=a%2", "code=a%", "code=%zz",
        "code=a%20b", "code=a b", "code=a%00b", "code=a%7Fb", "code=a\xC3\xA9",
        "Code=abc"})
    EXPECT_TRUE(rejected_as(query, Fault::unusable));
}

TEST_F(StrictUrlQuery, ExactReaderFindsARepeatedOrValuelessKeyAmbiguous) {
  for (const std::string_view query :
       {"code", "code=a&code=b", "code=a&c%6Fde=b", "code=a&code"})
    EXPECT_TRUE(rejected_as(query, Fault::ambiguous));
}

TEST_F(StrictUrlQuery, ExactReaderReportsAnErrorKeyAsRefused) {
  for (const std::string_view query :
       {"error=access_denied", "error", "code=a&%65rror=x", "error=x&%zz=1"})
    EXPECT_TRUE(rejected_as(query, Fault::refused));
}

TEST_F(StrictUrlQuery, ExactReaderFindsAMalformedFieldUndecodable) {
  for (const std::string_view query :
       {"=value&code=a", "&code=a", "code=a&&state=1", "a+b=1&code=a",
        "a%2=1&code=a", "a%zz=1&code=a", "a%20b=1&code=a", "a b=1&code=a",
        "%zz=1&error=x", "code=a&k%C3%A9=1"})
    EXPECT_TRUE(rejected_as(query, Fault::undecodable));
}

TEST_F(StrictUrlQuery, ExactReaderAcceptsAValueAtBothLimits) {
  const auto fits = exact("code=abcd", small);
  ASSERT_TRUE(fits);
  EXPECT_EQ(fits.value().view(), "abcd");
  const auto escaped = exact("code=%41%42%43", small);
  ASSERT_TRUE(escaped);
  EXPECT_EQ(escaped.value().view(), "ABC");
}

TEST_F(StrictUrlQuery, ExactReaderFindsAValueOverEitherLimitUnusable) {
  const auto decoded_too_long = exact("code=abcde", small);
  ASSERT_FALSE(decoded_too_long);
  EXPECT_EQ(decoded_too_long.error(), Fault::unusable);
  const auto encoded_too_long = exact("code=%41%42%43a", small);
  ASSERT_FALSE(encoded_too_long);
  EXPECT_EQ(encoded_too_long.error(), Fault::unusable);
}

TEST_F(StrictUrlQuery, ExactReaderFindsAKeyOverEitherLimitUndecodable) {
  const auto key_too_long = exact("abcdefghij=1&code=a", small);
  ASSERT_FALSE(key_too_long);
  EXPECT_EQ(key_too_long.error(), Fault::undecodable);
  const auto key_decodes_too_long = exact("abcde=1&code=a", small);
  ASSERT_FALSE(key_decodes_too_long);
  EXPECT_EQ(key_decodes_too_long.error(), Fault::undecodable);
}

TEST_F(StrictUrlQuery, FirstMatchReaderTakesEachValueVerbatim) {
  const std::string_view location =
      "gaikai://"
      "local?state=1&code=v3.a%2Fb+c&cid=2#access_token=tok&expires_in=3600";
  EXPECT_EQ(first(location, "code", 64).view(), "v3.a%2Fb+c");
  EXPECT_EQ(first(location, "state", 64).view(), "1");
  EXPECT_EQ(first(location, "access_token", 64).view(), "tok");
  EXPECT_EQ(first(location, "expires_in", 64).view(), "3600");
}

TEST_F(StrictUrlQuery, FirstMatchReaderMatchesKeysInTheirOwnCase) {
  EXPECT_TRUE(first("Code=x", "code", 64).empty());
  EXPECT_TRUE(first("?CODE=x&c%6Fde=y", "code", 64).empty());
  EXPECT_EQ(first("?Code=x&code=y", "code", 64).view(), "y");
}

TEST_F(StrictUrlQuery, ExactReaderStopsAtTheFirstUndecodableKey) {
  for (const std::string_view query :
       {"code=a&%zz=1&error=x", "state=1&a%2=1&error", "%zz&code=a"})
    EXPECT_TRUE(rejected_as(query, Fault::undecodable));
}

TEST_F(StrictUrlQuery, FirstMatchReaderTakesTheFirstOfRepeatedKeys) {
  EXPECT_EQ(first("code=a&code=b", "code", 64).view(), "a");
  EXPECT_EQ(first("x?code=a&code=b", "code", 64).view(), "a");
  EXPECT_EQ(first("x?mycode=a&code=b", "code", 64).view(), "b");
}

TEST_F(StrictUrlQuery, FirstMatchReaderStopsAtALineBreak) {
  EXPECT_EQ(first("x?a=1\r\n", "a", 64).view(), "1");
  EXPECT_EQ(first("x?a=1\nb", "a", 64).view(), "1");
}

TEST_F(StrictUrlQuery, FirstMatchReaderTruncatesToTheCapacity) {
  EXPECT_EQ(first("x?code=abcdef", "code", 4).view(), "abc");
  EXPECT_EQ(first("x?code=abcdef", "code", 2).view(), "a");
}

TEST_F(StrictUrlQuery, FirstMatchReaderStopsAtAnEmbeddedNul) {
  EXPECT_TRUE(
      first(std::string_view{"x?a=1\0&code=z", 13}, "code", 64).empty());
}

TEST_F(StrictUrlQuery, FirstMatchReaderReturnsNothingWhenAbsent) {
  EXPECT_TRUE(first("x?code=abc", "", 64).empty());
  EXPECT_TRUE(first("x?code=abc", "code", 1).empty());
  EXPECT_TRUE(first("x?code=abc", "code", 0).empty());
  EXPECT_TRUE(first("x?code=abc", "state", 64).empty());
  EXPECT_TRUE(first("x?mycode=abc", "code", 64).empty());
  EXPECT_TRUE(first("x/code=abc", "code", 64).empty());
  EXPECT_TRUE(first("x?code", "code", 64).empty());
  EXPECT_TRUE(first("x?code=&a=1", "code", 64).empty());
  EXPECT_TRUE(first("x?c%6Fde=abc", "code", 64).empty());
  EXPECT_TRUE(first("", "code", 64).empty());
}

TEST_F(StrictUrlQuery, TheTwoReadersDifferOnTheseInputs) {
  struct Case {
    std::string_view query;
    std::string_view first_match;
    bool exact_accepts;
    std::string_view exact;
  };
  constexpr std::array cases{
      Case{"code=abc", "abc", true, "abc"},
      Case{"code=a%2Fb", "a%2Fb", true, "a/b"},
      Case{"c%6Fde=abc", "", true, "abc"},
      Case{"code=a+b", "a+b", false, ""},
      Case{"code=a&code=b", "a", false, ""},
      Case{"code=a%2", "a%2", false, ""},
      Case{"code=a b", "a b", false, ""},
      Case{"code=a\xC3\xA9", "a\xC3\xA9", false, ""},
      Case{"code=a#b", "a", true, "a#b"},
      Case{"code=a\rb", "a", false, ""},
      Case{"code=a&error=x", "a", false, ""},
      Case{"code=a&&b=1", "a", false, ""},
      Case{"code=a&b+c=1", "a", false, ""},
      Case{"x=1#code=a", "a", false, ""},
  };
  for (const auto& item : cases) {
    SCOPED_TRACE(item.query);
    EXPECT_EQ(first(item.query, "code", 64).view(), item.first_match);
    const auto exact_value = exact(item.query);
    EXPECT_EQ(exact_value.has_value(), item.exact_accepts);
    if (exact_value) {
      EXPECT_EQ(exact_value.value().view(), item.exact);
    }
  }
}

TEST_F(StrictUrlQuery, OnlyTheFirstMatchReaderTakesAValueOverTheLimit) {
  const std::string long_code(3000, 'a');
  const auto query = "code=" + long_code;
  EXPECT_EQ(first(query, "code", 4096).view(), long_code);
  EXPECT_FALSE(exact(query));
}

TEST_F(StrictUrlQuery, RedirectQueryRefusesAnAddressThatOnlySharesAPrefix) {
  const std::string cloud{libchiaking::psn::cloud_stream_redirect_uri};
  const std::string legacy{libchiaking::psn::legacy_cloud_stream_redirect_uri};
  for (const auto& [location, address] :
       {std::pair{cloud + ".evil?code=x", cloud},
        std::pair{cloud + "@evil?code=x", cloud},
        std::pair{cloud + ":1?code=x", cloud},
        std::pair{legacy + "x?code=x", legacy},
        std::pair{legacy + "/x?code=x", legacy}})
    EXPECT_FALSE(StrictUrl::redirect_query(location, address, 4096))
        << location;
}

TEST_F(StrictUrlQuery,
       RedirectQueryTakesTheQueryAfterTheRootOfAPathlessAddress) {
  const std::string cloud{libchiaking::psn::cloud_stream_redirect_uri};
  for (const auto& location : {cloud + "?code=x", cloud + "/?code=x"}) {
    const auto query = StrictUrl::redirect_query(location, cloud, 4096);
    ASSERT_TRUE(query) << location;
    EXPECT_EQ(query.value(), "code=x") << location;
  }
}

TEST_F(StrictUrlQuery, RedirectQueryRefusesAFragment) {
  const std::string cloud{libchiaking::psn::cloud_stream_redirect_uri};
  const std::string legacy{libchiaking::psn::legacy_cloud_stream_redirect_uri};
  for (const auto& [location, address] :
       {std::pair{cloud + "?code=x#state", cloud},
        std::pair{cloud + "/?code=x#", cloud},
        std::pair{legacy + "?code=x#state", legacy}}) {
    const auto query = StrictUrl::redirect_query(location, address, 4096);
    ASSERT_FALSE(query) << location;
    EXPECT_EQ(query.error(), StrictUrl::RedirectFault::elsewhere) << location;
  }
}

}  // namespace
