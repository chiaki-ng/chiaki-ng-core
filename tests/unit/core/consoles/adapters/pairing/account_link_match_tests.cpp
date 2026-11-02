#include "src/core/consoles/adapters/pairing/account_link.hpp"
#include "tests/support/enum_printing.hpp"

#include <libchiaking/consoles_types.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace consoles_test {
namespace {

using libchiaking::consoles::AccountLink;
using libchiaking::consoles::DeviceUid;
using Match = AccountLink::Match;
using Matches = std::vector<Match>;
using Rule = AccountLink::Rule;
using Saved = AccountLink::Saved;
using Listed = AccountLink::Listed;

class AccountLinkMatch : public ::testing::Test {
 protected:
  static auto uid(char digit) -> DeviceUid {
    return DeviceUid::parse(std::string(64, digit)).value();
  }

  static auto matched(const std::vector<Saved>& saved,
                      const std::vector<Listed>& listed) -> Matches {
    return AccountLink::matched(saved, listed);
  }
};

TEST_F(AccountLinkMatch, PrefersAStoredDeviceUidOverTheName) {
  EXPECT_EQ(matched({{"Den", uid('a')}},
                    {{"Other", uid('b')}, {"Renamed", uid('a')}}),
            (Matches{{0, 1, Rule::stored_device_uid}}));
  EXPECT_EQ(matched({{"Den", uid('a')}, {"Attic", uid('b')}},
                    {{"Attic", uid('a')}, {"Den", uid('b')}}),
            (Matches{{0, 0, Rule::stored_device_uid},
                     {1, 1, Rule::stored_device_uid}}));
}

TEST_F(AccountLinkMatch, LinksNothingForAStoredUidThatIsNotListed) {
  EXPECT_TRUE(matched({{"Den", uid('a')}}, {{"Den", uid('b')}}).empty());
  EXPECT_TRUE(matched({{"Den", uid('a')}}, {{"Den", std::nullopt}}).empty());
}

TEST_F(AccountLinkMatch, LinksNothingForASharedUid) {
  EXPECT_TRUE(
      matched({{"Den", uid('a')}, {"Attic", uid('a')}}, {{"Den", uid('a')}})
          .empty());
  EXPECT_TRUE(
      matched({{"Den", uid('a')}}, {{"One", uid('a')}, {"Two", uid('a')}})
          .empty());
}

TEST_F(AccountLinkMatch, LinksAUniqueNameWhenThereIsNoUid) {
  EXPECT_EQ(matched({{" den ", std::nullopt}},
                    {{"Attic", uid('b')}, {"DEN", uid('a')}}),
            (Matches{{0, 1, Rule::unique_name}}));
  EXPECT_EQ(
      matched({{"Den", uid('a')}, {"Attic", std::nullopt}},
              {{"Attic", uid('b')}, {"Other", uid('a')}}),
      (Matches{{0, 1, Rule::stored_device_uid}, {1, 0, Rule::unique_name}}));
  EXPECT_EQ(matched({{"Den", std::nullopt}},
                    {{"Den", std::nullopt}, {"Attic", uid('b')}}),
            (Matches{{0, 0, Rule::unique_name}}));
}

TEST_F(AccountLinkMatch, LinksNothingWhenNoNameMatches) {
  EXPECT_TRUE(matched({{"Den", std::nullopt}},
                      {{"Attic", uid('a')}, {"Kitchen", uid('b')}})
                  .empty());
  EXPECT_TRUE(matched({{"  ", std::nullopt}, {"Attic", std::nullopt}},
                      {{"", uid('a')}, {"Kitchen", uid('b')}})
                  .empty());
}

TEST_F(AccountLinkMatch, LinksNothingForAnAmbiguousName) {
  EXPECT_TRUE(
      matched({{"Den", std::nullopt}}, {{"Den", uid('a')}, {"den", uid('b')}})
          .empty());
  EXPECT_TRUE(matched({{"Den", std::nullopt}, {"DEN ", std::nullopt}},
                      {{"Den", uid('a')}, {"Attic", uid('b')}})
                  .empty());
}

TEST_F(AccountLinkMatch, StillLinksTheUnambiguousNames) {
  EXPECT_EQ(matched({{"Den", std::nullopt},
                     {"Den", std::nullopt},
                     {"Attic", std::nullopt}},
                    {{"Den", uid('a')}, {"Attic", uid('b')}}),
            (Matches{{2, 1, Rule::unique_name}}));
  EXPECT_EQ(
      matched({{"Den", uid('a')}, {"Den", std::nullopt}},
              {{"Den", uid('a')}, {"Den", uid('b')}}),
      (Matches{{0, 0, Rule::stored_device_uid}, {1, 1, Rule::unique_name}}));
}

TEST_F(AccountLinkMatch, LinksTheSoleConsoleWhenNothingElseDoes) {
  EXPECT_EQ(matched({{"Den", std::nullopt}}, {{"Living Room", uid('a')}}),
            (Matches{{0, 0, Rule::sole_console}}));
  EXPECT_EQ(matched({{"Den", std::nullopt}}, {{"Den", uid('a')}}),
            (Matches{{0, 0, Rule::unique_name}}));
}

TEST_F(AccountLinkMatch, LinksNoSoleConsoleUnlessBothSidesHoldOne) {
  EXPECT_TRUE(
      matched({{"Den", uid('b')}}, {{"Living Room", uid('a')}}).empty());
  EXPECT_TRUE(matched({{"Den", std::nullopt}},
                      {{"Living Room", uid('a')}, {"Attic", uid('b')}})
                  .empty());
  EXPECT_TRUE(matched({{"Den", std::nullopt}, {"Attic", std::nullopt}},
                      {{"Living Room", uid('a')}})
                  .empty());
  EXPECT_TRUE(matched({}, {{"Living Room", uid('a')}}).empty());
  EXPECT_TRUE(matched({{"Den", std::nullopt}}, {}).empty());
}

}  // namespace
}  // namespace consoles_test
