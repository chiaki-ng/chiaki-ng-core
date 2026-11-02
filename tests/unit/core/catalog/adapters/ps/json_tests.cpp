#include "src/core/utils/json/json.hpp"
#include "src/core/utils/json/ps_reply.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace catalog_test::ps_json_shapes {

struct Spelled {
  std::string text;
  std::int64_t number{};
  std::optional<bool> absent;
};

struct Numbers {
  std::vector<std::int64_t> values;
};

struct Held {
  libchiaking::SecretString access_token;
  std::optional<libchiaking::SecretString> nul;
};

}  // namespace catalog_test::ps_json_shapes

namespace catalog_test {
namespace {

using libchiaking::json::ParseError;
using ps_json_shapes::Held;
using ps_json_shapes::Numbers;
using ps_json_shapes::Spelled;

class PsJsonWrite : public ::testing::Test {
 protected:
  static auto spelled(std::string_view text) -> std::string {
    const auto written = libchiaking::json::ps::write<
        libchiaking::json::ps::body_options_with_nulls>(
        Spelled{std::string{text}, -7, {}});
    EXPECT_TRUE(written) << text;
    return std::string{written.value().view()};
  }
};

TEST_F(PsJsonWrite, IsCompact) {
  EXPECT_EQ(spelled("https://a.example/x"),
            R"({"text":"https://a.example/x","number":-7,"absent":null})");
}

TEST_F(PsJsonWrite, EscapesQuotesAndBackslashes) {
  EXPECT_EQ(spelled("a\"b\\c"),
            R"({"text":"a\"b\\c","number":-7,"absent":null})");
  EXPECT_EQ(spelled("\\u001F"),
            R"({"text":"\\u001F","number":-7,"absent":null})");
}

TEST_F(PsJsonWrite, EscapesControlCharacters) {
  EXPECT_EQ(spelled("\b\f\n\r\t\x01\x1f\x7f"),
            "{\"text\":\"\\b\\f\\n\\r\\t\\u0001\\u001F\x7f\",\"number\":-7,"
            "\"absent\":null}");
}

TEST_F(PsJsonWrite, KeepsUtf8AsItIs) {
  EXPECT_EQ(spelled("Fable\xC2\xAE \xE2\x84\xA2"),
            "{\"text\":\"Fable\xC2\xAE "
            "\xE2\x84\xA2\",\"number\":-7,\"absent\":null}");
}

TEST_F(PsJsonWrite, RefusesBrokenUtf8) {
  EXPECT_FALSE(libchiaking::json::ps::write(
      Spelled{std::string{"\xFF\xC3(\xE0\x80"}, -7, {}}));
}

TEST(PsJsonRead, RefusesEmptyText) {
  EXPECT_EQ(libchiaking::json::read<Numbers>("", 100).error(),
            ParseError::invalid_json);
}

TEST(PsJsonRead, HonoursTheSizeLimit) {
  EXPECT_EQ(
      libchiaking::json::read<Numbers>(R"({"values":[1,2,3]})", 3).error(),
      ParseError::too_large);
  EXPECT_TRUE(libchiaking::json::read<Numbers>(R"({"values":[1,2,3]})", 18));
}

TEST(PsJsonRead, HonoursTheDepthLimit) {
  std::string nested(64, '[');
  nested.append(64, ']');
  EXPECT_TRUE(libchiaking::json::read<glz::raw_json>(nested));
  std::string deep(66, '[');
  deep.append(66, ']');
  EXPECT_EQ(libchiaking::json::read<glz::raw_json>(deep).error(),
            ParseError::too_deep);
}

TEST(PsJsonRead, IsStrict) {
  EXPECT_EQ(libchiaking::json::read<Numbers>(R"({"values":[1]}x)").error(),
            ParseError::invalid_json);
  EXPECT_EQ(libchiaking::json::read<Numbers>(R"({"values":[04]})").error(),
            ParseError::invalid_json);
  EXPECT_EQ(libchiaking::json::read<Numbers>(R"({"values":"no"})").error(),
            ParseError::unexpected_shape);
  EXPECT_EQ(
      libchiaking::json::read<Numbers>(R"({"other":[04],"values":[]})").error(),
      ParseError::invalid_json);
}

TEST(PsJsonRead, StopsAtANulTerminator) {
  const auto nul_terminated = libchiaking::json::read<Numbers>(
      std::string_view{"{\"values\":[1]}\0garbage", 24});
  ASSERT_TRUE(nul_terminated);
  EXPECT_EQ(nul_terminated.value().values.size(), 1U);
}

TEST(PsJsonRead, StopsAtTheEndOfTheView) {
  constexpr std::string_view backing = "[123]{\"values\":[1]}x";
  EXPECT_FALSE(libchiaking::json::ps::read_reply<std::vector<int>>(
      backing.substr(0, 3)));
  const auto object = libchiaking::json::read<Numbers>(backing.substr(5, 13));
  ASSERT_FALSE(object);
  EXPECT_EQ(object.error(), ParseError::invalid_json);
  EXPECT_FALSE(libchiaking::json::well_formed(backing.substr(0, 3)));
  EXPECT_TRUE(libchiaking::json::well_formed(backing.substr(0, 5)));
}

TEST(PsJsonRead, ReadsAWholeDocumentOfOneFieldType) {
  namespace ps = libchiaking::json::ps;
  const auto secret = ps::read_reply<libchiaking::SecretString>(R"("aAb")");
  ASSERT_TRUE(secret);
  EXPECT_EQ(secret.value().view(), "aAb");
  const auto text = ps::read_reply<ps::Text>(R"("2020-01-01")");
  ASSERT_TRUE(text);
  EXPECT_EQ(text.value().view(), "2020-01-01");
  const auto number = ps::read_reply<ps::Number>("12");
  ASSERT_TRUE(number);
  EXPECT_EQ(number.value().integer(), 12);
  const auto tolerant = ps::read_reply<ps::Tolerant<std::string>>(R"("x")");
  ASSERT_TRUE(tolerant);
  ASSERT_TRUE(tolerant.value());
  EXPECT_EQ(*tolerant.value(), "x");
}

TEST(PsJsonRead, ReadsSecretsIntoWipingStrings) {
  const auto held = libchiaking::json::read<Held>(
      R"({"access_token":"TOKEN","n":1,"nul":"a\u0000b"})");
  ASSERT_TRUE(held);
  EXPECT_EQ(held.value().access_token.view(), "TOKEN");
  ASSERT_TRUE(held.value().nul);
  EXPECT_EQ(held.value().nul->size(), 3U);
  EXPECT_FALSE(libchiaking::json::read<Held>(R"({"access_token":1})"));
}

}  // namespace
}  // namespace catalog_test
