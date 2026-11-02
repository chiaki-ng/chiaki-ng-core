#include "tests/unit/core/consoles/adapters/titles/console_library_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace consoles_test {
namespace {

using libchiaking::consoles::Console;
using libchiaking::consoles::ConsoleLibrary;
using libchiaking::consoles::ConsoleName;
using libchiaking::consoles::DeviceUid;
using libchiaking::consoles::Firmware;
using libchiaking::consoles::Generation;
using libchiaking::consoles::InstalledTitle;
using libchiaking::consoles::StartFailure;
using libchiaking::consoles::StartState;
using libchiaking::consoles::TitlesUnavailable;
using Known = InstalledTitle::LastUse::Known;
using Names = LibraryRig::Names;

class RecentlyPlayedAcrossConsoles : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(living_uid);
    ASSERT_TRUE(bedroom_uid);
    EXPECT_EQ(living->name(), "PS5");
    EXPECT_EQ(living->name_source(), ConsoleName::Source::generation);
    ASSERT_TRUE(rig.account.facts.linked(*living, *rig.user, *living_uid));
    saves = rig.account.store.pairing_saves;
    rig.clients(
        0, {{200, LibraryRig::clients_page(
                      {LibraryRig::listed_console(
                           LibraryRig::uid('a'), "Living Room",
                           {LibraryRig::installed("PPSA00001_00", "PS5"),
                            LibraryRig::installed("PPSA00002_00", "PS5"),
                            LibraryRig::installed("PPSA00003_00", "PS5")}),
                       LibraryRig::listed_console(
                           LibraryRig::uid('b'), "bedroom ",
                           {LibraryRig::installed("PPSA00001_00", "PS5"),
                            LibraryRig::installed("PPSA07001_00", "PS5")}),
                       LibraryRig::listed_console(
                           LibraryRig::uid('c'), "Elsewhere",
                           {LibraryRig::installed("PPSA07002_00", "PS5")})})}});
    auto first_page = LibraryRig::filler_games(0, 45);
    first_page.push_back(
        LibraryRig::game("PPSA00001_00", "Shared", "https://img/1.png",
                         "ps5_native_game", "2026-01-02T00:00:00Z"));
    first_page.push_back(
        LibraryRig::game("PPSA00002_00", "Solo", "https://img/2.png",
                         "ps5_native_game", "2026-01-03T00:00:00Z"));
    first_page.push_back(
        LibraryRig::game("PPSA07002_00", "Away", "https://img/5.png",
                         "ps5_native_game", "2026-01-01T00:00:00Z"));
    first_page.push_back(
        LibraryRig::game("PPSA00003_00", "Player", "https://img/3.png",
                         "ps5_native_media_app", "2026-01-04T00:00:00Z"));
    first_page.push_back(
        LibraryRig::game("PPSA07013_00", "Removed", "https://img/7.png",
                         "ps5_native_game", "2026-01-05T00:00:00Z"));
    rig.games(0, {{200, LibraryRig::gamelist(first_page)}});
    rig.games(50,
              {{200, LibraryRig::gamelist({LibraryRig::game(
                         "PPSA07001_00", "Earlier", "https://img/4.png")})}});
  }

  void fetch() {
    const auto recent = rig.account.consoles.recently_played(*rig.user, {});
    ASSERT_TRUE(recent);
  }

  LibraryRig rig;
  std::shared_ptr<Console> living =
      rig.account.paired(*rig.user, LibraryRig::den, std::nullopt);
  std::shared_ptr<Console> bedroom =
      rig.account.paired(*rig.user, LibraryRig::attic, "Bedroom");
  std::shared_ptr<Console> old = rig.account.paired(
      *rig.user, "112233445566", "Old", Firmware::Family::ps4_10);
  std::optional<DeviceUid> living_uid = DeviceUid::parse(LibraryRig::uid('a'));
  std::optional<DeviceUid> bedroom_uid = DeviceUid::parse(LibraryRig::uid('b'));
  std::size_t saves{};
};

TEST_F(RecentlyPlayedAcrossConsoles, ComeFromOneFetchForEveryConsole) {
  const auto recent = rig.account.consoles.recently_played(*rig.user, {});
  ASSERT_TRUE(recent);
  EXPECT_EQ(rig.http.seen.size(), 3U);
  EXPECT_TRUE(rig.http.unmatched.empty());
  EXPECT_EQ(recent.value().size(), 2U);
  const auto on_living = LibraryRig::titles_on(recent.value(), living);
  ASSERT_TRUE(on_living);
  EXPECT_EQ(LibraryRig::names(*on_living), (Names{"Solo", "Shared"}));
  const auto on_bedroom = LibraryRig::titles_on(recent.value(), bedroom);
  ASSERT_TRUE(on_bedroom);
  EXPECT_EQ(LibraryRig::names(*on_bedroom), (Names{"Shared"}));
  for (const auto& shelf : recent.value())
    for (const auto& entry : shelf.titles) {
      EXPECT_TRUE(entry.recently_played()) << entry.name();
      EXPECT_NE(entry.name(), "Away");
    }
}

