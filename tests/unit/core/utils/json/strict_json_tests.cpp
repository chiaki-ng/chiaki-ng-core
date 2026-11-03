#include "src/core/utils/json/strict.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace json_test {
namespace {

namespace strict = libchiaking::json::strict;

class StrictJson : public ::testing::Test {
 protected:
  static auto accepts(std::string_view text, std::string_view canonical)
      -> ::testing::AssertionResult {
    const auto parsed = strict::Value::parse(text);
    if (!parsed) return ::testing::AssertionFailure() << "rejected " << text;
    if (parsed.value().canonical().value() != canonical)
      return ::testing::AssertionFailure()
             << text << " became " << parsed.value().canonical().value()
             << " instead of " << canonical;
    return ::testing::AssertionSuccess();
  }

  static auto rejects(std::string_view text) -> ::testing::AssertionResult {
    if (strict::Value::parse(text))
      return ::testing::AssertionFailure() << "accepted " << text;
    return ::testing::AssertionSuccess();
  }
};

TEST_F(StrictJson, WritesTheCanonicalLayout) {
  EXPECT_TRUE(accepts("{ \"b\" : 2 ,\r\n\t\"a\":[1, \"x\"] }",
                      "{\"a\":[1,\"x\"],\"b\":2}"));
  EXPECT_TRUE(accepts("{}", "{}"));
  EXPECT_TRUE(accepts("[]", "[]"));
  EXPECT_TRUE(accepts("0", "0"));
  EXPECT_TRUE(accepts("9007199254740991", "9007199254740991"));
  EXPECT_TRUE(accepts("\"\\u0041\\n\\\"\\\\\\/\"", "\"A\\n\\\"\\\\/\""));
  EXPECT_TRUE(accepts("\"\\u0001\\u001f\\b\\f\\r\\t\"",
                      "\"\\u0001\\u001f\\b\\f\\r\\t\""));
  EXPECT_TRUE(
      accepts("{\"B\":1,\"a\":2,\"A\":3}", "{\"A\":3,\"B\":1,\"a\":2}"));
}

TEST_F(StrictJson, TakesOnlyJsonWhiteSpaceBetweenTokens) {
  EXPECT_TRUE(accepts(" \t\r\n{ \"a\" :\t1 }\r\n", "{\"a\":1}"));
  for (const std::string_view input :
       {"{\v\"a\":1}", "{\"a\":\f1}", "[1,\v2]", "{\"a\":1}\v", "\f{}"})
    EXPECT_TRUE(rejects(input)) << input;
}

TEST_F(StrictJson, RejectsValuesOutsideTheDialect) {
  EXPECT_TRUE(rejects(""));
  EXPECT_TRUE(rejects("{\"a\":1,\"a\":2}"));
  EXPECT_TRUE(rejects("{\"a\":1.0}"));
  EXPECT_TRUE(rejects("{\"a\":1e3}"));
  EXPECT_TRUE(rejects("{\"a\":-1}"));
  EXPECT_TRUE(rejects("{\"a\":true}"));
  EXPECT_TRUE(rejects("{\"a\":false}"));
  EXPECT_TRUE(rejects("{\"a\":null}"));
  EXPECT_TRUE(rejects("{\"a\":01}"));
  EXPECT_TRUE(rejects("9007199254740992"));
  EXPECT_TRUE(rejects("99999999999999999999999"));
  EXPECT_TRUE(rejects("{\"a\":1} x"));
  EXPECT_TRUE(rejects("{\"a\":1,}"));
  EXPECT_TRUE(rejects("[1,]"));
  EXPECT_TRUE(rejects("{\"a\" 1}"));
  EXPECT_TRUE(rejects("\"unterminated"));
  EXPECT_TRUE(rejects("\"raw\ncontrol\""));
  EXPECT_TRUE(rejects("\"\\u00e9\""));
  EXPECT_TRUE(rejects("\"\\ud83d\""));
  EXPECT_TRUE(rejects("\"\\ud83d\\ude00\""));
  EXPECT_TRUE(rejects("\"\\x41\""));
  EXPECT_TRUE(rejects(std::string_view{"\"\xc3\xa9\"", 4}));
  EXPECT_TRUE(rejects(std::string_view{"\"\xff\"", 3}));
}

TEST_F(StrictJson, LimitsNestingToEightLevels) {
  std::string nested;
  for (int level = 0; level < 8; ++level) nested.push_back('[');
  for (int level = 0; level < 8; ++level) nested.push_back(']');
  EXPECT_TRUE(strict::Value::parse(nested));
  EXPECT_TRUE(rejects("[" + nested + "]"));

  std::string objects;
  for (int level = 0; level < 9; ++level) objects.append("{\"a\":");
  objects.push_back('1');
  for (int level = 0; level < 9; ++level) objects.push_back('}');
  EXPECT_TRUE(rejects(objects));
}

TEST_F(StrictJson, LimitsTheMembersOfAnArray) {
  std::string array{"["};
  for (std::size_t index = 0; index < strict::max_members; ++index) {
    if (index != 0) array.push_back(',');
    array.push_back('1');
  }
  EXPECT_TRUE(strict::Value::parse(array + "]"));
  EXPECT_TRUE(rejects(array + ",1]"));
}

TEST_F(StrictJson, WritesASecretOnlyIntoWipingText) {
  strict::Value::Object members;
  members.push_back(
      {"key", strict::Value{libchiaking::SecretString{"hidden"}}});
  members.push_back({"n", strict::Value{std::uint64_t{1}}});
  const strict::Value document{std::move(members)};
  EXPECT_FALSE(document.canonical());
  EXPECT_EQ(document.canonical_secret().view(), R"({"key":"hidden","n":1})");
}

TEST_F(StrictJson, ClonesEverySecretOfAValue) {
  strict::Value::Array items;
  items.push_back(strict::Value{libchiaking::SecretString{"hidden"}});
  const strict::Value original{std::move(items)};
  const auto copy = original.clone();
  ASSERT_NE(copy.array(), nullptr);
  const auto* secret = copy.array()->front().secret();
  ASSERT_NE(secret, nullptr);
  EXPECT_EQ(secret->view(), "hidden");
  EXPECT_NE(secret->data(), original.array()->front().secret()->data());
}

}  // namespace
}  // namespace json_test
