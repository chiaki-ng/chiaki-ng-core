#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/openssl/household.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace play_test {

using test_support::LiveProviders;
using test_support::play::LaunchCase;
using test_support::play::SignedInPlayer;
using test_support::play::Title;
namespace {

namespace catalog = libchiaking::catalog;
namespace play = libchiaking::play;
namespace shared = libchiaking::progress;
using libchiaking::platform::ThreadIdentity;
using play::LaunchActivity;
using play::LaunchProgress;
using play::SessionState;
using play::Where;

constexpr auto service = catalog::ServiceGeneration::ps5_cloud;

class LeavingAtAllocation final : public play::SessionEvents {
 public:
  explicit LeavingAtAllocation(play::Play& play) noexcept : play_(play) {}

  void on_state(SessionState state) override { states.push_back(state); }
  void on_ended(const play::Outcome& outcome) override {
    ended_as_left = outcome.was_left();
  }

  void on_launch_progress(const LaunchProgress& progress) override {
    activities.push_back(progress.activity());
    queue_positions.push_back(progress.queue_position());
    threads.push_back(ThreadIdentity::current());
    if (progress.activity() != LaunchActivity::allocating_server) return;
    const auto live = play_.live();
    left_from_callback = live != nullptr && live->leave();
  }

  void on_datacenters_offered(const play::DatacenterOffer& offer) override {
    for (const auto& entry : offer.entries())
      offered.push_back(std::string{entry.name().text()});
  }

  void on_datacenter_chosen(const play::DatacenterChoice& choice) override {
    chosen = std::string{choice.name().text()};
    chosen_was_forced = choice.was_forced();
  }

  std::vector<SessionState> states;
  std::vector<std::optional<LaunchActivity>> activities;
  std::vector<std::optional<std::uint32_t>> queue_positions;
  std::vector<ThreadIdentity> threads;
  std::vector<std::string> offered;
  std::string chosen;
  bool chosen_was_forced{};
  bool left_from_callback{};
  bool ended_as_left{};

 private:
  play::Play& play_;
};

class PlayScriptedCloudLaunch : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto started = home_.play.start_scripted(player_, Where::cloud, link_,
                                                   {}, home_.media, events_);
    ASSERT_TRUE(started);
    const auto session = started.value();
    EXPECT_EQ(session->where(), Where::cloud);
    EXPECT_EQ(home_.play.live(), session);

    const std::array probes{play::DatacenterOffer::Probe{
                                "dc1", std::chrono::milliseconds{12}, true},
                            play::DatacenterOffer::Probe{
                                "dc2", std::chrono::milliseconds{48}, true}};
    link_.launch_progress(
        *LaunchProgress::at(service, LaunchActivity::reserving_server));
    link_.launch_progress(
        *LaunchProgress::at(service, LaunchActivity::measuring_datacenters));
    link_.offer_datacenters(play::DatacenterOffer::from(probes));
    link_.choose_datacenter(play::DatacenterChoice::picked_at_launch(
        *play::DatacenterName::parse("dc2"), std::chrono::milliseconds{48}));
    link_.launch_progress(*LaunchProgress::at(
        service, LaunchActivity::allocating_server, {.queue_position = 3}));
    link_.launch_progress(
        *LaunchProgress::at(service, LaunchActivity::connecting_to_server));

    const auto& outcome = session->wait_until_ended();
    EXPECT_TRUE(outcome.was_left());
    EXPECT_TRUE(ended_as(outcome, play::Outcome::Ending::left));
    EXPECT_EQ(session->state(), SessionState::ended);
    EXPECT_EQ(home_.play.live(), nullptr);
  }

  Household home_;
  std::shared_ptr<libchiaking::users::PsUser> player_ = SignedInPlayer::fresh();
  play::testing::ScriptedLink link_{LiveProviders::threading(), LiveProviders::steady()};
  LeavingAtAllocation events_{home_.play};
};

TEST_F(PlayScriptedCloudLaunch, CanBeLeftFromItsProgressCallback) {
  EXPECT_TRUE(events_.left_from_callback);
  EXPECT_TRUE(events_.ended_as_left);
  EXPECT_EQ(std::ranges::find(events_.states, SessionState::playing),
            events_.states.end());
  ASSERT_FALSE(events_.states.empty());
  EXPECT_EQ(events_.states.back(), SessionState::ended);
}

TEST_F(PlayScriptedCloudLaunch, PassesItsProgressOnInOrder) {
  ASSERT_GE(events_.activities.size(), 3U);
  ASSERT_GE(events_.queue_positions.size(), 3U);
  EXPECT_EQ(events_.activities[0], LaunchActivity::reserving_server);
  EXPECT_EQ(events_.activities[1], LaunchActivity::measuring_datacenters);
  EXPECT_EQ(events_.activities[2], LaunchActivity::allocating_server);
  EXPECT_EQ(events_.queue_positions[2], 3U);
  EXPECT_EQ(events_.offered, (std::vector<std::string>{"dc1", "dc2"}));
  EXPECT_EQ(events_.chosen, "dc2");
  EXPECT_TRUE(events_.chosen_was_forced);
}

