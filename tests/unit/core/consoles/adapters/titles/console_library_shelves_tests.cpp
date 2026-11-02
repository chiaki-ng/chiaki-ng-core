#include "tests/unit/core/consoles/adapters/titles/console_library_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace consoles_test {
namespace {

using libchiaking::consoles::ConsoleLibrary;
using libchiaking::consoles::Generation;
using Names = LibraryRig::Names;

class ConsoleLibraryShelved : public ::testing::Test {
 protected:
  static auto played(std::int64_t ms) { return LibraryRig::played_ms(ms); }

  void SetUp() override {
    metadata["PPSA00001_00"] = {"PPSA00001_00", "Shared", "https://1",
                                "ps5_native_game", played(5)};
    metadata["PPSA00002_00"] = {"PPSA00002_00", "Media", "https://2",
                                "ps5_native_media_app", played(4)};
    metadata["PPSA00003_00"] = {"PPSA00003_00", "Shouty", "https://3",
                                " PS4_VIDEO_SERVICE_WEB_APP ", played(4)};
    metadata["PPSA07001_00"] = {"PPSA07001_00", " ", "https://4",
                                "ps5_native_game", played(4)};
    metadata["PPSA07002_00"] = {"PPSA07002_00", "No art", "", "ps5_native_game",
                                played(4)};
    metadata["CUSA07003_00"] = {"CUSA07003_00", "Old game", "https://6",
                                "ps4_game", played(3)};
    metadata["PPSA07004_00"] = {"PPSA07004_00", "Mystery", "https://7",
                                "unknown", played(2)};
    metadata["PPSA07005_00"] = {"PPSA07005_00", "Uncategorised", "https://8",
                                "", played(1)};
    metadata["PPSA07014_00"] = {"PPSA07014_00", "Not installed", "https://99",
                                "ps5_native_game", played(9)};
    joined = ConsoleLibrary::shelved(consoles, metadata, {"PPSA00001_00"});
    ASSERT_EQ(joined.size(), 3U);
  }

