#include "tests/unit/api/chiaki_ng/rig.hpp"

#include <gtest/gtest.h>

namespace chiaki_ng_test {
namespace {

using ChiakiNgConsoleStart = ChiakiNgTest;

TEST_F(ChiakiNgConsoleStart, GoesHomeOrOverTheInternetFromWhereTheConsoleIs) {
  Household home;
  EXPECT_TRUE(home.credentials.save(SignedInPlayer::saved_for_remote_play()));
  EXPECT_TRUE(home.credentials.save(
      users::SavedHomeUser{AccountId{home_account}, "Living Room Player"}));
  EXPECT_TRUE(home.credentials.save(
      users::SavedHomeUser{AccountId{home_account + 1}, "Stranger"}));
  home.save_pairing(AccountId{home_account}, living_room);
  home.save_pairing(AccountId{SignedInPlayer::remote_play_account},
                    living_room);
  EXPECT_TRUE(home.chiaki.users().welcome_back());
  EXPECT_TRUE(home.chiaki.consoles().remember_saved());
  const auto home_user = home.chiaki.users().find(AccountId{home_account});
  const auto stranger = home.chiaki.users().find(AccountId{home_account + 1});
  const auto signed_in =
      home.chiaki.users().find(AccountId{SignedInPlayer::remote_play_account});
  const auto console =
      home.chiaki.consoles().find(Household::hardware(living_room));
  ASSERT_TRUE(home_user);
  ASSERT_TRUE(stranger);
  ASSERT_TRUE(signed_in);
  ASSERT_TRUE(console);

  EXPECT_EQ(refusal(console->start(nullptr, {}, home.media, home.events)),
            StartRefusal::Kind::invalid_preferences);
  EXPECT_EQ(refusal(console->start(stranger, {}, home.media, home.events)),
            StartRefusal::Kind::not_paired);
  EXPECT_EQ(route(console->route_for(*stranger)), consoles::Route::home);
  EXPECT_EQ(refusal(console->start(home_user, {}, home.media, home.events,
                                   consoles::Route::internet)),
            StartRefusal::Kind::needs_psn_sign_in);
  EXPECT_EQ(home.chiaki.play().live(), nullptr);
  EXPECT_TRUE(home.events.names().empty());

  fakes::FakeConsole fake{home.network, fakes::FakeConsole::Identity{
                                            .hardware_id = "AABBCCDDEEFF"}};
  home.sighted(living_room);
  EXPECT_EQ(console->presence(), consoles::Presence::ready);
  EXPECT_EQ(route(console->route_for(*signed_in)), consoles::Route::home);
  const auto at_home = console->start(signed_in, {}, home.media, home.events);
  ASSERT_TRUE(at_home);
  EXPECT_EQ(at_home.value()->where(), Where::home);
  EXPECT_EQ(at_home.value()->console(), console.get());
  EXPECT_EQ(at_home.value()->player().account_id(), signed_in->account_id());
  home.end(*at_home.value());

  home.sighted(living_room, true);
  EXPECT_EQ(console->presence(), consoles::Presence::rest_mode);
  EXPECT_EQ(route(console->route_for(*signed_in)), consoles::Route::home);
  RecordedEvents resting_events;
  const auto resting =
      console->start(signed_in, {}, home.media, resting_events);
  ASSERT_TRUE(resting);
  EXPECT_EQ(resting.value()->where(), Where::home);
  home.end(*resting.value());

  home.gone(*console);
  EXPECT_EQ(console->presence(), consoles::Presence::not_nearby);
  EXPECT_EQ(route(console->route_for(*signed_in)), consoles::Route::internet);
  EXPECT_EQ(route(console->route_for(*home_user)), consoles::Route::home);
  home.ps.consoles_on_account =
      test_support::play::ScriptedCloudPs::account_listing("07", R"(["remotePlay"])");
  RecordedEvents away_events;
  const auto away = console->start(signed_in, {}, home.media, away_events);
  ASSERT_TRUE(away);
  EXPECT_EQ(away.value()->where(), Where::internet);
  EXPECT_EQ(away.value()->console(), console.get());
  home.end(*away.value());

  RecordedEvents insisted_events;
  const auto insisted = console->start(signed_in, {}, home.media,
                                       insisted_events, consoles::Route::home);
  ASSERT_TRUE(insisted);
  EXPECT_EQ(insisted.value()->where(), Where::home);
  home.end(*insisted.value());

  RecordedEvents offline_events;
  const auto offline =
      console->start(home_user, {}, home.media, offline_events);
  ASSERT_TRUE(offline);
  EXPECT_EQ(offline.value()->where(), Where::home);
  home.end(*offline.value());

  platform::StopSource stopped;
  EXPECT_TRUE(stopped.request_stop());
  consoles::ConsoleFacts{home.chiaki.consoles()}.out_of_sight(*console, true);
  EXPECT_EQ(console->presence(), consoles::Presence::unknown);
  const auto unasked = console->route_for(*signed_in, stopped.token());
  ASSERT_FALSE(unasked);
  EXPECT_EQ(unasked.error().kind(), StartRefusal::Kind::cancelled);
  EXPECT_EQ(
      refusal(console->start(signed_in, {}, home.media, home.events,
                             consoles::Route::automatic, stopped.token())),
      StartRefusal::Kind::cancelled);

  EXPECT_TRUE(home.chiaki.consoles().forget(*console));
  const auto forgotten = console->start(signed_in, {}, home.media, home.events);
  ASSERT_EQ(refusal(forgotten), StartRefusal::Kind::console_unreachable);
  EXPECT_EQ(forgotten.error().unreachable(), consoles::Unreachable::forgotten);
  const auto no_route = console->route_for(*signed_in);
  ASSERT_FALSE(no_route);
  EXPECT_EQ(no_route.error().kind(), StartRefusal::Kind::console_unreachable);
  EXPECT_EQ(no_route.error().unreachable(), consoles::Unreachable::forgotten);
}

TEST_F(ChiakiNgConsoleStart,
       FindsAConsoleOnTheAccountAndStartsOverTheInternetWithoutHomePairing) {
  Household home;
  EXPECT_TRUE(home.credentials.save(SignedInPlayer::saved_for_remote_play()));
  EXPECT_TRUE(home.credentials.save(
      users::SavedHomeUser{AccountId{home_account}, "Living Room Player"}));
  EXPECT_TRUE(home.credentials.save(
      users::SavedHomeUser{AccountId{home_account + 1}, "Stranger"}));
  home.save_pairing(AccountId{home_account}, living_room);
  EXPECT_TRUE(home.chiaki.users().welcome_back());
  EXPECT_TRUE(home.chiaki.consoles().remember_saved());
  const auto stranger = home.chiaki.users().find(AccountId{home_account + 1});
  const auto signed_in =
      home.chiaki.users().find(AccountId{SignedInPlayer::remote_play_account});
  const auto console =
      home.chiaki.consoles().find(Household::hardware(living_room));
  ASSERT_TRUE(stranger);
  ASSERT_TRUE(signed_in);
  ASSERT_TRUE(console);
  EXPECT_TRUE(signed_in->has_psn_account());
  EXPECT_FALSE(console->is_paired_with(*signed_in));

  EXPECT_EQ(refusal(console->start(stranger, {}, home.media, home.events)),
            StartRefusal::Kind::not_paired);
  EXPECT_EQ(route(console->route_for(*stranger)), consoles::Route::home);
  EXPECT_EQ(home.ps.console_lists, 0U);

  EXPECT_EQ(refusal(console->start(signed_in, {}, home.media, home.events)),
            StartRefusal::Kind::not_paired);
  EXPECT_EQ(route(console->route_for(*signed_in)), consoles::Route::home);
  EXPECT_EQ(home.ps.console_lists, 2U);

  home.ps.consoles_on_account = R"({"clients":[]})";
  EXPECT_EQ(refusal(console->start(signed_in, {}, home.media, home.events)),
            StartRefusal::Kind::not_paired);
  EXPECT_EQ(route(console->route_for(*signed_in)), consoles::Route::home);
  EXPECT_EQ(home.ps.console_lists, 4U);

  home.ps.consoles_on_account =
      test_support::play::ScriptedCloudPs::account_listing("aa", "[]");
  EXPECT_EQ(refusal(console->start(signed_in, {}, home.media, home.events)),
            StartRefusal::Kind::not_paired);
  EXPECT_EQ(route(console->route_for(*signed_in)), consoles::Route::home);
  EXPECT_EQ(home.ps.console_lists, 6U);
  EXPECT_EQ(refusal(console->start(signed_in, {}, home.media, home.events,
                                   consoles::Route::internet)),
            StartRefusal::Kind::remote_play_disabled);

  platform::StopSource stopped;
  EXPECT_TRUE(stopped.request_stop());
  const auto asked_before_cancelling = home.ps.console_lists;
  EXPECT_EQ(
      refusal(console->start(signed_in, {}, home.media, home.events,
                             consoles::Route::automatic, stopped.token())),
      StartRefusal::Kind::cancelled);
  const auto unasked = console->route_for(*signed_in, stopped.token());
  ASSERT_FALSE(unasked);
  EXPECT_EQ(unasked.error().kind(), StartRefusal::Kind::cancelled);
  EXPECT_EQ(home.ps.console_lists, asked_before_cancelling);
  EXPECT_EQ(home.chiaki.play().live(), nullptr);
  EXPECT_TRUE(home.events.names().empty());

  home.ps.consoles_on_account =
      test_support::play::ScriptedCloudPs::account_listing("aa", R"(["remotePlay"])");
  EXPECT_EQ(route(console->route_for(*signed_in)), consoles::Route::internet);
  const auto asked_before_starting = home.ps.console_lists;
  const auto away = console->start(signed_in, {}, home.media, home.events);
  ASSERT_TRUE(away);
  EXPECT_EQ(away.value()->where(), Where::internet);
  EXPECT_EQ(away.value()->console(), console.get());
  EXPECT_EQ(away.value()->player().account_id(), signed_in->account_id());
  EXPECT_EQ(home.ps.console_lists, asked_before_starting + 1);
  EXPECT_FALSE(console->is_paired_with(*signed_in));
  home.end(*away.value());

  RecordedEvents insisted_events;
  EXPECT_EQ(refusal(console->start(signed_in, {}, home.media, insisted_events,
                                   consoles::Route::home)),
            StartRefusal::Kind::not_paired);

  home.sighted(living_room);
  EXPECT_EQ(console->presence(), consoles::Presence::ready);
  const auto asked_before_sighting = home.ps.console_lists;
  EXPECT_EQ(route(console->route_for(*signed_in)), consoles::Route::home);
  RecordedEvents nearby_events;
  EXPECT_EQ(refusal(console->start(signed_in, {}, home.media, nearby_events)),
            StartRefusal::Kind::not_paired);
  EXPECT_EQ(home.ps.console_lists, asked_before_sighting);
}

TEST_F(ChiakiNgConsoleStart, RefusesAConsoleKnownWithoutPlay) {
  Household home;
  fakes::InMemoryConsoleStore store{home.threading};
  platform::SystemSecureRandom random;
  consoles::KnownConsoles alone{
      libchiaking::testing::ServiceBuilder::build<consoles::KnownConsoles>(
          home.network, home.ps, home.crypto, random, store, home.threading,
          home.steady)};
  auto host = consoles::Host::create("127.0.0.1", Generation::ps5);
  ASSERT_TRUE(host);
  const auto console = consoles::ConsoleFacts{alone}.sighted(
      {Household::hardware(living_room),
       consoles::Firmware::newest(Generation::ps5), std::nullopt,
       std::move(host.value()), false, std::nullopt});
  const auto added = home.chiaki.users().add_home_user(AccountId{home_account},
                                                       "Living Room Player");
  ASSERT_TRUE(console);
  ASSERT_TRUE(added);
  const auto refused =
      console->start(added.value(), {}, home.media, home.events);
  ASSERT_EQ(refusal(refused), StartRefusal::Kind::console_unreachable);
  EXPECT_EQ(refused.error().unreachable(),
            consoles::Unreachable::not_available);
  const auto no_route = console->route_for(*added.value());
  ASSERT_FALSE(no_route);
  EXPECT_EQ(no_route.error().kind(), StartRefusal::Kind::console_unreachable);
  EXPECT_EQ(no_route.error().unreachable(),
            consoles::Unreachable::not_available);
}

}  // namespace
}  // namespace chiaki_ng_test
