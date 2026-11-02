#include "src/core/utils/json/strict.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace json_test {
namespace {

namespace json = libchiaking::json::strict;

class StrictJsonObjectReader : public ::testing::Test {
 protected:
  void SetUp() override {
    auto parsed = json::Value::parse("{\"n\":7,\"s\":\"x\",\"a\":[],\"o\":{}}");
    ASSERT_TRUE(parsed);
    document_.emplace(std::move(parsed).value());
    ASSERT_NE(document_->object(), nullptr);
  }

  auto object() const -> const json::Value::Object& {
    return *document_->object();
  }

 private:
  std::optional<json::Value> document_;
};

TEST_F(StrictJsonObjectReader, FinishesOnceEveryMemberWasRead) {
  auto complete = json::ObjectReader::create(object());
  ASSERT_TRUE(complete);
  EXPECT_EQ(complete.value().integer("n"), 7U);
  const auto text = complete.value().string("s");
  ASSERT_TRUE(text);
  EXPECT_EQ(*text, "x");
  EXPECT_TRUE(complete.value().array("a"));
  EXPECT_TRUE(complete.value().object("o"));
  EXPECT_TRUE(complete.value().finish());
}

TEST_F(StrictJsonObjectReader, RefusesToFinishWithUnreadMembers) {
  auto partial = json::ObjectReader::create(object());
  ASSERT_TRUE(partial);
  EXPECT_EQ(partial.value().integer("n"), 7U);
  EXPECT_FALSE(partial.value().finish());
  for (std::size_t index = 0; index < object().size(); ++index)
    EXPECT_EQ(partial.value().integer("n"), 7U);
  EXPECT_FALSE(partial.value().finish());
}

TEST_F(StrictJsonObjectReader, FailsAMistypedReadButStillReadsTheRightType) {
  auto mistyped = json::ObjectReader::create(object());
  ASSERT_TRUE(mistyped);
  EXPECT_FALSE(mistyped.value().string("n"));
  EXPECT_FALSE(mistyped.value().integer("s"));
  EXPECT_FALSE(mistyped.value().array("o"));
  EXPECT_FALSE(mistyped.value().object("a"));
  EXPECT_FALSE(mistyped.value().finish());
  EXPECT_EQ(mistyped.value().integer("n"), 7U);
  EXPECT_TRUE(mistyped.value().string("s"));
  EXPECT_TRUE(mistyped.value().array("a"));
  EXPECT_TRUE(mistyped.value().object("o"));
  EXPECT_TRUE(mistyped.value().finish());
}

TEST_F(StrictJsonObjectReader, FailsAMissingMember) {
  auto missing = json::ObjectReader::create(object());
  ASSERT_TRUE(missing);
  EXPECT_FALSE(missing.value().integer("absent"));
  EXPECT_FALSE(missing.value().finish());
}

TEST_F(StrictJsonObjectReader, RefusesUnsortedDuplicateOrExcessiveMembers) {
  json::Value::Object unsorted;
  unsorted.push_back({"b", json::Value{std::uint64_t{1}}});
  unsorted.push_back({"a", json::Value{std::uint64_t{2}}});
  EXPECT_FALSE(json::ObjectReader::create(unsorted));
  json::Value::Object duplicate;
  duplicate.push_back({"a", json::Value{std::uint64_t{1}}});
  duplicate.push_back({"a", json::Value{std::uint64_t{2}}});
  EXPECT_FALSE(json::ObjectReader::create(duplicate));
  json::Value::Object excessive;
  for (std::size_t index = 0; index <= json::max_members; ++index)
    excessive.push_back(
        {"k" + std::to_string(index), json::Value{std::uint64_t{1}}});
  EXPECT_FALSE(json::ObjectReader::create(excessive));
}

TEST_F(StrictJsonObjectReader, FinishesAnEmptyObjectAtOnce) {
  auto empty = json::ObjectReader::create({});
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty.value().finish());
}

}  // namespace
}  // namespace json_test