  std::vector<ConsoleLibrary::ListedConsole> consoles{
      {"D1",
       "One",
       {{"PPSA00001_00", "PS5"},
        {"PPSA00002_00", "PS5"},
        {"PPSA00003_00", "PS5"},
        {"PPSA07001_00", "PS5"},
        {"PPSA07002_00", "PS5"},
        {"CUSA07003_00", "PS4"},
        {"PPSA07004_00", "PS5"},
        {"PPSA07005_00", ""},
        {"PPSA07006_00", "PS5"}}},
      {"D2", "Two", {{"PPSA00001_00", "PS5"}}},
      {"D3", "Three", {}}};
  ConsoleLibrary::PlayedTitles metadata;
  std::vector<ConsoleLibrary::Shelf> joined;
};

TEST_F(ConsoleLibraryShelved, KeepOnlyNamedCoveredGamesInstalledHere) {
  EXPECT_EQ(LibraryRig::names(joined[0].titles),
            (Names{"Shared", "Mystery", "Old game", "Uncategorised"}));
}

TEST_F(ConsoleLibraryShelved, KeepEachTitlesPlatformAndStartId) {
  ASSERT_EQ(joined[0].titles.size(), 4U);
  EXPECT_EQ(joined[0].titles[2].platform(), Generation::ps4);
  EXPECT_EQ(joined[0].titles[1].platform(), Generation::ps5);
  EXPECT_EQ(joined[0].titles[3].platform(), Generation::ps5);
  EXPECT_EQ(joined[0].titles[2].start_id(), "CUSA07003");
}

TEST_F(ConsoleLibraryShelved, MarkOnlyTheRecentTitles) {
  ASSERT_EQ(joined[0].titles.size(), 4U);
  EXPECT_TRUE(joined[0].titles[0].recently_played());
  EXPECT_FALSE(joined[0].titles[1].recently_played());
}

TEST_F(ConsoleLibraryShelved, ShareATitleAcrossConsoles) {
  ASSERT_EQ(joined[1].titles.size(), 1U);
  EXPECT_EQ(joined[1].titles[0].name(), "Shared");
  EXPECT_TRUE(joined[2].titles.empty());
  EXPECT_EQ(joined[2].duid, "D3");
}

TEST(ConsoleLibraryShelfOrder, SortsRecentByTimeThenTheRestByName) {
  std::vector<ConsoleLibrary::ListedConsole> consoles{{"D",
                                                       "",
                                                       {{"A", ""},
                                                        {"B", ""},
                                                        {"C", ""},
                                                        {"D", ""},
                                                        {"E", ""},
                                                        {"F", ""},
                                                        {"G", ""}}}};
  ConsoleLibrary::PlayedTitles metadata;
  metadata["A"] = {"A", "zeta", "i", "", LibraryRig::played_ms(100)};
  metadata["B"] = {"B", "Alpha", "i", "", LibraryRig::played_ms(300)};
  metadata["C"] = {"C", "beta", "i", "", LibraryRig::played_ms(300)};
  metadata["D"] = {"D", "delta", "i", "", LibraryRig::played_ms(999)};
  metadata["E"] = {"E", "Charlie", "i", "", LibraryRig::played_ms(5000)};
  metadata["F"] = {"F", "alpha", "i", "", LibraryRig::played_ms(1)};
  metadata["G"] = {"G", "Bravo", "i", "", LibraryRig::played_ms(0)};
  const auto joined =
      ConsoleLibrary::shelved(consoles, metadata, {"A", "B", "C", "D"});
  ASSERT_FALSE(joined.empty());
  std::vector<std::string> order;
  for (const auto& row : joined[0].titles) order.push_back(row.title_id());
  EXPECT_EQ(order,
            (std::vector<std::string>{"D", "B", "C", "A", "F", "G", "E"}));
}

TEST(ConsoleLibraryStartId, StripsOneSuffix) {
  EXPECT_EQ(ConsoleLibrary::start_id("PPSA01234_00"), "PPSA01234");
  EXPECT_EQ(ConsoleLibrary::start_id(" PPSA01234_00 "), "PPSA01234");
  EXPECT_EQ(ConsoleLibrary::start_id("PPSA01234"), "PPSA01234");
  EXPECT_EQ(ConsoleLibrary::start_id("CUSA12345_00_00"), "CUSA12345_00");
  EXPECT_EQ(ConsoleLibrary::start_id("PPSA01234_01"), "PPSA01234_01");
  EXPECT_TRUE(ConsoleLibrary::start_id("_00").empty());
  EXPECT_EQ(ConsoleLibrary::title_key(" ppsa01234_00\t"), "PPSA01234_00");
}

TEST(ConsoleLibraryPlayedAt, ReadsTheIsoVariants) {
  EXPECT_EQ(ConsoleLibrary::played_at("2026-01-01T18:00:00Z"),
            LibraryRig::played_ms(1767290400000));
  EXPECT_EQ(ConsoleLibrary::played_at("2026-01-01T18:00:00.123Z"),
            LibraryRig::played_ms(1767290400123));
  EXPECT_EQ(ConsoleLibrary::played_at("2026-01-01T18:00:00.5Z"),
            LibraryRig::played_ms(1767290400500));
  EXPECT_EQ(ConsoleLibrary::played_at("2026-01-01T18:00:00.123456789Z"),
            LibraryRig::played_ms(1767290400123));
  EXPECT_EQ(ConsoleLibrary::played_at(" 2026-01-01T12:00:00.000Z "),
            LibraryRig::played_ms(1767268800000));
  EXPECT_EQ(ConsoleLibrary::played_at("2026-01-01T19:00:00+01:00"),
            LibraryRig::played_ms(1767290400000));
  EXPECT_EQ(ConsoleLibrary::played_at("2026-01-01t18:00:00z"),
            LibraryRig::played_ms(1767290400000));
  EXPECT_EQ(ConsoleLibrary::played_at("2024-02-29T23:59:59Z"),
            LibraryRig::played_ms(1709251199000));
  EXPECT_EQ(ConsoleLibrary::played_at("1970-01-01T00:00:01Z"),
            LibraryRig::played_ms(1000));
}

TEST(ConsoleLibraryPlayedAt, RefusesAnythingElse) {
  for (const std::string_view garbage :
       {"", "garbage", "2026-01-01", "2026-01-01T18:00:00",
        "2026-13-01T18:00:00Z", "2025-02-29T18:00:00Z", "2026-01-01T24:00:00Z",
        "2026-01-01T18:00:00.Z", "2026-01-01T18:00:00Zjunk",
        "2026/01/01T18:00:00Z", "2026-01-01T18:00:00+0100",
        "1969-12-31T23:59:59Z", "1970-01-01T00:00:00Z"})
    EXPECT_FALSE(ConsoleLibrary::played_at(garbage)) << garbage;
}

TEST(ConsoleLibraryCategory, IsAnAppOnlyWithTheAppSuffix) {
  EXPECT_TRUE(ConsoleLibrary::is_app("ps5_native_media_app"));
  EXPECT_TRUE(ConsoleLibrary::is_app(" PS4_APP "));
  EXPECT_FALSE(ConsoleLibrary::is_app("ps5_native_game"));
  EXPECT_FALSE(ConsoleLibrary::is_app("unknown"));
  EXPECT_FALSE(ConsoleLibrary::is_app(""));
  EXPECT_FALSE(ConsoleLibrary::is_app("app"));
}

}  // namespace
}  // namespace consoles_test
