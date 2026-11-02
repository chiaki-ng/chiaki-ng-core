#include "tests/unit/core/consoles/adapters/titles/console_library_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace consoles_test {
namespace {

using libchiaking::consoles::Generation;
using libchiaking::users::Unanswered;
using Reason = Unanswered::Reason;
using Reply = LibraryRig::Reply;

class ConsoleLibraryGamelist : public ::testing::Test {
 protected:
  void installs(std::string_view title_id) {
    rig.clients(
        0, {{200, LibraryRig::clients_page({LibraryRig::client(
                      "a", "A", {LibraryRig::installed(title_id, "PS5")})})}});
  }

  LibraryRig rig;
};

TEST_F(ConsoleLibraryGamelist, StopsOnceEveryInstalledIdIsCovered) {
  installs("PPSA00001_00");
  auto first = LibraryRig::filler_games(0, 49);
  first.push_back(LibraryRig::game("PPSA00001_00", "Wanted"));
  rig.games(0, {{200, LibraryRig::gamelist(first)}});
  EXPECT_TRUE(rig.run());
  EXPECT_EQ(rig.http.count("/gamelist/"), 1);
}

TEST_F(ConsoleLibraryGamelist, PagesUntilCovered) {
  installs("PPSA00001_00");
  rig.games(0, {{200, LibraryRig::gamelist(LibraryRig::filler_games(0, 50))}});
  auto second = LibraryRig::filler_games(50, 49);
  second.push_back(LibraryRig::game("PPSA00001_00", "Wanted"));
  rig.games(50, {{200, LibraryRig::gamelist(second)}});
  const auto result = rig.run();
  ASSERT_TRUE(result);
  EXPECT_EQ(rig.http.count("/gamelist/"), 2);
  EXPECT_TRUE(rig.http.unmatched.empty());
  const auto* console = LibraryRig::shelf(result, "a");
  ASSERT_TRUE(console);
  ASSERT_EQ(console->titles.size(), 1U);
  EXPECT_EQ(console->titles.front().name(), "Wanted");
  EXPECT_FALSE(console->titles.front().recently_played());
}

TEST_F(ConsoleLibraryGamelist, IsBoundedAtTenPages) {
  installs("NEVER0001_00");
  for (int page = 0; page < 12; ++page)
    rig.games(
        static_cast<std::size_t>(page) * 50,
        {{200, LibraryRig::gamelist(LibraryRig::filler_games(page * 50, 50))}});
  EXPECT_TRUE(rig.run());
  EXPECT_EQ(rig.http.count("/gamelist/"), 10);
  EXPECT_EQ(rig.http.count("offset=450"), 1);
  EXPECT_EQ(rig.http.count("offset=500"), 0);
}

TEST_F(ConsoleLibraryGamelist, StopsAtAShortPage) {
  installs("NEVER0001_00");
  rig.games(0, {{200, LibraryRig::gamelist(LibraryRig::filler_games(0, 50))}});
  rig.games(50, {{200, LibraryRig::gamelist(LibraryRig::filler_games(50, 3))}});
  EXPECT_TRUE(rig.run());
  EXPECT_EQ(rig.http.count("/gamelist/"), 2);
}

TEST_F(ConsoleLibraryGamelist, StopsAtARepeatedPage) {
  installs("NEVER0001_00");
  rig.games(0, {{200, LibraryRig::gamelist(LibraryRig::filler_games(0, 50))}});
  rig.games(50, {{200, LibraryRig::gamelist(LibraryRig::filler_games(0, 50))}});
  EXPECT_TRUE(rig.run());
  EXPECT_EQ(rig.http.count("/gamelist/"), 2);
}

TEST_F(ConsoleLibraryGamelist, StopsAtAnEmptyPage) {
  installs("NEVER0001_00");
  rig.games(0, {{200, LibraryRig::gamelist({})}});
  EXPECT_TRUE(rig.run());
  EXPECT_EQ(rig.http.count("/gamelist/"), 1);
}

TEST(ConsoleLibraryGamelistPages, FailEverythingWhenOneFails) {
  struct Case {
    Reply later;
    Reason reason;
  };
  const std::vector<Case> cases{
      {Reply{500, ""}, Reason::unavailable},
      {Reply{200, "{\"items\":[]}"}, Reason::malformed},
      {Reply{200, "[]"}, Reason::malformed},
      {Reply::transport(), Reason::unavailable}};
  for (std::size_t index = 0; index < cases.size(); ++index) {
    LibraryRig rig;
    rig.clients(
        0, {{200,
             LibraryRig::clients_page({LibraryRig::client(
                 "a", "A", {LibraryRig::installed("PPSA00001_00", "PS5")})})}});
    auto first = LibraryRig::filler_games(0, 49);
    first.push_back(LibraryRig::game("PPSA00002_00", "Other"));
    rig.games(0, {{200, LibraryRig::gamelist(first)}});
    rig.games(50, {cases[index].later});
    const auto result = rig.run();
    ASSERT_FALSE(result) << index;
    EXPECT_EQ(result.error().reason, cases[index].reason) << index;
  }
}

TEST_F(ConsoleLibraryGamelist, FailsOnAnObjectWithoutTitles) {
  rig.clients(0, {{200, LibraryRig::clients_page({})}});
  rig.games(0, {{200, "{}"}});
  const auto result = rig.run();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, Reason::malformed);
}

