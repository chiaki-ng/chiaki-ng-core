#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/internet_link_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace play_test {
namespace {

using internet_links::PairingsHeard;
using internet_links::ScriptedRemote;
using internet_links::Target;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace script = remote_start_script;
using StreamState = remote_play::StreamState;

class PairingTest : public internet_links::ScriptedInternetLinkTest {
 protected:
  static auto pairing_script() -> std::vector<std::string_view> {
    return {script::created_body,       script::members_body,
            script::ready_members_body, script::ready_custom_body,
            script::route_offer_body,   script::route_result_body,
            script::route_accept_body,  script::members_deleted_body};
  }

  static void answer(ScriptedRemote<>& rig, Target target) {
    rig.http().start_status = 200;
    rig.crypto().allow_registration_request = true;
    rig.answer_control_route();
    rig.sockets().registration_ps4 = target == Target::ps4_10;
  }

  static auto configured(Target target) -> play::InternetLinkConfiguration {
    auto config = ScriptedRemote<>::configuration(target);
    config.enable_ipv6 = false;
    return config;
  }
};

class InternetLinkPairingOnly : public PairingTest,
                                public ::testing::WithParamInterface<Target> {};

TEST_P(InternetLinkPairingOnly, RegistersTheConsoleAndStopsBeforeCtrl) {
  const auto target = GetParam();
  ScriptedRemote<> rig{pairing_script()};
  answer(rig, target);
  auto* link = rig.open_pairing(configured(target));
  ASSERT_NE(link, nullptr);
  ASSERT_TRUE(link->start());
  EXPECT_EQ(link->state(), StreamState::stopped);
  EXPECT_FALSE(link->failure());
  const auto& heard = rig.heard();
  EXPECT_EQ(heard.registered_events, 1U);
  EXPECT_TRUE(heard.registered_before_state);
  EXPECT_EQ(heard.registered_target, target);
  EXPECT_EQ(heard.registered_key_tail, std::byte{0x0f});
  EXPECT_EQ(heard.registered_mac_tail, std::byte{0x55});
  EXPECT_TRUE(heard.nicknames.empty());
  EXPECT_TRUE(heard.holepunch_finished.empty());
  EXPECT_EQ(heard.events, 1U);
  ASSERT_TRUE(
      std::holds_alternative<remote_play::event::StateChanged>(heard.last));
  EXPECT_EQ(std::get<remote_play::event::StateChanged>(heard.last).state,
            StreamState::stopped);
  const auto& sockets = rig.sockets();
  EXPECT_EQ(sockets.registration_http_sends, 1U);
  EXPECT_EQ(sockets.ctrl_http_sends, 0U);
  EXPECT_EQ(sockets.creates, 1U);
  EXPECT_EQ(sockets.closes, 1U);
  EXPECT_EQ(rig.counts().close, 1U);
  EXPECT_EQ(rig.requests("release"), 1);
  EXPECT_FALSE(link->pump_once());
  EXPECT_TRUE(link->stop());
}

TEST_P(InternetLinkPairingOnly, StopsQuietlyFromTheRegisteredEvent) {
  const auto target = GetParam();
  ScriptedRemote<> rig{pairing_script()};
  answer(rig, target);
  const PairingsHeard pairings{false};
  auto* link = rig.open_pairing(configured(target));
  ASSERT_NE(link, nullptr);
  rig.sink().stop_inside([link] { EXPECT_TRUE(link->stop()); }, true, false);
  ASSERT_TRUE(link->start());
  EXPECT_EQ(link->state(), StreamState::stopped);
  EXPECT_FALSE(link->failure());
  const auto& heard = rig.heard();
  EXPECT_EQ(heard.registered_events, 1U);
  EXPECT_TRUE(heard.nicknames.empty());
  EXPECT_TRUE(pairings.heard().empty());
  EXPECT_EQ(heard.failed_events, 0U);
  EXPECT_EQ(heard.stopped_events, 1U);
  EXPECT_EQ(rig.sockets().ctrl_http_sends, 0U);
  EXPECT_FALSE(link->pump_once());
  EXPECT_TRUE(link->stop());
  EXPECT_EQ(heard.stopped_events, 1U);
  EXPECT_EQ(heard.failed_events, 0U);
}

INSTANTIATE_TEST_SUITE_P(Ps5AndPs4, InternetLinkPairingOnly,
                         ::testing::Values(Target::ps5_1, Target::ps4_10));

class InternetLinkPairing : public PairingTest {};

TEST_F(InternetLinkPairing, TellsItsListenerAboutThePairedPs4) {
  ScriptedRemote<> rig{pairing_script()};
  answer(rig, Target::ps4_10);
  PairingsHeard pairings{false};
  auto* link = rig.open(configured(Target::ps4_10), &pairings);
  ASSERT_NE(link, nullptr);
  rig.sink().stop_inside([link] { EXPECT_TRUE(link->stop()); }, false, true);
  ASSERT_TRUE(link->start());
  EXPECT_EQ(link->state(), StreamState::stopped);
  EXPECT_FALSE(link->failure());
  const auto& heard = rig.heard();
  EXPECT_EQ(heard.registered_events, 0U);
  EXPECT_EQ(heard.nicknames.size(), 1U);
  ASSERT_EQ(pairings.heard().size(), 1U);
  const auto& paired = pairings.heard().front();
  EXPECT_EQ(paired.hardware_id.bytes()[5], std::byte{0x55});
  EXPECT_EQ(paired.firmware.generation(),
            libchiaking::consoles::Generation::ps4);
  EXPECT_EQ(paired.keys.registration_key()[15], std::byte{0x0f});
  EXPECT_EQ(heard.failed_events, 0U);
  EXPECT_EQ(heard.stopped_events, 1U);
  EXPECT_EQ(rig.sockets().ctrl_http_sends, 0U);
  EXPECT_FALSE(link->pump_once());
  EXPECT_TRUE(link->stop());
  EXPECT_EQ(heard.stopped_events, 1U);
  EXPECT_EQ(heard.failed_events, 0U);
}

TEST_F(InternetLinkPairing, FailsWhenItsListenerRefusesThePairing) {
  ScriptedRemote<> rig{pairing_script()};
  answer(rig, Target::ps4_10);
  PairingsHeard pairings{true};
  auto* link = rig.open(configured(Target::ps4_10), &pairings);
  ASSERT_NE(link, nullptr);
  rig.sink().stop_inside([link] { EXPECT_TRUE(link->stop()); }, false, true);
  EXPECT_FALSE(link->start());
  EXPECT_EQ(link->state(), StreamState::failed);
  const auto& failure = link->failure();
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->end_cause(),
            libchiaking::play::EndReason::Cause::unpaired_console);
  EXPECT_EQ(failure->end_reason().step(),
            libchiaking::play::EndReason::Step::pairing);
  EXPECT_EQ(pairings.heard().size(), 1U);
  EXPECT_TRUE(rig.heard().nicknames.empty());
  EXPECT_EQ(rig.heard().failed_events, 1U);
  EXPECT_EQ(rig.sockets().ctrl_http_sends, 0U);
}

}  // namespace
}  // namespace play_test
