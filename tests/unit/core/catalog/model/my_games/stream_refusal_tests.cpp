#include "tests/unit/core/catalog/model/value_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <optional>

namespace catalog_test {
namespace {

using libchiaking::catalog::OwnershipId;
using libchiaking::catalog::StreamRefusal;
using libchiaking::users::Tier;
using Holding = StreamRefusal::Holding;

class StreamRefusals : public ValueCases {
 protected:
  Holding held = Holding::held_as(*OwnershipId::parse(show));
  StreamRefusal::Circumstances then{Tier::premium, held};
  StreamRefusal refusal{account, product(show),
                        StreamRefusal::Kind::no_game_for_entitlement,
                        at_ms(5000), then};
  std::chrono::milliseconds day =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          StreamRefusal::lifetime);
};

class StreamRefusalHolding : public StreamRefusals {};

TEST_F(StreamRefusals, BelongToOneUserAndTitle) {
  EXPECT_EQ(refusal.account(), account);
  EXPECT_EQ(refusal.product_id(), product(show));
  EXPECT_TRUE(refusal.questions_ownership());
}

TEST_F(StreamRefusals, StandForOneDay) {
  EXPECT_EQ(refusal.expires_at(), at_ms(5000) + day);
  EXPECT_TRUE(refusal.still_stands(at_ms(5000), then));
  EXPECT_TRUE(refusal.still_stands(at_ms(5000) + day - day / 24, then));
  EXPECT_FALSE(refusal.still_stands(at_ms(5000) + day, then));
  EXPECT_FALSE(refusal.still_stands(at_ms(4999), then));
}

TEST_F(StreamRefusals, LapseWhenTheTierChangesOrIsUnknown) {
  EXPECT_FALSE(refusal.still_stands(at_ms(6000), {Tier::extra, held}));
  EXPECT_FALSE(refusal.still_stands(at_ms(6000), {std::nullopt, held}));
}

TEST_F(StreamRefusals, LapseWhenTheHoldingChanges) {
  EXPECT_FALSE(
      refusal.still_stands(at_ms(6000), {Tier::premium, Holding::not_held()}));
  EXPECT_FALSE(refusal.still_stands(
      at_ms(6000),
      {Tier::premium, Holding::held_as(*OwnershipId::parse(rivet))}));
  EXPECT_TRUE(
      refusal.still_stands(at_ms(6000), {Tier::premium, Holding::unknown()}));
}

TEST_F(StreamRefusals, MadeWithoutKnowingTheHoldingStandWhateverIsHeld) {
  const StreamRefusal unsure{account,
                             product(show),
                             StreamRefusal::Kind::refused_by_stream_server,
                             at_ms(5000),
                             {Tier::premium, Holding::unknown()}};
  EXPECT_TRUE(unsure.still_stands(at_ms(6000), then));
  EXPECT_TRUE(unsure.still_stands(at_ms(6000), {Tier::premium, {}}));
  EXPECT_FALSE(unsure.still_stands(at_ms(6000), {Tier::extra, held}));
}

TEST_F(StreamRefusals, ByTheServerDoNotQuestionOwnership) {
  const StreamRefusal server{account, product(show),
                             StreamRefusal::Kind::refused_by_stream_server,
                             at_ms(5000), then};
  EXPECT_FALSE(server.questions_ownership());
  EXPECT_NE(server, refusal);
}

TEST_F(StreamRefusalHolding, NamesWhatIsHeld) {
  EXPECT_TRUE(held.is_known());
  EXPECT_TRUE(held.is_held());
  ASSERT_TRUE(held.id());
  EXPECT_EQ(held.id()->text(), show);
  EXPECT_EQ(held.state(), Holding::State::held);
}

TEST_F(StreamRefusalHolding, KnowsWhenNothingIsHeld) {
  EXPECT_TRUE(Holding::not_held().is_known());
  EXPECT_FALSE(Holding::not_held().is_held());
  EXPECT_FALSE(Holding::not_held().id());
}

TEST_F(StreamRefusalHolding, IsUnknownByDefault) {
  EXPECT_FALSE(Holding{}.is_known());
  EXPECT_EQ(Holding{}, Holding::unknown());
  EXPECT_NE(Holding::unknown(), Holding::not_held());
}

TEST_F(StreamRefusalHolding, ChangesOnlyBetweenKnownHoldings) {
  EXPECT_TRUE(held.has_changed_to(Holding::not_held()));
  EXPECT_TRUE(Holding::not_held().has_changed_to(held));
  EXPECT_FALSE(held.has_changed_to(held));
  EXPECT_FALSE(held.has_changed_to(Holding::unknown()));
  EXPECT_FALSE(Holding::unknown().has_changed_to(held));
  EXPECT_FALSE(Holding::unknown().has_changed_to(Holding::not_held()));
}

}  // namespace
}  // namespace catalog_test
