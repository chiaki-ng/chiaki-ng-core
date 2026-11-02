#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/openssl/adapters/links/internet/remote_pairing_rig.hpp"
#include "tests/support/stop_states.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string_view>
#include <vector>

namespace play_test {
namespace {

namespace consoles = libchiaking::consoles;
namespace platform = libchiaking::platform;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using Rig = RemotePairingRig;
using FailureStage = Rig::FailureStage;
using Stage = Rig::Stage;
using StreamError = Rig::StreamError;

class InternetPairingAttempt : public ::testing::Test {
 protected:
  static auto session(std::shared_ptr<Rig::Script> script)
      -> std::unique_ptr<play::Link> {
    return std::make_unique<Rig::Session>(std::move(script));
  }

  Rig rig_;
};

static_assert(Rig::Pairing::wait_step == std::chrono::milliseconds{100});

TEST_F(InternetPairingAttempt, ReturnsTheRegisteredConsole) {
  auto registrar = rig_.pairing();
  EXPECT_EQ(registrar.stage(), Stage::starting);
  auto script = std::make_shared<Rig::Script>();
  script->pumps.push_back(Rig::progress(FailureStage::psn_start));
  script->pumps.push_back(Rig::progress(FailureStage::control_holepunch));
  script->pumps.push_back(Rig::registered());
  std::vector<Stage> seen;
  script->on_pump = [&] { seen.push_back(registrar.stage()); };
  const auto result = registrar.run(session(script));
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().target, remote_play::RegistrationTarget::ps5_1);
  EXPECT_EQ(result.value().mac[0], std::byte{0x7a});
  EXPECT_EQ(result.value().mac[5], std::byte{0x5f});
  EXPECT_EQ(result.value().registration_key[0], std::byte{1});
  EXPECT_EQ(result.value().registration_key[15], std::byte{16});
  EXPECT_EQ(result.value().key[0], std::byte{0xa0});
  EXPECT_EQ(result.value().key[15], std::byte{0xaf});
  EXPECT_EQ(result.value().key_type, 2);
  EXPECT_EQ(std::string_view{result.value().nickname.data()}, "PS4-Living");
  EXPECT_EQ(std::string_view{result.value().ap_key.data()}, "5f4e3d2c");
  EXPECT_EQ(result.value().console_pin, 24680U);
  EXPECT_EQ(result.value().mac[1], std::byte{0x1b});
  EXPECT_EQ(result.value().mac[4], std::byte{0x4e});
  EXPECT_EQ(script->pumped, 3);
  EXPECT_EQ(script->stops, 1);
  EXPECT_EQ(seen, (std::vector{Stage::opening_session, Stage::waking_console,
                               Stage::connecting}));
  EXPECT_EQ(registrar.stage(), Stage::pairing);

  const auto repeated = registrar.run(session(std::make_shared<Rig::Script>()));
  ASSERT_FALSE(repeated);
  EXPECT_TRUE(repeated.error().cancelled);
}

TEST_F(InternetPairingAttempt, KeepsTheStageAndDetailOfAFailure) {
  auto registrar = rig_.pairing();
  auto script = std::make_shared<Rig::Script>();
  script->pumps.push_back(Rig::progress(FailureStage::psn_start));
  script->failure.emplace(FailureStage::control_holepunch,
                          StreamError::route_failed,
                          play::LinkFailure::ConsoleRefusal{0x80108b12, 0});
  const auto result = registrar.run(session(script));
  ASSERT_FALSE(result);
  const auto& failure = result.error();
  EXPECT_FALSE(failure.cancelled);
  EXPECT_EQ(failure.error, StreamError::route_failed);
  EXPECT_EQ(failure.stage, Stage::connecting);
  ASSERT_TRUE(failure.detail);
  EXPECT_EQ(failure.detail->route(), play::Venue::internet());
  EXPECT_EQ(failure.detail->stage(), FailureStage::control_holepunch);
  EXPECT_EQ(script->stops, 1);
}

TEST_F(InternetPairingAttempt, StopsASessionThatWouldNotStart) {
  auto unstarted = rig_.pairing();
  auto refused = std::make_shared<Rig::Script>();
  refused->start_result =
      libchiaking::unexpected(StreamError::provider_unavailable);
  const auto start = unstarted.run(session(refused));
  ASSERT_FALSE(start);
  EXPECT_EQ(start.error().error, StreamError::provider_unavailable);
  EXPECT_FALSE(start.error().detail);
  EXPECT_EQ(refused->pumped, 0);
  EXPECT_EQ(refused->stops, 1);
}

TEST_F(InternetPairingAttempt, DoesNotRunOnceCancelled) {
  auto registrar = rig_.pairing();
  registrar.cancel();
  auto script = std::make_shared<Rig::Script>();
  const auto result = registrar.run(session(script));
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().cancelled);
  EXPECT_EQ(script->pumped, 0);
}