TEST_F(RecentlyPlayedAcrossConsoles, NameAndLinkTheListedConsoles) {
  ASSERT_NO_FATAL_FAILURE(fetch());
  EXPECT_EQ(living->name(), "Living Room");
  EXPECT_EQ(living->name_source(), ConsoleName::Source::account);
  EXPECT_EQ(bedroom->name(), "Bedroom");
  EXPECT_EQ(bedroom->name_source(), ConsoleName::Source::pairing);
  EXPECT_EQ(rig.account.facts.device_uid_of(*bedroom, *rig.user), bedroom_uid);
  EXPECT_EQ(rig.account.store.pairing_saves, saves + 1);
  const auto stored =
      rig.account.store.pairing(rig.user->account_id(), bedroom->hardware_id());
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->device_uid, bedroom_uid);
}

TEST_F(RecentlyPlayedAcrossConsoles,
       FeedEachConsolesTitlesWithoutAnotherFetch) {
  ASSERT_NO_FATAL_FAILURE(fetch());
  const auto on_living = living->titles(*rig.user, {});
  ASSERT_TRUE(on_living);
  EXPECT_EQ(LibraryRig::names(on_living.value()), (Names{"Solo", "Shared"}));
  const auto on_bedroom = bedroom->titles(*rig.user, {});
  ASSERT_TRUE(on_bedroom);
  ASSERT_EQ(on_bedroom.value().size(), 2U);
  EXPECT_EQ(on_bedroom.value()[0].name(), "Shared");
  EXPECT_TRUE(on_bedroom.value()[0].recently_played());
  EXPECT_EQ(on_bedroom.value()[1].name(), "Earlier");
  EXPECT_FALSE(on_bedroom.value()[1].recently_played());
  EXPECT_EQ(rig.http.seen.size(), 3U);
}

TEST_F(RecentlyPlayedAcrossConsoles, LeaveAPs4WithoutTitlesOrStarts) {
  ASSERT_NO_FATAL_FAILURE(fetch());
  const auto on_old = old->titles(*rig.user, {});
  ASSERT_FALSE(on_old);
  EXPECT_EQ(on_old.error().reason, TitlesUnavailable::Reason::ps5_only);
  const auto on_living = living->titles(*rig.user, {});
  ASSERT_TRUE(on_living);
  ASSERT_FALSE(on_living.value().empty());
  const auto started_old =
      old->start(on_living.value().front(), *rig.user, nullptr, {});
  ASSERT_FALSE(started_old);
  EXPECT_EQ(started_old.error().reason, StartFailure::Reason::ps5_only);
  EXPECT_EQ(rig.http.seen.size(), 3U);
}

TEST_F(RecentlyPlayedAcrossConsoles, AreNotShownToAnotherUser) {
  ASSERT_NO_FATAL_FAILURE(fetch());
  const auto stranger = rig.account.user(8, LibraryRig::other_token);
  const auto not_theirs = living->titles(*stranger, {});
  ASSERT_FALSE(not_theirs);
  EXPECT_EQ(not_theirs.error().reason, TitlesUnavailable::Reason::not_paired);
  EXPECT_EQ(rig.http.seen.size(), 3U);
}

TEST_F(RecentlyPlayedAcrossConsoles, AreFetchedAgainWhenAskedAgain) {
  ASSERT_NO_FATAL_FAILURE(fetch());
  const auto again = rig.account.consoles.recently_played(
      *rig.account.signed_in.users.find(libchiaking::AccountId{7}), {});
  ASSERT_TRUE(again);
  EXPECT_EQ(rig.http.seen.size(), 6U);
}

TEST_F(RecentlyPlayedAcrossConsoles, AreGoneOnceTheConsoleIsForgotten) {
  ASSERT_NO_FATAL_FAILURE(fetch());
  const auto on_living = living->titles(*rig.user, {});
  ASSERT_TRUE(on_living);
  ASSERT_FALSE(on_living.value().empty());
  ASSERT_TRUE(rig.account.consoles.forget(*bedroom));
  const auto gone = bedroom->titles(*rig.user, {});
  ASSERT_FALSE(gone);
  EXPECT_EQ(gone.error().reason, TitlesUnavailable::Reason::forgotten);
  const auto not_started =
      bedroom->start(on_living.value().front(), *rig.user, nullptr, {});
  ASSERT_FALSE(not_started);
  EXPECT_EQ(not_started.error().reason, StartFailure::Reason::forgotten);
}

