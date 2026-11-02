#include "tests/support/consoles/start_states.hpp"
#include "tests/unit/api/consoles/titles_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace api_consoles_test {
namespace {

using libchiaking::AccountId;
using libchiaking::consoles::ConsoleName;
using libchiaking::consoles::DeviceUid;
using libchiaking::consoles::Firmware;
using libchiaking::consoles::Generation;
using libchiaking::consoles::InstalledTitle;
using libchiaking::consoles::StartFailure;
using libchiaking::consoles::StartState;
using libchiaking::consoles::TitlesUnavailable;
using libchiaking::users::Unanswered;

class ConsoleTitles : public ConsoleTitlesTest {};

TEST_F(ConsoleTitles, NeedASignedInUser) {
  const auto home = rig.home(40);
  const auto console = rig.paired(living_room, Firmware::Family::ps5_1, 40);
  const auto titles = console->titles(*home, {});
  ASSERT_FALSE(titles);
  EXPECT_EQ(titles.error().reason, TitlesUnavailable::Reason::unanswered);
  ASSERT_TRUE(titles.error().unanswered);
  EXPECT_EQ(titles.error().unanswered->reason, Unanswered::Reason::signed_out);
  const auto recent = rig.known.recently_played(*home, {});
  ASSERT_FALSE(recent);
  EXPECT_EQ(recent.error().reason, Unanswered::Reason::signed_out);
  for (const auto generation : {Generation::ps4, Generation::ps5}) {
    const auto listed = rig.known.consoles_on_account(*home, generation, {});
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().reason, Unanswered::Reason::signed_out);
  }
  EXPECT_TRUE(rig.http.seen.empty());
}

TEST_F(ConsoleTitles, AreListedAndStartedOnTheirConsole) {
  const std::string duid(64, 'a');
  const std::string clients_url =
      "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/"
      "clients?platform=PS5&includeFields=device,systemData&limit=10&offset=0";
  const std::string played_url =
      "https://m.np.playstation.com/api/gamelist/"
      "v2/users/me/titles?limit=50&offset=0";
  const std::string poll_url =
      std::string{commands_url} +
      "?platform=PS5&commandIds=CMD&includeFields=actions,commandDetail&"
      "commandStatus=executing,completed";

  const auto user = rig.signed_in(7);
  const auto console = rig.paired(living_room, Firmware::Family::ps5_1, 7);
  const auto old = rig.paired(old_console, Firmware::Family::ps4_10, 7);
  rig.http.on(
      "GET", clients_url, 200,
      R"({"clients":[{"duid":")" + duid +
          R"(","platform":"PS5","device":{"name":"Living Room"},"systemData":{"installedTitles":{"titles":[)"
          R"({"npTitleId":"PPSA01234_00","psPlatform":"PS5"},{"npTitleId":"PPSA07055_00","psPlatform":"PS5"}]}}}]})");
  rig.http.on(
      "GET", played_url, 200,
      R"({"titles":[{"titleId":"PPSA01234_00","name":"Game","imageUrl":"https://img/g.png",)"
      R"("category":"ps5_native_game","lastPlayedDateTime":"2026-01-01T18:00:00Z"},)"
      R"({"titleId":"PPSA07055_00","name":"TV","imageUrl":"https://img/t.png","category":"ps5_native_media_app"}]})");
  rig.http.on("POST", std::string{commands_url}, 202, R"({"commandId":"CMD"})");
  rig.http.on(
      "GET", poll_url, 200,
      R"({"commands":[{"commandId":"CMD","commandStatus":"completed"}]})");

  const auto recent = rig.known.recently_played(*user, {});
  ASSERT_TRUE(recent);
  ASSERT_EQ(recent.value().size(), 1U);
  EXPECT_EQ(rig.http.seen.size(), 2U);
  EXPECT_EQ(recent.value().front().console, console);
  ASSERT_EQ(recent.value().front().titles.size(), 1U);
  const auto& played = recent.value().front().titles.front();
  EXPECT_EQ(played.title_id(), "PPSA01234_00");
  EXPECT_EQ(played.start_id(), "PPSA01234");
  EXPECT_EQ(played.name(), "Game");
  EXPECT_EQ(played.cover_url(), "https://img/g.png");
  EXPECT_EQ(played.platform(), Generation::ps5);
  EXPECT_TRUE(played.recently_played());
  ASSERT_TRUE(played.last_used());
  EXPECT_EQ(played.last_used()->at,
            std::chrono::system_clock::time_point{
                std::chrono::milliseconds{1767290400000}});
  EXPECT_EQ(played.last_used()->known,
            InstalledTitle::LastUse::Known::on_the_account);
  EXPECT_EQ(console->name(), "Living Room");
  EXPECT_EQ(console->name_source(), ConsoleName::Source::account);
  const auto link = rig.store.pairing(AccountId{7}, console->hardware_id());
  ASSERT_TRUE(link);
  EXPECT_EQ(link->device_uid, DeviceUid::parse(duid));

  const auto titles = console->titles(*user, {});
  ASSERT_TRUE(titles);
  ASSERT_EQ(titles.value().size(), 1U);
  EXPECT_EQ(titles.value().front(), played);
  EXPECT_EQ(rig.http.seen.size(), 2U);

  test_support::consoles::StartStates watched;
  const auto started =
      console->start(titles.value().front(), *user, &watched, {});
  ASSERT_TRUE(started);
  EXPECT_EQ(started.value(), StartState::completed);
  EXPECT_EQ(watched.states, (std::vector<StartState>{StartState::pending,
                                                     StartState::completed}));
  ASSERT_EQ(rig.http.seen.size(), 4U);
  EXPECT_EQ(rig.http.seen[2].method, "POST");
  EXPECT_EQ(rig.http.seen[2].url, commands_url);
  EXPECT_EQ(
      rig.http.seen[2].body,
      R"({"commandDetail":{"platform":"PS5","duid":")" + duid +
          R"(","commandType":"launchTitle","parameters":{"titleIds":["PPSA01234"]}}})");
  for (const auto& sent : rig.http.seen)
    EXPECT_EQ(sent.authorization, "Bearer " + std::string{TitlesRig::token});

  const auto main = rig.known.consoles_on_account(*user, Generation::ps4, {});
  ASSERT_TRUE(main);
  ASSERT_EQ(main.value().size(), 1U);
  EXPECT_TRUE(main.value().front().is_main_ps4());
  EXPECT_FALSE(old->titles(*user, {}));
  EXPECT_EQ(rig.http.seen.size(), 4U);

  EXPECT_TRUE(rig.known.forget(*console));
  const auto gone = console->titles(*user, {});
  ASSERT_FALSE(gone);
  EXPECT_EQ(gone.error().reason, TitlesUnavailable::Reason::forgotten);
  const auto not_started =
      console->start(titles.value().front(), *user, &watched, {});
  ASSERT_FALSE(not_started);
  EXPECT_EQ(not_started.error().reason, StartFailure::Reason::forgotten);
  EXPECT_TRUE(rig.never_logged_the_token());
}

}  // namespace
}  // namespace api_consoles_test