TEST_F(PlayScriptedCloudLaunch, CallsBackOnOneThreadOfItsOwn) {
  ASSERT_FALSE(events_.threads.empty());
  EXPECT_TRUE(std::ranges::all_of(events_.threads, [&](ThreadIdentity thread) {
    return thread == events_.threads.front();
  }));
  EXPECT_NE(events_.threads.front(), ThreadIdentity::current());
}

TEST_F(PlayScriptedCloudLaunch, LeavesNoLaunchProgressForALaterHomeStart) {
  play::testing::ScriptedLink at_home{LiveProviders::threading(), LiveProviders::steady()};
  RecordedEvents home_events;
  RecordedMedia home_media;
  at_home.launch_progress(
      *LaunchProgress::at(service, LaunchActivity::reserving_server));
  at_home.connect();
  const auto again = home_.play.start_scripted(player_, Where::home, at_home,
                                               {}, home_media, home_events);
  ASSERT_TRUE(again);
  home_events.until_state(SessionState::playing);
  EXPECT_EQ(home_events.count("launch_progress"), 0U);
  EXPECT_TRUE(again.value()->leave());
  EXPECT_TRUE(again.value()->wait_until_ended().was_left());
}

TEST(PlayCloudStart, LaunchesThroughTheCatalogWithTypedProgress) {
  Household home;
  const auto player = SignedInPlayer::fresh();
  const auto chosen_product = Title::product_id(Household::cloud_title);
  const auto started = home.play.start_on_cloud(player, chosen_product, {},
                                                LaunchCase::named("dc1"),
                                                home.media, home.events);
  ASSERT_TRUE(started);
  const auto session = started.value();
  EXPECT_EQ(session->where(), Where::cloud);
  EXPECT_EQ(session->console(), nullptr);
  auto* launch = session->launch();
  ASSERT_NE(launch, nullptr);
  EXPECT_EQ(launch->product(), chosen_product);
  EXPECT_EQ(launch->service(), catalog::ServiceGeneration::ps5_cloud);

  home.events.until("datacenter_chosen");
  EXPECT_GE(home.events.count("launch_progress"), 3U);
  const auto chosen = launch->chosen();
  ASSERT_TRUE(chosen);
  EXPECT_EQ(chosen->name(), "dc1");
  EXPECT_TRUE(chosen->was_forced());
  EXPECT_EQ(home.events.count("datacenters_offered"), 0U);
  static_cast<void>(session->leave());
  const auto& outcome = session->wait_until_ended();
  EXPECT_FALSE(outcome.succeeded());
  EXPECT_EQ(home.clearance.clears, 1U);
  ASSERT_FALSE(home.clearance.cleared_products.empty());
  EXPECT_EQ(home.clearance.cleared_products.front(), chosen_product);
  EXPECT_TRUE(home.clearance.remembered.empty());
  EXPECT_EQ(home.ps.unscripted, 0U);
  EXPECT_EQ(home.ps.starts, 1U);
  EXPECT_EQ(home.datacenters.saves, 0U);
  EXPECT_TRUE(home.play.known_datacenters(catalog::ServiceGeneration::ps5_cloud)
                  .empty());
  EXPECT_EQ(home.play.live(), nullptr);
}

TEST(PlayCloudStart, ShowsTheAppEachLaunchWaitItMustRender) {
  const shared::Wait closing{shared::WaitReason::previous_session_closing,
                             std::chrono::seconds{10}};
  const std::vector scripted{
      *LaunchProgress::at(service, LaunchActivity::reserving_server,
                          {.step = shared::Position{7, 10}}),
      *LaunchProgress::at(service, LaunchActivity::reserving_server,
                          {.attempt = 1, .wait = closing}),
      *LaunchProgress::at(service, LaunchActivity::reserving_server,
                          {.attempt = 2, .wait = closing}),
      *LaunchProgress::at(service, LaunchActivity::measuring_datacenters,
                          {.step = shared::Position{8, 10},
                           .measured = shared::Position{1, 3}}),
      *LaunchProgress::at(
          service, LaunchActivity::waiting_for_datacenter_choice,
          {.wait = shared::Wait{shared::WaitReason::datacenter_choice,
                                std::chrono::seconds{15}}}),
      *LaunchProgress::at(
          service, LaunchActivity::allocating_server,
          {.queue_position = 3,
           .attempt = 1,
           .wait = shared::Wait{shared::WaitReason::server_queue,
                                std::chrono::seconds{15}}}),
      LaunchProgress::at(
          service, LaunchActivity::migrating_data,
          {.attempt = 2,
           .migration_percent = 40,
           .wait = shared::Wait{shared::WaitReason::data_migration,
                                std::chrono::seconds{5}}})
          ->after(std::chrono::seconds{23})};
  Household home;
  const auto player = SignedInPlayer::fresh();
  play::testing::ScriptedLink link{LiveProviders::threading(), LiveProviders::steady()};
  for (const auto& progress : scripted) link.launch_progress(progress);
  link.finish();
  const auto started = home.play.start_scripted(player, Where::cloud, link, {},
                                                home.media, home.events);
  ASSERT_TRUE(started);
  static_cast<void>(started.value()->wait_until_ended());
  const auto history = home.events.launch_history();
  EXPECT_EQ(history, scripted);
  ASSERT_FALSE(history.empty());
  EXPECT_EQ(history.back().status(),
            (shared::Status{std::nullopt, 2, std::chrono::seconds{23},
                            shared::Wait{shared::WaitReason::data_migration,
                                         std::chrono::seconds{5}}}));
  EXPECT_TRUE(home.events.progress().empty());
}

