#include <libchiaking/platform/platform.hpp>

#include <gtest/gtest.h>

#include <string>
#include <type_traits>

namespace {

using libchiaking::platform::RecordName;

class RecordNames : public ::testing::Test {
 protected:
  [[nodiscard]] static auto named(std::string_view text) -> RecordName {
    return RecordName::parse(text).value();
  }
};

TEST_F(RecordNames, CannotBeDefaultConstructed) {
  EXPECT_FALSE(std::is_default_constructible_v<RecordName>);
}

TEST_F(RecordNames, AcceptLowercaseSlashSeparatedSegments) {
  for (const char* valid : {"library", "user/42/library", "streamable/gb-en",
                            "included/en-gb", "a_b-c/0"}) {
    const auto name = RecordName::parse(valid);
    ASSERT_TRUE(name) << valid;
    EXPECT_EQ(name->text(), valid);
  }
}

TEST_F(RecordNames, RefuseUnsafeOrReservedText) {
  for (const char* invalid :
       {"", "/", "user/", "/user", "user//library", "User/42", "user/4 2", "..",
        "user/..", "user/../other", "user/.hidden", "a.b", "user\\42", "c:/x",
        "nul", "user/con/library", "com1", "user/lpt9", "caf\xc3\xa9"})
    EXPECT_FALSE(RecordName::parse(invalid)) << '"' << invalid << '"';
}

TEST_F(RecordNames, RespectTheSegmentAndTotalLimits) {
  EXPECT_FALSE(
      RecordName::parse(std::string(RecordName::segment_limit + 1, 'a')));
  EXPECT_TRUE(RecordName::parse(std::string(RecordName::segment_limit, 'a')));
  EXPECT_FALSE(RecordName::parse(std::string(RecordName::limit, 'a')));
}

TEST_F(RecordNames, AreUnderThemselvesAndTheirAncestorsOnly) {
  const auto user = named("user/42");
  EXPECT_TRUE(named("user/42/library").is_under(user));
  EXPECT_TRUE(user.is_under(user));
  EXPECT_FALSE(named("user/421/library").is_under(user));
  EXPECT_FALSE(named("user/4").is_under(user));
  EXPECT_FALSE(user.is_under(named("user/42/library")));
}

TEST_F(RecordNames, GrowByOneValidSegment) {
  const auto user = named("user/42");
  const auto library = user.child("library");
  ASSERT_TRUE(library);
  EXPECT_EQ(*library, named("user/42/library"));
  EXPECT_FALSE(user.child(""));
  EXPECT_FALSE(user.child("a/b"));
  EXPECT_FALSE(user.child(".."));
}

TEST_F(RecordNames, OrderByTextAndAllowNamesThatOnlyStartReserved) {
  EXPECT_LT(named("a/b"), named("a/c"));
  EXPECT_EQ(named("com10").text(), "com10");
}

}  // namespace
