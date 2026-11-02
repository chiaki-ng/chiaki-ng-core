#include "tests/unit/core/catalog/model/value_cases.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

namespace catalog_test {
namespace {

using libchiaking::catalog::NotAGame;
using libchiaking::catalog::OwnershipAnswer;

class OwnershipAnswers : public ValueCases {};

TEST_F(OwnershipAnswers, CarryTheOwnedGame) {
  const auto rift = owned_rivet();
  const auto answer = OwnershipAnswer::owned(rift);
  EXPECT_TRUE(answer.is_owned());
  ASSERT_TRUE(answer.game());
  EXPECT_EQ(*answer.game(), rift);
}

TEST_F(OwnershipAnswers, CarryNoGameWhenNotOwned) {
  EXPECT_EQ(OwnershipAnswer::not_owned().kind(),
            OwnershipAnswer::Kind::not_owned);
  EXPECT_FALSE(OwnershipAnswer::not_owned().game());
}

TEST_F(OwnershipAnswers, SayWhyAProductIsNotAGame) {
  const auto add_on = OwnershipAnswer::not_a_game(NotAGame::add_on);
  EXPECT_FALSE(add_on.is_owned());
  EXPECT_EQ(add_on.why_not_a_game(), NotAGame::add_on);
}

}  // namespace
}  // namespace catalog_test