TEST_F(ConsoleLibraryGamelist, FallsBackToTheBaseFields) {
  const std::string body =
      "{\"titles\":["
      "{\"titleId\":\" ppsa00001_00 "
      "\",\"localizedName\":\"Local\",\"name\":\"Base\","
      "\"localizedImageUrl\":\"https://l\",\"imageUrl\":\"https://"
      "b\",\"category\":\"ps5_native_game\","
      "\"lastPlayedDateTime\":\"2026-01-01T18:00:00.250Z\"},"
      "{\"titleId\":\"PPSA00002_00\",\"localizedName\":\"  "
      "\",\"name\":\"Base2\",\"imageUrl\":\"https://b2\"},"
      "{\"name\":\"no id\"},"
      "42]}";
  rig.clients(0,
              {{200, LibraryRig::clients_page({LibraryRig::client(
                         "d", "D",
                         {LibraryRig::installed("PPSA00001_00", "PS5"),
                          LibraryRig::installed("PPSA00002_00", "PS5")})})}});
  rig.games(0, {{200, body}});
  const auto result = rig.run();
  ASSERT_TRUE(result);
  EXPECT_EQ(rig.http.count("/gamelist/"), 1);
  const auto* console = LibraryRig::shelf(result, "d");
  ASSERT_TRUE(console);
  ASSERT_EQ(console->titles.size(), 2U);
  EXPECT_EQ(console->titles[0],
            LibraryRig::title("ppsa00001_00", "ppsa00001", "Local", "https://l",
                              Generation::ps5, LibraryRig::at_ms(1767290400250),
                              true));
  EXPECT_EQ(
      console->titles[1],
      LibraryRig::title("PPSA00002_00", "PPSA00002", "Base2", "https://b2",
                        Generation::ps5, std::nullopt, true));
}

TEST_F(ConsoleLibraryGamelist, FailsWhenTitlesIsAnObject) {
  rig.clients(0, {{200, LibraryRig::clients_page({})}});
  rig.games(0, {{200, "{\"titles\":{}}"}});
  const auto failed = rig.run();
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().reason, Reason::malformed);
}

TEST_F(ConsoleLibraryGamelist, IsUnavailableOverTheResponseLimit) {
  const std::string padded = "{\"titles\":[]}" + std::string(40, ' ');
  rig.clients(0, {{200, LibraryRig::clients_page({})}});
  rig.games(0, {{200, padded}});
  auto library = rig.library(padded.size() - 1);
  const auto failed = library.asked(*rig.user, {});
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().reason, Reason::unavailable);
  EXPECT_EQ(rig.http.count("/gamelist/"), 1);
}

}  // namespace
}  // namespace consoles_test
