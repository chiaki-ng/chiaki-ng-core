#include "tests/support/enum_printing.hpp"

#include <libchiaking/consoles_types.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <type_traits>

namespace consoles_test {
namespace {

using libchiaking::consoles::Generation;
using libchiaking::consoles::InstalledTitle;

static_assert(!std::is_default_constructible_v<InstalledTitle>);

class InstalledTitleFacts : public ::testing::Test {
 protected:
  const InstalledTitle::Facts facts{
      "PPSA01234_00",  "PPSA01234",  "A Game", "https://image.example/a.png",
      Generation::ps5, std::nullopt, true};
};

TEST_F(InstalledTitleFacts, KeepsEveryFact) {
  const auto title = InstalledTitle::create(facts);
  ASSERT_TRUE(title);
  EXPECT_EQ(title->title_id(), "PPSA01234_00");
  EXPECT_EQ(title->start_id(), "PPSA01234");
  EXPECT_EQ(title->name(), "A Game");
  EXPECT_EQ(title->cover_url(), "https://image.example/a.png");
  EXPECT_EQ(title->platform(), Generation::ps5);
  EXPECT_FALSE(title->last_used());
  EXPECT_TRUE(title->concept_id().empty());
  EXPECT_TRUE(title->recently_played());
}

TEST_F(InstalledTitleFacts, KeepsAConceptAndTheLastUse) {
  auto grouped = facts;
  grouped.concept_id = "900002";
  grouped.last_used = InstalledTitle::LastUse{
      std::chrono::system_clock::time_point{std::chrono::seconds{5}},
      InstalledTitle::LastUse::Known::on_this_console};
  const auto known = InstalledTitle::create(grouped);
  ASSERT_TRUE(known);
  EXPECT_EQ(known->concept_id(), "900002");
  EXPECT_EQ(known->last_used(), grouped.last_used);
}

TEST_F(InstalledTitleFacts, RefusesAConceptThatIsNotShortDigits) {
  auto grouped = facts;
  grouped.concept_id = "22a";
  EXPECT_FALSE(InstalledTitle::create(grouped));
  grouped.concept_id = std::string(InstalledTitle::concept_limit + 1, '1');
  EXPECT_FALSE(InstalledTitle::create(grouped));
}

TEST_F(InstalledTitleFacts, NeedsATitleId) {
  auto missing = facts;
  missing.title_id.clear();
  EXPECT_FALSE(InstalledTitle::create(missing));
}

TEST_F(InstalledTitleFacts, RefusesAStartIdOverTheLimit) {
  auto missing = facts;
  missing.start_id = std::string(InstalledTitle::id_limit + 1, 'A');
  EXPECT_FALSE(InstalledTitle::create(missing));
}

TEST_F(InstalledTitleFacts, NeedsAName) {
  auto missing = facts;
  missing.name = "  ";
  EXPECT_FALSE(InstalledTitle::create(missing));
}

TEST_F(InstalledTitleFacts, NeedsACover) {
  auto missing = facts;
  missing.cover_url.clear();
  EXPECT_FALSE(InstalledTitle::create(missing));
}

}  // namespace
}  // namespace consoles_test
