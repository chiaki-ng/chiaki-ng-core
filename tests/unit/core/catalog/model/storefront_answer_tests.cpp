#include "tests/unit/core/catalog/model/value_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <optional>
#include <span>

namespace catalog_test {
namespace {

using libchiaking::catalog::StorefrontAnswer;
using Streaming = StorefrontAnswer::Streaming;

class StorefrontAnswers : public ValueCases {
 protected:
  ProductId asked = product("UP0112-PPSA01234_00-GAME000000000000-E001");
  StorefrontAnswer::LibraryOffer offer{"SKU-1", {"ENT-1"}, false};
  std::array<StorefrontAnswer::Product, 2> answered{
      StorefrontAnswer::Product{
          product("EP0100-PPSA01234_00-FAKE0086TITLE000"), true, {}, true, {}},
      StorefrontAnswer::Product{product("UP0112-PPSA01234_00-GAME000000000000"),
                                true,
                                {},
                                false,
                                offer},
  };
};

TEST_F(StorefrontAnswers, CountTheEditionThatAnswersTheProduct) {
  const auto streams =
      StorefrontAnswer::of(account, asked, answered, at_ms(1000));
  EXPECT_TRUE(streams.says_it_streams());
  EXPECT_EQ(streams.streaming(), Streaming::streams);
  EXPECT_EQ(streams.answered_product(),
            product("UP0112-PPSA01234_00-GAME000000000000"));
  EXPECT_EQ(streams.account(), account);
  EXPECT_EQ(streams.product_id(), asked);
  EXPECT_EQ(streams.checked_at(), at_ms(1000));
  EXPECT_FALSE(streams.owned_per_store());
  EXPECT_EQ(streams.library_offer(), offer);
  EXPECT_TRUE(streams.offers_library_add());
}

TEST_F(StorefrontAnswers, AreFreshForTheirLifetimeFromTheCheck) {
  const auto streams =
      StorefrontAnswer::of(account, asked, answered, at_ms(1000));
  EXPECT_TRUE(streams.is_fresh_at(at_ms(1000)));
  EXPECT_FALSE(streams.is_fresh_at(at_ms(999)));
  EXPECT_FALSE(streams.is_fresh_at(
      at_ms(1000) + std::chrono::duration_cast<std::chrono::milliseconds>(
                        StorefrontAnswer::lifetime)));
}

TEST_F(StorefrontAnswers, MissTheEditionWhenOnlyAnotherIsAnswered) {
  const auto other_edition = StorefrontAnswer::of(
      account, asked, std::span{answered}.first(1), at_ms(1000));
  EXPECT_EQ(other_edition.streaming(), Streaming::edition_missing);
  EXPECT_FALSE(other_edition.says_it_streams());
  EXPECT_FALSE(other_edition.answered_product());
  EXPECT_FALSE(other_edition.owned_per_store());
  EXPECT_FALSE(other_edition.library_offer());
  EXPECT_EQ(StorefrontAnswer::of(account, asked, {}, at_ms(1000)).streaming(),
            Streaming::edition_missing);
}

TEST_F(StorefrontAnswers, KeepTheNoticeOfAnUnstreamableEdition) {
  const std::array refused{StorefrontAnswer::Product{
      asked, false, "Not available on PS5 streaming", true, {}}};
  const auto not_streamable =
      StorefrontAnswer::of(account, asked, refused, at_ms(1000));
  EXPECT_EQ(not_streamable.streaming(), Streaming::not_streamable);
  EXPECT_EQ(not_streamable.notice(), "Not available on PS5 streaming");
  EXPECT_TRUE(not_streamable.owned_per_store());
  EXPECT_FALSE(not_streamable.offers_library_add());
}

TEST_F(StorefrontAnswers, IgnoreANoticeWithoutAStreamingFlag) {
  const std::array silent{
      StorefrontAnswer::Product{asked, std::nullopt, "ignored", false, {}}};
  const auto no_notice =
      StorefrontAnswer::of(account, asked, silent, at_ms(1000));
  EXPECT_EQ(no_notice.streaming(), Streaming::no_notice);
  EXPECT_TRUE(no_notice.notice().empty());
}

}  // namespace
}  // namespace catalog_test