TEST_F(InternetPairingAttempt, WakesAndStopsTheSessionWhenCancelledMidway) {
  auto registrar = rig_.pairing();
  auto script = std::make_shared<Rig::Script>();
  script->pumps.push_back(Rig::progress(FailureStage::psn_start));
  script->pumps.push_back(Rig::registered());
  script->on_pump = [&] {
    if (script->pumped == 2) registrar.cancel();
  };
  const auto result = registrar.run(session(script));
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().cancelled);
  EXPECT_GE(script->wakes, 1);
  EXPECT_EQ(script->stops, 1);
}

TEST_F(InternetPairingAttempt, RefusesAMissingSessionOrAnAnonymousRequest) {
  auto registrar = rig_.pairing();
  const auto missing = registrar.run(std::unique_ptr<play::Link>{});
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().error, StreamError::invalid_argument);
  Rig::Pairing::Request anonymous;
  const auto refused = registrar.run(std::move(anonymous));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().error, StreamError::invalid_configuration);
}

TEST_F(InternetPairingAttempt, TellsTheWatcherEachStageOnce) {
  StagesHeard heard;
  auto pairing = rig_.pairing(&heard);
  auto script = std::make_shared<Rig::Script>();
  script->pumps.push_back(Rig::progress(FailureStage::psn_open));
  script->pumps.push_back(Rig::progress(FailureStage::psn_start));
  script->pumps.push_back(Rig::progress(FailureStage::control_holepunch));
  script->pumps.push_back(Rig::progress(FailureStage::control_holepunch));
  script->pumps.push_back(Rig::registered());
  EXPECT_TRUE(pairing.run(session(script)));
  EXPECT_EQ(heard.stages,
            (std::vector{Stage::opening_session, Stage::waking_console,
                         Stage::connecting, Stage::pairing}));
}

class StepsHeard final : public consoles::PairingWatcher {
 public:
  void on_progress(const consoles::PairingProgress& progress) override {
    heard.push_back(progress);
  }

  std::vector<consoles::PairingProgress> heard;
};

