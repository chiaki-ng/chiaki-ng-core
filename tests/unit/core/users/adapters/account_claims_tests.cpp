#include "src/core/users/adapters/account_claims.hpp"
#include "src/core/utils/text/base64.hpp"

#include <libchiaking/detail/users/account_context.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <span>
#include <string>
#include <string_view>

namespace users_test {
namespace {

using libchiaking::users::AccountClaims;
using libchiaking::users::AccountContext;

class AccountClaimsRead : public ::testing::Test {
 protected:
  static auto token_carrying(std::string_view claims) -> std::string {
    const auto bytes = std::as_bytes(std::span{claims.data(), claims.size()});
    return "eyJhbGciOiJub25lIn0." +
           libchiaking::text::base64::encode(
               bytes, libchiaking::text::base64::Alphabet::url) +
           ".signature";
  }

  static auto stated_by(std::string_view claims) -> AccountContext {
    return AccountClaims::read(token_carrying(claims));
  }

  static auto claim(std::string_view name, std::string_view value)
      -> AccountContext {
    return stated_by(R"({")" + std::string{name} + R"(":)" +
                     std::string{value} + "}");
  }
};

TEST_F(AccountClaimsRead, TakesTheClaimsOfASignInToken) {
  const auto context = stated_by(
      R"({"sub":"1","age":34,"legal_country":"GB","locale":"en-GB","is_child":false,"iat":1900000000,"birthdate":"1990-01-01"})");
  EXPECT_EQ(context.legal_country(), "GB");
  EXPECT_EQ(context.locale(), "en-GB");
  EXPECT_EQ(context.language(), "en");
  EXPECT_EQ(context.age(), 34);
  EXPECT_EQ(context.is_child(), false);
  EXPECT_EQ(context.issued_at(),
            AccountContext::Time{std::chrono::seconds{1'900'000'000}});
  EXPECT_TRUE(context.names_a_catalogue_audience());
}

TEST_F(AccountClaimsRead, NeverTakesTheLegalCountryFromTheLocale) {
  const auto abroad =
      stated_by(R"({"age":30,"legal_country":"gb","locale":"en-US"})");
  EXPECT_EQ(abroad.legal_country(), "GB");
  EXPECT_EQ(abroad.locale(), "en-US");
  EXPECT_EQ(abroad.language(), "en");

  const auto unstated = stated_by(R"({"age":30,"locale":"en-GB"})");
  EXPECT_FALSE(unstated.legal_country());
  EXPECT_EQ(unstated.locale(), "en-GB");
  EXPECT_FALSE(unstated.names_a_catalogue_audience());

  const auto bare =
      stated_by(R"({"age":30,"legal_country":"JP","locale":"ja"})");
  EXPECT_EQ(bare.locale(), "ja");
  EXPECT_EQ(bare.language(), "ja");
}

TEST_F(AccountClaimsRead, LeavesAMissingAgeUnknownAndTakesZeroAsAnAge) {
  const auto unstated = stated_by(R"({"legal_country":"GB","locale":"en-GB"})");
  EXPECT_FALSE(unstated.age());
  EXPECT_FALSE(unstated.names_a_catalogue_audience());
  const auto newborn =
      stated_by(R"({"age":0,"legal_country":"GB","locale":"en-GB"})");
  EXPECT_EQ(newborn.age(), 0);
  EXPECT_TRUE(newborn.names_a_catalogue_audience());
  EXPECT_EQ(stated_by(R"({"age":150})").age(), 150);
}

TEST_F(AccountClaimsRead, StatesNothingForMalformedClaims) {
  for (const std::string_view age :
       {R"("34")", "34.5", "true", "-1", "151", "null", "[34]"})
    EXPECT_FALSE(claim("age", age).age()) << age;
  for (const std::string_view country :
       {"44", R"("GBR")", R"("G")", R"("G1")", R"("")", "null"})
    EXPECT_FALSE(claim("legal_country", country).legal_country()) << country;
  for (const std::string_view locale :
       {"1", R"("e")", R"("english")", R"("en-")", R"("en_GB")",
        R"("en-GB-and-far-too-long")", R"("e1-GB")"})
    EXPECT_FALSE(claim("locale", locale).locale()) << locale;
  for (const std::string_view child : {R"("false")", "0", "null"})
    EXPECT_FALSE(claim("is_child", child).is_child()) << child;
  EXPECT_FALSE(stated_by(R"({"age":34})").is_child());
}

TEST_F(AccountClaimsRead, RestoresAKeptContextUnderTheSameRules) {
  const auto kept = stated_by(
      R"({"age":34,"legal_country":"GB","locale":"en-GB","is_child":false,"iat":1900000000})");
  ASSERT_TRUE(kept.legal_country());
  ASSERT_TRUE(kept.locale());
  ASSERT_TRUE(kept.age());
  const std::string country = *kept.legal_country();
  const std::string locale = *kept.locale();
  EXPECT_EQ(AccountContext::stating({.legal_country = country,
                                     .locale = locale,
                                     .age = *kept.age(),
                                     .is_child = kept.is_child(),
                                     .issued_at = kept.issued_at()}),
            kept);
  const auto damaged = AccountContext::stating(
      {.legal_country = "GBR", .locale = "english", .age = 900});
  EXPECT_EQ(damaged, AccountContext{});
  EXPECT_EQ(AccountContext::stating({}), AccountContext{});
}

TEST_F(AccountClaimsRead, StatesNothingForAnUnreadableToken) {
  for (const std::string_view unreadable :
       {"", "app", "a.b", "a.!!!.c", "a..c"})
    EXPECT_EQ(AccountClaims::read(unreadable), AccountContext{}) << unreadable;
  EXPECT_EQ(stated_by("[1,2]"), AccountContext{});
  EXPECT_EQ(stated_by("not json"), AccountContext{});
  EXPECT_EQ(
      AccountClaims::read(std::string(AccountClaims::token_limit + 1, 'a')),
      AccountContext{});
}

}  // namespace
}  // namespace users_test
