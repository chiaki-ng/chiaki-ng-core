#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/utils/json/wire_dialect_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace json_test {
namespace {

using Dialect = WireDialectRig::Dialect;
using ParseError = WireDialectRig::ParseError;

TEST(WireDialectCanonicalText, AcceptsWhatTheWireSends) {
  for (const std::string_view input :
       {"{}", "[]", "trueX", "falseX", "nullX", "true/", "null{", "{\"n\":04}",
        "1 x", "{\"a\":1,\"a\":2}", "\xEF\xBB\xBF{\"a\":1}", "\"a\tb\"",
        "\"\xFF\"", R"("\uD800x")",
        "[null,true,false,-0,0,9223372036854775808,1.25e300]"})
    EXPECT_TRUE(Dialect::canonical_text(input)) << input;
}

TEST(WireDialectCanonicalText, RefusesTrailingTextAndCommasInsideAValue) {
  EXPECT_TRUE(WireDialectRig::rejected("[trueX]"));
  EXPECT_TRUE(WireDialectRig::rejected("[1,]"));
}

TEST(WireDialectCanonicalText, TakesOnlyJsonWhiteSpaceBetweenTokens) {
  EXPECT_EQ(WireDialectRig::canonical(" \t\r\n{ \t\r\n\"a\" :\n1\r\n}"),
            R"({"a":1})");
  for (const std::string_view input :
       {"{\v\"a\":1}", "{\"a\":\f1}", "[1,\v2]", "[1\f]", "\v{}", "\f[]"})
    EXPECT_TRUE(WireDialectRig::rejected(input)) << input;
}

TEST(WireDialectCanonicalText, DropsALeadingByteOrderMarkOnly) {
  EXPECT_EQ(WireDialectRig::canonical("\xEF\xBB\xBF{\"a\":1}"), R"({"a":1})");
  EXPECT_TRUE(WireDialectRig::rejected("[\xEF\xBB\xBFnull]"));
}

TEST(WireDialectCanonicalText, StopsAtTheEndOfTheFirstValue) {
  EXPECT_EQ(WireDialectRig::canonical("trueX"), "true");
}

TEST(WireDialectCanonicalText, WritesUnicodeAndSlashesUnescaped) {
  EXPECT_EQ(WireDialectRig::canonical(R"(["😀","é","\/"])"),
            "[\"\xF0\x9F\x98\x80\",\"\xC3\xA9\",\"/\"]");
}

TEST(WireDialectCanonicalText, ProjectsEmptyValuesAndLeadingZeros) {
  EXPECT_EQ(
      WireDialectRig::canonical(
          R"({"connRequest":{"localPeerAddr":},"n":004,"a":[true,null,"text"]})"),
      R"({"connRequest":{"localPeerAddr":{}},"n":4,"a":[true,null,"text"]})");
  EXPECT_EQ(WireDialectRig::canonical("\"\xFF\""), "\"\xFF\"");
  EXPECT_TRUE(WireDialectRig::rejected("[1}"));
}

TEST(WireDialectCanonicalText, FollowsThePsnDefaults) {
  EXPECT_EQ(WireDialectRig::canonical(
                R"({"connRequest":{"localPeerAddr":},"n":004}])"),
            R"({"connRequest":{"localPeerAddr":{}},"n":4})");
  EXPECT_EQ(WireDialectRig::canonical(R"({"a":1,"a":2})"), R"({"a":2})");
  EXPECT_EQ(WireDialectRig::canonical("[\"a\tb\"]"), R"(["a\tb"])");
  EXPECT_EQ(WireDialectRig::canonical(R"(["\uD800x","\uDC00"])"),
            "[\"\xEF\xBF\xBDx\",\"\xEF\xBF\xBD\"]");
  EXPECT_EQ(WireDialectRig::canonical("[\"\xC3\x28\"]"), "[\"\xC3\x28\"]");
  EXPECT_TRUE(WireDialectRig::rejected("[1}"));
  EXPECT_TRUE(WireDialectRig::rejected(R"({"x":})"));
}

TEST(WireDialectCanonicalText, FillsAMissingPeerAddressOnlyWhereItBelongs) {
  EXPECT_EQ(
      WireDialectRig::canonical(
          R"({"action":"OFFER","reqId":2,"error":0,"connRequest":{"localPeerAddr":,"localHashedId":"test"}})"),
      R"({"action":"OFFER","reqId":2,"error":0,"connRequest":{"localPeerAddr":{},"localHashedId":"test"}})");
  for (const auto input : {R"({"connRequest":{"localPeerAddr":null}})",
                           R"({"connRequest":{"localPeerAddr":{}}})",
                           R"({"connRequest":{"localPeerAddr":""}})",
                           R"({"text":"\"localPeerAddr\":,"})"})
    EXPECT_TRUE(Dialect::canonical_text(input)) << input;
  EXPECT_TRUE(WireDialectRig::rejected(R"({"localPeerAddr":})"));
  EXPECT_TRUE(WireDialectRig::rejected(R"({"other":{"localPeerAddr":}})"));
  EXPECT_TRUE(WireDialectRig::rejected(R"({"connRequest":{"localPeerAddr":)"));
  EXPECT_TRUE(WireDialectRig::rejected(R"({"x":,})"));
  EXPECT_TRUE(Dialect::canonical_text(R"({"connRequest":{"localPeerAddr":}})"));
}

TEST(WireDialectCanonicalText, RefusesBrokenStringsAndEscapesARawNul) {
  for (const auto input :
       {R"(["\x00"])", R"(["\uXXXX"])", "[\"unterminated", "[\"a\\"})
    EXPECT_TRUE(WireDialectRig::rejected(input)) << input;
  const std::string raw_nul{"[\"a\0b\"]", 7};
  EXPECT_EQ(WireDialectRig::canonical(raw_nul), R"(["a\u0000b"])");
}

TEST(WireDialectCanonicalText, ReadsLenientNumbers) {
  EXPECT_EQ(WireDialectRig::canonical("[04,-007,00.5,0.05e01]"),
            "[4,-7,0.5,0.05e01]");
  for (const auto input : {"-", "1.", "1e", "1e+", "--1", "1+2", "01e-"})
    EXPECT_TRUE(WireDialectRig::rejected(input)) << input;
  EXPECT_TRUE(
      Dialect::canonical_text("18446744073709551616000000000000000000"));
}

TEST(WireDialectCanonicalText, LimitsDepthAndSize) {
  constexpr std::size_t maximum_depth = 64;
  constexpr std::size_t maximum_bytes = 4 * 1024 * 1024;
  const std::string deepest =
      std::string(maximum_depth, '[') + std::string(maximum_depth, ']');
  EXPECT_TRUE(Dialect::canonical_text(deepest));
  EXPECT_TRUE(WireDialectRig::rejected(std::string(maximum_depth + 1, '['),
                                       ParseError::too_deep));
  EXPECT_TRUE(WireDialectRig::rejected(std::string(maximum_bytes + 1, ' '),
                                       ParseError::too_large));
}

TEST(WireDialectRepresentableText, RefusesNumbersThatOverflowADouble) {
  EXPECT_TRUE(Dialect::canonical_text("[1e400]"));
  auto overflow = Dialect::representable_text("{\"x\":[1e400]}");
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error(), ParseError::invalid_json);
  auto big = Dialect::representable_text(
      "[18446744073709551616000000000000000000,1e-400]");
  ASSERT_TRUE(big);
  EXPECT_EQ(big.value().view(),
            "[18446744073709551616000000000000000000,1e-400]");
}

TEST(WireDialectConcealedText, HidesTextUnderConcealedKeys) {
  const auto conceals = [](std::string_view key) { return key == "token"; };
  auto hidden = Dialect::concealed_text(
      R"({"token":"T","n":{"token":"U","keep":"v"},"list":[{"token":1}]})",
      conceals, "[x]");
  ASSERT_TRUE(hidden);
  EXPECT_EQ(
      hidden.value().view(),
      R"({"token":"[x]","n":{"token":"[x]","keep":"v"},"list":[{"token":1}]})");
}

}  // namespace
}  // namespace json_test