TEST_F(InternetPairingAttempt, TellsTheWatcherTypedStepsAndRouteSubSteps) {
  using remote_play::event::SetupProgress;
  using Route = libchiaking::progress::InternetRoute;
  using Step = consoles::PairingStep;
  namespace shared = libchiaking::progress;
  const std::vector<SetupProgress> scripted{
      {FailureStage::psn_open,
       10,
       {},
       Route{Route::Step::opening_account_session}},
      {FailureStage::control_holepunch,
       20,
       {},
       Route{Route::Step::gathering_candidates}},
      {FailureStage::psn_start,
       30,
       {},
       Route{Route::Step::waiting_for_console}},
      {FailureStage::control_holepunch,
       40,
       {},
       Route{Route::Step::exchanging_offers, Route::Purpose::control,
             Route::Candidates{1, 2, 0}}},
      {FailureStage::control_holepunch,
       50,
       {},
       Route{Route::Step::testing_candidates, Route::Purpose::control,
             Route::Candidates{2, 1, 75}}},
      {FailureStage::control_holepunch,
       60,
       {},
       Route{Route::Step::route_chosen, Route::Purpose::control, std::nullopt,
             Route::Kind::port_guessed}},
      {FailureStage::control_holepunch,
       70,
       {},
       Route{Route::Step::confirming_route, Route::Purpose::control,
             std::nullopt, Route::Kind::port_guessed}},
      {FailureStage::registration, 80, {}, std::nullopt},
      {FailureStage::media, 90, {}, std::nullopt}};
  StepsHeard heard;
  auto pairing = rig_.pairing(&heard);
  auto script = std::make_shared<Rig::Script>();
  for (const auto& event : scripted) {
    std::vector<Rig::StreamEvent> pump;
    pump.emplace_back(event);
    script->pumps.push_back(std::move(pump));
  }
  script->pumps.push_back(Rig::registered());
  EXPECT_TRUE(pairing.run(session(script)));

  const std::vector expected{
      Step::opening_account_session, Step::reaching_console,
      Step::waking_console,          Step::reaching_console,
      Step::reaching_console,        Step::reaching_console,
      Step::reaching_console,        Step::sending_request};
  ASSERT_EQ(heard.heard.size(), expected.size() + 1);
  EXPECT_EQ(heard.heard.front(),
            (consoles::PairingProgress{.step = Step::opening_account_session}));
  for (std::size_t index = 0; index < expected.size(); ++index) {
    SCOPED_TRACE(index);
    const auto& told = heard.heard[index + 1];
    EXPECT_EQ(told.step, expected[index]);
    EXPECT_EQ(told.attempt, 1U);
    EXPECT_EQ(told.route, scripted[index].route);
    EXPECT_EQ(told.elapsed, std::chrono::microseconds{
                                static_cast<int>(scripted[index].elapsed_us)});
    if (told.step == Step::waking_console)
      EXPECT_EQ(told.wait, (shared::Wait{shared::WaitReason::console_joining,
                                         std::nullopt}));
    else
      EXPECT_FALSE(told.wait);
    EXPECT_EQ(told.status(),
              (shared::Status{std::nullopt, 1, told.elapsed, told.wait}));
  }
  ASSERT_TRUE(heard.heard[5].route);
  ASSERT_TRUE(heard.heard[5].route->candidates);
  EXPECT_EQ(heard.heard[5].route->candidates->total(), 78U);
  ASSERT_TRUE(heard.heard[6].route);
  EXPECT_EQ(heard.heard[6].route->chosen, Route::Kind::port_guessed);
}

TEST_F(InternetPairingAttempt, IsCancelledByACallerStoppedBeforeItRuns) {
  auto pairing = rig_.pairing(nullptr, test_support::Stopped::token());
  auto script = std::make_shared<Rig::Script>();
  const auto result = pairing.run(session(script));
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().cancelled);
  EXPECT_EQ(script->pumped, 0);
  EXPECT_EQ(result.error().typed().reason,
            consoles::PairingFailure::Reason::cancelled);
}

TEST_F(InternetPairingAttempt, IsCancelledWhenTheCallerStopsMidway) {
  platform::StopSource caller;
  auto pairing = rig_.pairing(nullptr, caller.token());
  auto script = std::make_shared<Rig::Script>();
  script->pumps.push_back(Rig::progress(FailureStage::psn_start));
  script->pumps.push_back(Rig::registered());
  script->on_pump = [&] {
    if (script->pumped == 1) caller.request_stop();
  };
  const auto result = pairing.run(session(script));
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().cancelled);
  EXPECT_EQ(script->pumped, 1);
  EXPECT_EQ(script->stops, 1);
}

}  // namespace
}  // namespace play_test
