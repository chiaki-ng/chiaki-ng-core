#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/rule_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <string>

namespace catalog_test {
namespace {

using libchiaking::catalog::Platform;
using libchiaking::catalog::ProductId;
using libchiaking::catalog::TitleKey;
using libchiaking::catalog::TitleNumber;

class ProductIds : public ::testing::Test, protected RuleCases {};

TEST_F(ProductIds, RefuseMalformedText) {
  EXPECT_FALSE(ProductId::parse(""));
  EXPECT_FALSE(ProductId::parse("EP0109 PPSA07028"));
  EXPECT_FALSE(ProductId::parse("EP0109-PPSA07028_00-RIVET/../GAME"));
  EXPECT_FALSE(ProductId::parse(std::string(ProductId::limit + 1, 'A')));
  EXPECT_TRUE(ProductId::parse(std::string(ProductId::limit, 'A')));
}

TEST_F(ProductIds, KnowTheirTitle) {
  const auto rivet = product("EP0109-PPSA07028_00-FAKE0110TITLE000");
  EXPECT_EQ(rivet.text(), "EP0109-PPSA07028_00-FAKE0110TITLE000");
  EXPECT_EQ(rivet.title_number(), *TitleNumber::from("PPSA07028"));
  EXPECT_EQ(rivet.title_key(), *TitleKey::of("PPSA07028"));
}

TEST_F(ProductIds, KnowTheirPlatform) {
  EXPECT_EQ(product("EP0109-PPSA07028_00-FAKE0110TITLE000").platform(),
            Platform::ps5);
  EXPECT_EQ(product("UP0114-CUSA07009_00-FAKE0051TITLE000").platform(),
            Platform::ps4);
  EXPECT_FALSE(product("UP0114-NPUA87001_00-FAKE0084TITLE000").platform());
  EXPECT_FALSE(product("UP0114-NPUA87001_00-FAKE0084TITLE000").title_number());
}

TEST_F(ProductIds, CompareByText) {
  const auto rivet = product("EP0109-PPSA07028_00-FAKE0110TITLE000");
  EXPECT_EQ(rivet, product("EP0109-PPSA07028_00-FAKE0110TITLE000"));
  EXPECT_NE(rivet, product("EP0109-PPSA07028_00-FAKE0111TITLE000"));
}

TEST_F(ProductIds, KnowTheirSku) {
  const auto with_sku = product("UP0112-PPSA01234_00-GAME000000000000-E001");
  const auto base = product("UP0112-PPSA01234_00-GAME000000000000");
  EXPECT_TRUE(with_sku.has_sku());
  EXPECT_FALSE(base.has_sku());
  EXPECT_EQ(with_sku.without_sku(), base);
  EXPECT_EQ(base.without_sku(), base);
}

TEST_F(ProductIds, TakeOnlyAnUppercaseLetterAndThreeDigitsAsASku) {
  EXPECT_FALSE(product("UP0112-PPSA01234_00-GAME000000000000-e001").has_sku());
  EXPECT_FALSE(product("UP0112-PPSA01234_00-GAME000000000000-E01").has_sku());
  EXPECT_FALSE(
      product("UP0112-PPSA01234_00-GAME000000000000-EDITION").has_sku());
  EXPECT_FALSE(product("A-B-E001").has_sku());
}

TEST_F(ProductIds, AreAnsweredByTheSameProductOrItsBase) {
  const auto base = product("UP0112-PPSA01234_00-GAME000000000000");
  const auto with_sku = product("UP0112-PPSA01234_00-GAME000000000000-E001");
  EXPECT_TRUE(base.is_answered_by(base));
  EXPECT_TRUE(with_sku.is_answered_by(base));
  EXPECT_TRUE(with_sku.is_answered_by(with_sku));
  EXPECT_FALSE(base.is_answered_by(with_sku));
  EXPECT_TRUE(product("UP0112-PPSA01234_00-GAME000000000000-DELUXE")
                  .is_answered_by(base));
}

TEST_F(ProductIds, AreNotAnsweredByAnotherProductOfTheSameTitle) {
  const auto base = product("UP0112-PPSA01234_00-GAME000000000000");
  const auto with_sku = product("UP0112-PPSA01234_00-GAME000000000000-E001");
  const auto other_product = product("EP0100-PPSA01234_00-FAKE0086TITLE000");
  const auto longer_label = product("UP0112-PPSA01234_00-GAME0000000000001");
  EXPECT_EQ(base.title_number(), other_product.title_number());
  EXPECT_FALSE(base.is_answered_by(other_product));
  EXPECT_FALSE(with_sku.is_answered_by(other_product));
  EXPECT_FALSE(longer_label.is_answered_by(base));
}

}  // namespace
}  // namespace catalog_test