class ConsoleTitlesLastUse : public ::testing::Test {
 protected:
  static auto at(std::string_view iso)
      -> std::chrono::system_clock::time_point {
    return std::chrono::system_clock::time_point{
        ConsoleLibrary::played_at(iso).value()};
  }

  void SetUp() override {
    rig.clients(
        0, {{200,
             LibraryRig::clients_page({
                 LibraryRig::listed_console(
                     LibraryRig::uid('a'), "Living Room",
                     {LibraryRig::installed(
                          "PPSA00001_00", "PS5",
                          LibraryRig::accessed("2026-03-05T20:00:00Z") +
                              ",\"conceptId\":\"900002\",\"version\":\"01.000."
                              "000\""),
                      LibraryRig::installed(
                          "PPSA00002_00", "PS5",
                          LibraryRig::accessed("2026-03-01T09:30:00.250Z") +
                              ",\"conceptId\":90000014"),
                      LibraryRig::installed("PPSA00003_00", "PS5",
                                            ",\"conceptId\":\"12x\""),
                      LibraryRig::installed("PPSA07001_00", "PS5",
                                            LibraryRig::accessed("yesterday")),
                      LibraryRig::installed(
                          "PPSA07002_00", "PS5",
                          ",\"lastAccessDateTime\":1772740800000"),
                      LibraryRig::installed(
                          "PPSA07003_00", "PS5",
                          LibraryRig::accessed("2026-02-20T00:00:00Z"))}),
                 LibraryRig::listed_console(
                     LibraryRig::uid('b'), "Bedroom",
                     {LibraryRig::installed(
                          "PPSA00001_00", "PS5",
                          LibraryRig::accessed("2026-02-01T08:00:00+01:00")),
                      LibraryRig::installed(
                          "PPSA00002_00", "PS5",
                          LibraryRig::accessed("2026-03-04T23:59:59Z"))}),
             })}});
    rig.games(
        0,
        {{200,
          LibraryRig::gamelist(
              {LibraryRig::game("PPSA00001_00", "Alpha", "https://img/1.png",
                                "ps5_native_game", "2026-03-05T20:00:00Z"),
               LibraryRig::game("PPSA00002_00", "Beta", "https://img/2.png",
                                "ps5_native_game", "2026-03-04T23:59:59Z"),
               LibraryRig::game("PPSA00003_00", "Gamma", "https://img/3.png",
                                "ps5_native_game", "2026-03-03T00:00:00Z"),
               LibraryRig::game("PPSA07001_00", "Delta", "https://img/4.png",
                                "ps5_native_game", "2026-01-10T00:00:00Z"),
               LibraryRig::game("PPSA07002_00", "Epsilon", "https://img/5.png",
                                "ps5_native_game", "2026-01-09T00:00:00Z"),
               LibraryRig::game("PPSA07003_00", "Zeta", "https://img/6.png",
                                "ps5_native_game", "")})}});
    const auto recent = rig.account.consoles.recently_played(*rig.user, {});
    ASSERT_TRUE(recent);
    EXPECT_EQ(recent.value().size(), 2U);
    EXPECT_EQ(rig.http.seen.size(), 2U);
    EXPECT_TRUE(rig.http.unmatched.empty());
    EXPECT_EQ(rig.http.count("/clients?"), 1);
    EXPECT_EQ(rig.http.count("/gamelist/"), 1);
    auto on_living = LibraryRig::titles_on(recent.value(), living);
    auto on_bedroom = LibraryRig::titles_on(recent.value(), bedroom);
    ASSERT_TRUE(on_living);
    ASSERT_TRUE(on_bedroom);
    here = std::move(*on_living);
    there = std::move(*on_bedroom);
    ASSERT_EQ(here.size(), 6U);
    ASSERT_EQ(there.size(), 2U);
  }

  LibraryRig rig;
  std::shared_ptr<Console> living =
      rig.account.paired(*rig.user, LibraryRig::den, "Living Room");
  std::shared_ptr<Console> bedroom =
      rig.account.paired(*rig.user, LibraryRig::attic, "Bedroom");
  LibraryRig::Titles here;
  LibraryRig::Titles there;
};

TEST_F(ConsoleTitlesLastUse, OrdersEachConsoleByItsOwnLastUse) {
  EXPECT_EQ(LibraryRig::names(here),
            (Names{"Alpha", "Gamma", "Beta", "Zeta", "Delta", "Epsilon"}));
  EXPECT_EQ(LibraryRig::names(there), (Names{"Beta", "Alpha"}));
}

