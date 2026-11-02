#include "libchiaking/catalog_types.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <string_view>
#include <type_traits>

namespace text_test {
namespace {

class StrongText : public ::testing::Test {
 protected:
  struct LowercaseWords {
    static auto admits(std::string_view text) noexcept -> bool {
      return !text.empty() &&
             text.find_first_not_of("abcdefghijklmnopqrstuvwxyz") ==
                 std::string_view::npos;
    }
  };

  struct Nickname;
  struct Hostname;
  using NicknameText = libchiaking::StrongText<Nickname, LowercaseWords>;
  using HostnameText = libchiaking::StrongText<Hostname, LowercaseWords>;
};

TEST_F(StrongText, WrapsOnlyAdmittedText) {
  const auto name = NicknameText::parse("kai");
  ASSERT_TRUE(name);
  EXPECT_EQ(name->text(), "kai");
  EXPECT_EQ(*name, "kai");
  EXPECT_FALSE(NicknameText::parse(""));
  EXPECT_FALSE(NicknameText::parse("Kai"));
  EXPECT_FALSE(NicknameText::parse("k i"));
}

TEST_F(StrongText, ComparesAndOrdersByItsText) {
  const auto alpha = *NicknameText::parse("alpha");
  const auto beta = *NicknameText::parse("beta");
  EXPECT_EQ(alpha, *NicknameText::parse("alpha"));
  EXPECT_NE(alpha, beta);
  EXPECT_LT(alpha, beta);
  const std::set<NicknameText> names{beta, alpha, alpha};
  ASSERT_EQ(names.size(), 2U);
  EXPECT_EQ(names.begin()->text(), "alpha");
}

TEST_F(StrongText, KeepsKindsSharingARuleApart) {
  static_assert(!std::is_convertible_v<NicknameText, HostnameText>);
  static_assert(!std::is_constructible_v<NicknameText, std::string_view>);
  static_assert(!std::is_default_constructible_v<NicknameText>);
}

TEST(ProductIdText, KeepsItsRule) {
  using libchiaking::catalog::ProductId;
  const auto id = ProductId::parse("UP0112-PPSA01234_00-GAMENAME0000000-E001");
  ASSERT_TRUE(id);
  EXPECT_EQ(id->text(), "UP0112-PPSA01234_00-GAMENAME0000000-E001");
  EXPECT_TRUE(id->has_sku());
  EXPECT_EQ(id->without_sku(), "UP0112-PPSA01234_00-GAMENAME0000000");
  EXPECT_FALSE(ProductId::parse(""));
  EXPECT_FALSE(ProductId::parse("bad id"));
  EXPECT_FALSE(ProductId::parse(std::string(ProductId::limit + 1, 'a')));
  EXPECT_TRUE(ProductId::parse(std::string(ProductId::limit, 'a')));
}

TEST(ConceptIdText, KeepsItsRule) {
  using libchiaking::catalog::ConceptId;
  const auto padded = ConceptId::parse("0042");
  ASSERT_TRUE(padded);
  EXPECT_EQ(padded->text(), "0042");
  EXPECT_FALSE(ConceptId::parse("000"));
  EXPECT_FALSE(ConceptId::parse(""));
  EXPECT_FALSE(ConceptId::parse("12a"));
  EXPECT_FALSE(ConceptId::parse(std::string(ConceptId::limit + 1, '1')));
  const auto ten = ConceptId::of(10);
  ASSERT_TRUE(ten);
  EXPECT_EQ(ten->text(), "10");
  EXPECT_FALSE(ConceptId::of(0));
  EXPECT_NE(*ConceptId::parse("42"), *ConceptId::parse("042"));
}

}  // namespace
}  // namespace text_test