TEST(PlayCloudStart, PassesAHeldSessionLockOnAsTypedWaits) {
  using Activity = play::LaunchActivity;
  Household home;
  home.ps.busy_locks = 2;
  home.ps.lock_poll_seconds = 10;
  const auto player = SignedInPlayer::fresh();
  const auto started = home.play.start_on_cloud(
      player, Title::product_id(Household::cloud_title), {}, LaunchCase::named("dc1"),
      home.media, home.events);
  ASSERT_TRUE(started);
  home.events.until("datacenter_chosen");
  const auto history = home.events.launch_history();
  static_cast<void>(started.value()->leave());
  static_cast<void>(started.value()->wait_until_ended());

  const auto locking =
      std::ranges::find(history, std::optional{shared::Position{7, 10}},
                        &play::LaunchProgress::step);
  ASSERT_NE(locking, history.end());
  ASSERT_GT(history.end() - locking, 3);
  EXPECT_EQ(locking->activity(), Activity::reserving_server);
  EXPECT_FALSE(locking->wait());
  EXPECT_FALSE(locking->attempt());
  EXPECT_EQ(locking->status().attempt, 1U);
  EXPECT_FALSE(locking->status().is_waiting());
  for (std::uint32_t attempt = 1; attempt <= 2; ++attempt) {
    SCOPED_TRACE(attempt);
    const auto& waiting = locking[attempt];
    EXPECT_EQ(waiting.activity(), Activity::reserving_server);
    EXPECT_EQ(waiting.phase(), play::LaunchPhase::preparing);
    EXPECT_EQ(waiting.wait(),
              (shared::Wait{shared::WaitReason::previous_session_closing,
                            std::chrono::seconds{10}}));
    EXPECT_EQ(waiting.attempt(), attempt);
    EXPECT_TRUE(waiting.elapsed());
    EXPECT_EQ(waiting.status(),
              (shared::Status{std::nullopt, attempt, waiting.elapsed(),
                              waiting.wait()}));
  }
  EXPECT_EQ(locking[3].activity(), Activity::measuring_datacenters);
  EXPECT_FALSE(locking[3].wait());
  EXPECT_EQ(locking[3].step(), (shared::Position{8, 10}));
  EXPECT_EQ(std::ranges::count_if(history,
                                  [](const auto& progress) {
                                    return progress.wait().has_value();
                                  }),
            2);
  ASSERT_GE(home.waiter.waits.size(), 2U);
  EXPECT_EQ(home.waiter.waits[0], 10'000);
  EXPECT_EQ(home.waiter.waits[1], 10'000);
  EXPECT_EQ(home.ps.unscripted, 0U);
}

TEST(PlayCloudStart, EndsWithTheTypedRefusalWhenTheCatalogRefuses) {
  Household home;
  ScriptedClearance refusing{
      catalog::LaunchRefusal::needs_premium(libchiaking::users::Tier::extra)};
  home.answering.answer_with(refusing);
  const auto player = SignedInPlayer::fresh();
  const auto chosen_product = Title::product_id(Household::cloud_title);
  const auto started = home.play.start_on_cloud(player, chosen_product, {}, {},
                                                home.media, home.events);
  ASSERT_TRUE(started);
  const auto& outcome = started.value()->wait_until_ended();
  EXPECT_FALSE(outcome.succeeded());
  ASSERT_NE(outcome.launch_refused(), nullptr);
  EXPECT_EQ(outcome.launch_refused()->kind(),
            play::LaunchRefused::Kind::not_cleared);
  ASSERT_NE(started.value()->launch(), nullptr);
  EXPECT_TRUE(started.value()->launch()->refusal());
  EXPECT_EQ(refusing.clears, 1U);
  EXPECT_TRUE(refusing.remembered.empty());
  EXPECT_EQ(home.ps.starts, 0U);
  const auto typed = home.events.launch_progress();
  ASSERT_TRUE(typed);
  EXPECT_EQ(typed->service(), catalog::ServiceGeneration::ps5_cloud);
  EXPECT_EQ(typed->activity(), play::LaunchActivity::clearing_with_catalog);
  EXPECT_EQ(home.play.live(), nullptr);

  home.answering.answer_with(home.clearance);
  RecordedEvents next_events;
  const auto next = home.play.start_on_cloud(player, chosen_product, {},
                                             LaunchCase::named("dc1"),
                                             home.media, next_events);
  ASSERT_TRUE(next);
  next_events.until("datacenter_chosen");
  static_cast<void>(next.value()->leave());
  static_cast<void>(next.value()->wait_until_ended());
}

}  // namespace
}  // namespace play_test