TEST_F(ConsoleTitlesLastUse, KeepsTheUseOnThisConsole) {
  EXPECT_EQ(here[0].last_used(),
            (InstalledTitle::LastUse{at("2026-03-05T20:00:00Z"),
                                     Known::on_this_console}));
  EXPECT_EQ(there[1].last_used(),
            (InstalledTitle::LastUse{at("2026-02-01T07:00:00Z"),
                                     Known::on_this_console}));
  EXPECT_EQ(here[2].last_used(),
            (InstalledTitle::LastUse{at("2026-03-01T09:30:00.250Z"),
                                     Known::on_this_console}));
  EXPECT_EQ(there[0].last_used(),
            (InstalledTitle::LastUse{at("2026-03-04T23:59:59Z"),
                                     Known::on_this_console}));
  EXPECT_NE(here[0].last_used(), there[1].last_used());
  EXPECT_NE(here[2].last_used(), there[0].last_used());
  EXPECT_EQ(here[3].last_used(),
            (InstalledTitle::LastUse{at("2026-02-20T00:00:00Z"),
                                     Known::on_this_console}));
}

TEST_F(ConsoleTitlesLastUse, FallsBackToTheUseOnTheAccount) {
  EXPECT_EQ(here[1].last_used(),
            (InstalledTitle::LastUse{at("2026-03-03T00:00:00Z"),
                                     Known::on_the_account}));
  EXPECT_EQ(here[4].last_used(),
            (InstalledTitle::LastUse{at("2026-01-10T00:00:00Z"),
                                     Known::on_the_account}));
  EXPECT_EQ(here[5].last_used(),
            (InstalledTitle::LastUse{at("2026-01-09T00:00:00Z"),
                                     Known::on_the_account}));
}

TEST_F(ConsoleTitlesLastUse, KeepsOnlyValidConcepts) {
  EXPECT_EQ(here[0].concept_id(), "900002");
  EXPECT_EQ(here[2].concept_id(), "90000014");
  EXPECT_TRUE(here[1].concept_id().empty());
  EXPECT_TRUE(there[0].concept_id().empty());
}

TEST_F(ConsoleTitlesLastUse, ListsOnlyWhatEachConsoleHolds) {
  for (const auto& title : there) {
    EXPECT_NE(title.name(), "Gamma");
    EXPECT_NE(title.name(), "Delta");
    EXPECT_NE(title.name(), "Epsilon");
    EXPECT_NE(title.name(), "Zeta");
  }
}

TEST_F(ConsoleTitlesLastUse, FeedsEachConsoleWithoutAnotherFetch) {
  const auto on_living = living->titles(*rig.user, {});
  const auto on_bedroom = bedroom->titles(*rig.user, {});
  ASSERT_TRUE(on_living);
  EXPECT_EQ(on_living.value(), here);
  ASSERT_TRUE(on_bedroom);
  EXPECT_EQ(on_bedroom.value(), there);
  EXPECT_EQ(rig.http.seen.size(), 2U);
  EXPECT_TRUE(rig.log.contains("8 titles, 5 with the console's own last use"));
}

TEST(ConsoleTitlesOffTheAccount, AreUnavailableForAConsolePsDoesNotList) {
  LibraryRig rig;
  const auto living =
      rig.account.paired(*rig.user, LibraryRig::den, "Living Room");
  const auto bedroom =
      rig.account.paired(*rig.user, LibraryRig::attic, "Bedroom");
  rig.clients(
      0,
      {{200,
        LibraryRig::clients_page(
            {LibraryRig::listed_console(LibraryRig::uid('a'), "Living Room"),
             LibraryRig::listed_console(LibraryRig::uid('c'), "Kitchen")})}});
  rig.games(0, {{200, LibraryRig::gamelist({})}});
  const auto on_living = living->titles(*rig.user, {});
  ASSERT_TRUE(on_living);
  EXPECT_TRUE(on_living.value().empty());
  const auto on_bedroom = bedroom->titles(*rig.user, {});
  ASSERT_FALSE(on_bedroom);
  EXPECT_EQ(on_bedroom.error().reason,
            TitlesUnavailable::Reason::not_on_account);
  EXPECT_EQ(rig.http.seen.size(), 2U);
  const auto wanted =
      LibraryRig::title("PPSA00001_00", "PPSA00001", "One", "https://img/a.png",
                        Generation::ps5, std::nullopt, false);
  const auto started = bedroom->start(wanted, *rig.user, nullptr, {});
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error().reason, StartFailure::Reason::not_on_account);
  EXPECT_EQ(started.error().reached, StartState::pending);
  EXPECT_EQ(rig.http.seen.size(), 2U);
}

}  // namespace
}  // namespace consoles_test
