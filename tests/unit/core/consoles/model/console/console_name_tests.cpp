#include "tests/support/enum_printing.hpp"

#include <libchiaking/consoles_types.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace consoles_test {
namespace {

using libchiaking::consoles::ConsoleName;
using libchiaking::consoles::Generation;
using Source = ConsoleName::Source;

static_assert(!std::is_default_constructible_v<ConsoleName>);

class ConsoleNamePrecedence : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(renamed);
    ASSERT_TRUE(discovered);
    ASSERT_TRUE(pairing);
    ASSERT_TRUE(account);
  }

  const std::optional<ConsoleName> renamed =
      ConsoleName::create("Den", Source::renamed);
  const std::optional<ConsoleName> discovered =
      ConsoleName::create("PS5-123", Source::discovered);
  const std::optional<ConsoleName> pairing =
      ConsoleName::create("Nick", Source::pairing);
  const std::optional<ConsoleName> account =
      ConsoleName::create("PS's", Source::account);
  const ConsoleName fallback = ConsoleName::of(Generation::ps4);
};

TEST_F(ConsoleNamePrecedence, FallsBackToTheGeneration) {
  EXPECT_EQ(fallback.text(), "PS4");
  EXPECT_EQ(fallback.source(), Source::generation);
  EXPECT_EQ(ConsoleName::of(Generation::ps5).text(), "PS5");
}

TEST_F(ConsoleNamePrecedence, PutsARenameAboveADiscovery) {
  EXPECT_TRUE(renamed->replaces(*discovered));
  EXPECT_FALSE(discovered->replaces(*renamed));
}

TEST_F(ConsoleNamePrecedence, PutsADiscoveryAboveAPairing) {
  EXPECT_TRUE(discovered->replaces(*pairing));
  EXPECT_FALSE(pairing->replaces(*discovered));
}

TEST_F(ConsoleNamePrecedence, PutsAPairingAboveTheAccount) {
  EXPECT_TRUE(pairing->replaces(*account));
  EXPECT_FALSE(account->replaces(*pairing));
}

TEST_F(ConsoleNamePrecedence, PutsTheAccountAboveTheGeneration) {
  EXPECT_TRUE(account->replaces(fallback));
  EXPECT_FALSE(fallback.replaces(*account));
}

TEST_F(ConsoleNamePrecedence, LetsANewerNameReplaceOneFromTheSameSource) {
  const auto older = ConsoleName::create("Older", Source::discovered);
  ASSERT_TRUE(older);
  EXPECT_TRUE(discovered->replaces(*older));
}

TEST(ConsoleName, RefusesABlankUnprintableOrOverlongName) {
  EXPECT_FALSE(ConsoleName::create("", Source::renamed));
  EXPECT_FALSE(ConsoleName::create("   ", Source::renamed));
  EXPECT_FALSE(ConsoleName::create("a\tb", Source::renamed));
  EXPECT_FALSE(ConsoleName::create(std::string(ConsoleName::limit + 1, 'n'),
                                   Source::renamed));
}

TEST(ConsoleName, IsNeverCreatedFromTheGeneration) {
  EXPECT_FALSE(ConsoleName::create("PS5", Source::generation));
}

TEST(ConsoleName, AcceptsANameAtTheLimitAndUtf8) {
  EXPECT_TRUE(ConsoleName::create(std::string(ConsoleName::limit, 'n'),
                                  Source::renamed));
  EXPECT_TRUE(ConsoleName::create("Salle \xc3\xa0 manger", Source::renamed));
}

}  // namespace
}  // namespace consoles_test
