#include "src/core/psn/gk/launch_spec.hpp"
#include "tests/support/utc_offset.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <string_view>

namespace cloud_test {
namespace {

namespace gk = libchiaking::psn::gk;

class GkLanguage : public ::testing::Test {
 protected:
  static constexpr std::array<std::string_view, 11> gk_locales{
      "en-US", "en-GB", "de-DE", "fr-FR", "fi-FI", "it-IT",
      "es-ES", "nl-NL", "pt-BR", "ja-JP", "ko-KR"};

  static auto keeps_the_language_of_every_locale() -> testing::AssertionResult {
    for (const auto locale : gk_locales) {
      const gk::Language language{locale};
      if (language.view() != locale.substr(0, 2))
        return testing::AssertionFailure()
               << locale << " became " << language.view();
    }
    return testing::AssertionSuccess();
  }

  static auto blank_languages_send_english() -> testing::AssertionResult {
    for (const auto blank : {std::string_view{}, std::string_view{"  \t "},
                             std::string_view{"\0en-US", 7}}) {
      const gk::Language language{blank};
      if (language.view() != "en")
        return testing::AssertionFailure()
               << "a blank language became " << language.view();
      const auto spec = gk::LaunchSpec::cloud_stream(
          "synthetic-entitlement", language,
          gk::TimeZone::parse("UTC+09:00").value(), 1080);
      if (!spec)
        return testing::AssertionFailure()
               << "a blank language refused the spec";
      if (spec.value().json().find("\"language\":\"en\",") ==
          std::string_view::npos)
        return testing::AssertionFailure()
               << "the spec does not send en: " << spec.value().json();
    }
    return testing::AssertionSuccess();
  }
};

TEST_F(GkLanguage, KeepsTheLowercasedLanguageSubtag) {
  EXPECT_EQ(gk::Language{"en-US"}.view(), "en");
  EXPECT_EQ(gk::Language{"EN_gb"}.view(), "en");
  EXPECT_EQ(gk::Language{"  \t eS-419"}.view(), "es");
  EXPECT_EQ(gk::Language{"   FR-fr"}.view(), "fr");
}

TEST_F(GkLanguage, FallsBackToEnglishWithoutALanguage) {
  EXPECT_EQ(gk::Language{""}.view(), "en");
  EXPECT_EQ(gk::Language{"  \t "}.view(), "en");
  EXPECT_EQ(gk::Language{"-FR"}.view(), "en");
  EXPECT_EQ(gk::Language{"_FR"}.view(), "en");
}

TEST_F(GkLanguage, KeepsAtMostFifteenCharacters) {
  EXPECT_EQ(gk::Language{"abcdefghijklmno"}.view(), "abcdefghijklmno");
  EXPECT_EQ(gk::Language{"abcdefghijklmnop"}.view(), "abcdefghijklmno");
}

TEST_F(GkLanguage, StopsAtAnEmbeddedNul) {
  const gk::Language german{std::string_view{"de\0-DE", 6}};
  EXPECT_EQ(german.view(), "de");
  const gk::Language leading{std::string_view{"\0en-US", 7}};
  EXPECT_EQ(leading.view(), "en");
}

TEST_F(GkLanguage, SendsEnglishInTheSpecForABlankLanguage) {
  EXPECT_TRUE(blank_languages_send_english());
}

TEST_F(GkLanguage, KeepsTheLanguageOfEveryGkLocale) {
  EXPECT_TRUE(keeps_the_language_of_every_locale());
}

TEST(GkTimeZone, SendsTheOffsetOfAnUnzonedName) {
  const test_support::FixedUtcOffset kolkata{std::chrono::minutes{330}};
  const auto unzoned = gk::LaunchSpec::legacy_cloud_stream(
      "synthetic-entitlement", gk::Language{"en-US"},
      gk::TimeZone{"Europe/London", kolkata}, 1080);
  ASSERT_TRUE(unzoned);
  EXPECT_NE(unzoned.value().json().find("\"timeZone\":\"UTC+05:30\""),
            std::string_view::npos);
}

}  // namespace
}  // namespace cloud_test
