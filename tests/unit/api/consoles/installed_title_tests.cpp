#include "tests/unit/api/consoles/titles_rig.hpp"

#include <gtest/gtest.h>

#include <string>

namespace api_consoles_test {
namespace {

using libchiaking::consoles::DeviceUid;
using libchiaking::consoles::InstalledTitle;
using libchiaking::consoles::StartState;

static_assert(InstalledTitle::id_limit == 64);
static_assert(DeviceUid::text_length == 64);
static_assert(static_cast<int>(StartState::pending) == 0);
static_assert(static_cast<int>(StartState::starting) == 1);
static_assert(static_cast<int>(StartState::completed) == 2);
static_assert(static_cast<int>(StartState::failed) == 3);

TEST(InstalledTitles, AcceptIdsUpToTheirLimit) {
  EXPECT_TRUE(TitlesRig::title(std::string(InstalledTitle::id_limit, 'P'),
                               "PPSA01234"));
}

TEST(InstalledTitles, RefuseIdsPastTheirLimit) {
  EXPECT_FALSE(TitlesRig::title(std::string(InstalledTitle::id_limit + 1, 'P'),
                                "PPSA01234"));
  EXPECT_FALSE(TitlesRig::title(
      "PPSA01234_00", std::string(InstalledTitle::id_limit + 1, 'P')));
}

TEST(InstalledTitles, RefuseEmptyIds) {
  EXPECT_FALSE(TitlesRig::title("", "PPSA01234"));
  EXPECT_FALSE(TitlesRig::title("PPSA01234_00", ""));
}

TEST(InstalledTitles, RefuseIdsHoldingNul) {
  EXPECT_FALSE(TitlesRig::title(std::string{"PPSA\00001_00", 10}, "PPSA01234"));
  EXPECT_FALSE(TitlesRig::title("PPSA01234_00", std::string{"PPSA\00001", 7}));
}

TEST(InstalledTitles, RefuseABlankNameOrAnEmptyCover) {
  EXPECT_FALSE(InstalledTitle::create(
      {"PPSA01234_00", "PPSA01234", " ", "https://img/g.png"}));
  EXPECT_FALSE(
      InstalledTitle::create({"PPSA01234_00", "PPSA01234", "Game", ""}));
}

TEST(DeviceUids, AreExactlySixtyFourCharactersWithoutNul) {
  EXPECT_FALSE(DeviceUid::parse(""));
  EXPECT_FALSE(DeviceUid::parse(std::string(65, 'f')));
  EXPECT_FALSE(DeviceUid::parse(std::string{"f1e2\0d3c4", 9}));
  EXPECT_TRUE(DeviceUid::parse(std::string(64, 'f')));
}

}  // namespace
}  // namespace api_consoles_test
