#include "src/core/psn/gk/language.hpp"

#include <gtest/gtest.h>

namespace cloud_api_test {
namespace {

using libchiaking::psn::gk::Language;

TEST(GkLanguage, TrimsARegionalLocaleToItsLanguage) {
  EXPECT_EQ(Language{"  pt-BR"}.view(), "pt");
}

TEST(GkLanguage, FallsBackToEnglishForAnEmptyLocale) {
  EXPECT_EQ(Language{""}.view(), "en");
}

TEST(GkLanguage, ReadsAnUnderscoredLocale) {
  EXPECT_EQ(Language{"ja_JP"}.view(), "ja");
}

}  // namespace
}  // namespace cloud_api_test
