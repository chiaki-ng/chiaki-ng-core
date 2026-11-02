#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/internet_link_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace play_test {
namespace {

using internet_links::RemoteCtrlRig;
using internet_links::Target;
namespace internet = libchiaking::play::internet;
namespace remote_play = libchiaking::net::protocol::remote_play;
using Progress = internet::remote_ctrl::EffectProgress;

class InternetLinkCtrl : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(rig_.created());
    rig_.crypto().allow_registration_request = true;
  }

  auto rig() -> RemoteCtrlRig& { return rig_; }

 private:
  RemoteCtrlRig rig_;
};

TEST_F(InternetLinkCtrl, RefusesAFrameBeforeTheSessionId) {
  const auto connect = remote_play::CtrlCodec::microphone_connect_frame();
  EXPECT_FALSE(rig().session().send_ctrl_frame(rig().channel(), rig().crypto(),
                                               Target::ps5_1, connect.type,
                                               connect.bytes()));
  EXPECT_TRUE(rig().channel().history().empty());
}

TEST_F(InternetLinkCtrl, SendsMicrophoneFramesAfterTheSessionId) {
  const auto connect = remote_play::CtrlCodec::microphone_connect_frame();
  const auto mute = remote_play::CtrlCodec::microphone_mute_frame(true);
  auto& session = rig().session();
  ASSERT_FALSE(session.send_ctrl_frame(rig().channel(), rig().crypto(),
                                       Target::ps5_1, connect.type,
                                       connect.bytes()));
  ASSERT_NO_FATAL_FAILURE(rig().fall_back_to_session_id(Target::ps5_1));
  const auto before = session.outbound_state();
  EXPECT_TRUE(session.send_ctrl_frame(rig().channel(), rig().crypto(),
                                      Target::ps5_1, connect.type,
                                      connect.bytes()));
  rig().channel().fail_sends(true);
  const auto failed = session.send_ctrl_frame(
      rig().channel(), rig().crypto(), Target::ps5_1, mute.type, mute.bytes());
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(),
            internet::remote_ctrl::OutboundStepError::transport);
  rig().channel().fail_sends(false);
  EXPECT_TRUE(session.send_ctrl_frame(rig().channel(), rig().crypto(),
                                      Target::ps5_1, mute.type, mute.bytes()));
  ASSERT_EQ(rig().channel().history().size(), 3U);
  constexpr std::array<std::uint16_t, 3> types{0x30, 0x36, 0x36};
  const std::array<std::vector<std::byte>, 3> payloads{{
      {std::byte{0}, std::byte{0}},
      {std::byte{0}, std::byte{1}, std::byte{1}, std::byte{89}},
      {std::byte{0}, std::byte{1}, std::byte{1}, std::byte{89}},
  }};
  rig().expect_sent(before.local_rudp_counter, types, payloads);
  EXPECT_EQ(session.outbound_state().crypt_counter, before.crypt_counter + 3);
  EXPECT_EQ(session.pump_state().local_counter,
            session.outbound_state().local_rudp_counter);
}

class InternetLinkCtrlFeatures : public InternetLinkCtrl {
 protected:
  void hold_a_command() {
    ASSERT_TRUE(features_.queue_command(remote_play::command::GotoBed{}));
    ASSERT_TRUE(flush());
    EXPECT_TRUE(rig().channel().history().empty());
    EXPECT_TRUE(features_.front_outbound());
  }

  void send_post_id_features() {
    ASSERT_NO_FATAL_FAILURE(hold_a_command());
    ASSERT_TRUE(rig().deliver(0x33, RemoteCtrlRig::session_id_bytes()));
    ASSERT_TRUE(rig().session().send_post_id_features(
        rig().channel(), rig().crypto(), Target::ps5_1, true, true));
    post_id_ = rig().recorded_frames();
  }

  void repeat_post_id_features() {
    ASSERT_NO_FATAL_FAILURE(send_post_id_features());
    ASSERT_TRUE(rig().deliver(0x33, {}));
    ASSERT_TRUE(flush(true, true));
    const auto repeated = rig().recorded_frames(post_id_.size());
    ASSERT_EQ(repeated.size(), post_id_.size() + 1);
    EXPECT_TRUE(std::equal(post_id_.begin(), post_id_.end(), repeated.begin()));
    EXPECT_EQ(repeated.back(), (remote_play::CtrlOutboundFrame{0x50, {}}));
    rig().channel().keep_first(post_id_.size());
  }

  void send_keyboard_and_home() {
    ASSERT_NO_FATAL_FAILURE(repeat_post_id_features());
    ASSERT_TRUE(features_.queue_command(remote_play::command::KeyboardSetText{
        libchiaking::SecretString{"hi"}}));
    ASSERT_TRUE(features_.queue_command(remote_play::command::GoHome{}));
    ASSERT_TRUE(flush());
  }

  auto flush(bool dualsense = false, bool keyboard = false) -> bool {
    return static_cast<bool>(internet::remote_ctrl::Effects::flush_outbound(
        rig().session(), rig().channel(), rig().crypto(), Target::ps5_1,
        features_, dualsense, keyboard));
  }

  auto features() -> remote_play::CtrlFeatures& { return features_; }
  auto post_id() const -> const std::vector<remote_play::CtrlOutboundFrame>& {
    return post_id_;
  }

 private:
  remote_play::CtrlFeatures features_;
  std::vector<remote_play::CtrlOutboundFrame> post_id_;
};

TEST_F(InternetLinkCtrlFeatures, HoldsQueuedCommandsUntilTheSessionIdArrives) {
  hold_a_command();
}

TEST_F(InternetLinkCtrlFeatures, SendsThePostIdFeaturesOnceTheIdArrives) {
  ASSERT_NO_FATAL_FAILURE(send_post_id_features());
  EXPECT_EQ(post_id(), remote_play::CtrlCodec::post_id_frames(true, true));
  ASSERT_GE(post_id().size(), 4U);
  EXPECT_EQ(post_id()[2].type, 0xd);
  EXPECT_EQ(post_id()[3].type, 0x20);
}

TEST_F(InternetLinkCtrlFeatures,
       RepeatsThePostIdFeaturesBeforeTheHeldCommandOnADuplicateId) {
  repeat_post_id_features();
}

TEST_F(InternetLinkCtrlFeatures, SendsQueuedKeyboardTextAndHomeCommands) {
  ASSERT_NO_FATAL_FAILURE(send_keyboard_and_home());
  EXPECT_FALSE(features().front_outbound());
  const auto sent = rig().recorded_frames(post_id().size());
  ASSERT_EQ(sent.size(), 2U);
  EXPECT_EQ(sent[0],
            remote_play::CtrlCodec::encode_keyboard_set_text(1, "hi").value());
  EXPECT_EQ(sent[1], remote_play::CtrlCodec::encode_go_home());
}

TEST_F(InternetLinkCtrlFeatures, TurnsKeyboardNoticesIntoEvents) {
  ASSERT_NO_FATAL_FAILURE(send_keyboard_and_home());
  std::vector<std::byte> open(0x20);
  open[0x1f] = std::byte{1};
  open.push_back(std::byte{'z'});
  ASSERT_TRUE(rig().deliver(0x21, open));
  ASSERT_TRUE(rig().deliver(0x22, {}));
  const auto stepped = rig().step();
  ASSERT_TRUE(stepped.progress);
  EXPECT_EQ(stepped.progress.value(), Progress::idle);
  std::vector<remote_play::StreamEvent> events;
  internet::remote_ctrl::Effects::drain_notices(rig().session(), features(),
                                                events);
  ASSERT_EQ(events.size(), 2U);
  ASSERT_TRUE(
      std::holds_alternative<remote_play::event::KeyboardOpen>(events[0]));
  EXPECT_EQ(std::get<remote_play::event::KeyboardOpen>(events[0]).text, "z");
  EXPECT_TRUE(std::holds_alternative<remote_play::event::KeyboardRemoteClose>(
      events[1]));
}

TEST(InternetLinkCtrlDisplay, ReportsDisplayProhibitionLikeTheReference) {
  remote_play::CtrlFeatures features;
  std::vector<remote_play::StreamEvent> events;
  const auto deliver = [&](std::uint16_t type, std::vector<std::byte> payload) {
    auto notice = remote_play::CtrlCodec::decode_notice(type, payload);
    ASSERT_TRUE(notice);
    features.on_notice(std::move(*notice), events);
  };
  const auto prohibited = [&](std::size_t index) {
    return std::get<remote_play::event::DisplayProhibited>(events[index])
        .prohibited;
  };
  deliver(0x1, {std::byte{0}});
  ASSERT_EQ(events.size(), 1U);
  EXPECT_FALSE(prohibited(0));
  deliver(0x16, {std::byte{0}, std::byte{0}});
  deliver(0x1, {std::byte{1}});
  deliver(0x16, {std::byte{1}, std::byte{0xff}});
  EXPECT_EQ(events.size(), 1U);
  deliver(0x16, {std::byte{2}, std::byte{0}});
  ASSERT_EQ(events.size(), 2U);
  EXPECT_TRUE(prohibited(1));
  deliver(0x16, {std::byte{2}, std::byte{0}});
  deliver(0x1, {std::byte{0}});
  EXPECT_EQ(events.size(), 2U);
  deliver(0x16, {std::byte{1}, std::byte{0xff}});
  deliver(0x1, {std::byte{0}});
  ASSERT_EQ(events.size(), 3U);
  EXPECT_FALSE(prohibited(2));
  features.on_rp_prohibit(false, events);
  EXPECT_EQ(events.size(), 3U);
  features.on_rp_prohibit(true, events);
  ASSERT_EQ(events.size(), 4U);
  EXPECT_TRUE(prohibited(3));
}

TEST(InternetLinkCtrlDisplay, StartsProhibitedWhenTheStartupSaysSo) {
  internet::remote_ctrl::StartupResult startup{};
  startup.final.response.prohibited = true;
  RemoteCtrlRig rig{std::move(startup)};
  ASSERT_TRUE(rig.created());
  EXPECT_TRUE(rig.session().rp_prohibited());
}

class InternetLinkCtrlPostId
    : public ::testing::TestWithParam<std::pair<Target, bool>> {
 protected:
  static constexpr std::array<std::uint16_t, 5> types{0x13, 0x11, 0x36, 0x36,
                                                      0x910};

  void SetUp() override {
    ASSERT_TRUE(rig_.created());
    rig_.crypto().allow_registration_request = true;
  }

  static auto payloads() -> std::array<std::vector<std::byte>, 5> {
    return {{
        {std::byte{0}, std::byte{0x40}, std::byte{0}},
        {std::byte{0xa0}, std::byte{0xab}, std::byte{0x51}, std::byte{0xbd},
         std::byte{0xd1}, std::byte{0x7e}, std::byte{0}, std::byte{0},
         std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
         std::byte{0xff}, std::byte{0}, std::byte{0}, std::byte{0}},
        {std::byte{0}, std::byte{1}, std::byte{1}, std::byte{0x59}},
        {std::byte{0}, std::byte{1}, std::byte{1}, std::byte{0x59}},
        {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}},
    }};
  }

  auto send() -> bool {
    return static_cast<bool>(rig_.session().send_post_id_features(
        rig_.channel(), rig_.crypto(), GetParam().first, GetParam().second));
  }

  auto rig() -> RemoteCtrlRig& { return rig_; }

 private:
  RemoteCtrlRig rig_;
};

TEST_P(InternetLinkCtrlPostId, RefusesTheFeaturesBeforeTheSessionId) {
  EXPECT_FALSE(send());
}

TEST_P(InternetLinkCtrlPostId, SendsTheFeaturesOnceAfterTheSessionId) {
  const auto [target, dualsense] = GetParam();
  ASSERT_FALSE(send());
  ASSERT_NO_FATAL_FAILURE(rig().fall_back_to_session_id(target));
  auto& session = rig().session();
  const auto before = session.outbound_state();
  rig().channel().fail_sends(true);
  EXPECT_TRUE(send());
  rig().channel().fail_sends(false);
  const std::size_t offset = dualsense ? 0 : 2;
  const auto expected_payloads = payloads();
  ASSERT_EQ(rig().channel().history().size(), types.size() - offset);
  rig().expect_sent(before.local_rudp_counter, std::span{types}.subspan(offset),
                    std::span{expected_payloads}.subspan(offset));
  EXPECT_EQ(session.outbound_state().crypt_counter,
            before.crypt_counter + rig().channel().history().size());
  EXPECT_EQ(session.pump_state().local_counter,
            session.outbound_state().local_rudp_counter);
  EXPECT_TRUE(send());
  EXPECT_EQ(rig().channel().history().size(), types.size() - offset);
}

INSTANTIATE_TEST_SUITE_P(TargetsWithAndWithoutDualSense, InternetLinkCtrlPostId,
                         ::testing::Values(std::pair{Target::ps5_1, true},
                                           std::pair{Target::ps5_1, false},
                                           std::pair{Target::ps4_10, true},
                                           std::pair{Target::ps4_10, false}));

class InternetLinkCtrlPin : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(rig_.created()); }

  void apply_switch_counters() {
    auto& session = rig_.session();
    EXPECT_FALSE(session.apply_switch_counters(8, 11, 4));
    EXPECT_TRUE(session.apply_switch_counters(7, 11, 0));
    EXPECT_EQ(session.pump_state().local_counter, 0);
    EXPECT_TRUE(session.apply_switch_counters(7, 11, 4));
    EXPECT_EQ(session.pump_state().remote_crypt_counter, 11U);
    EXPECT_EQ(session.pump_state().local_counter, 4);
    EXPECT_EQ(session.outbound_state().local_rudp_counter, 4);
    EXPECT_FALSE(session.apply_switch_counters(7, 10, 5));
  }

  auto rig() -> RemoteCtrlRig& { return rig_; }

 private:
  RemoteCtrlRig rig_;
};

TEST_F(InternetLinkCtrlPin, AppliesSwitchCountersOnlyForItsOwnAttempt) {
  apply_switch_counters();
}

TEST_F(InternetLinkCtrlPin, ReportsAPinRequestOnceThenAnswersAHeartbeat) {
  apply_switch_counters();
  ASSERT_TRUE(rig().accept(remote_play::CtrlLoginPinRequest{}));
  const auto first = rig().step();
  ASSERT_TRUE(first.progress);
  EXPECT_EQ(first.progress.value(), Progress::pin_required);
  EXPECT_TRUE(first.pin_notified);
  EXPECT_TRUE(rig().channel().sent().empty());
  const auto again = rig().step(first.pin_notified);
  ASSERT_TRUE(again.progress);
  EXPECT_EQ(again.progress.value(), Progress::idle);
  EXPECT_TRUE(rig().session().inbox().front_effect());
  static_cast<void>(rig().session().inbox().take_effect());
  ASSERT_TRUE(rig().accept(remote_play::CtrlHeartbeatRequest{}));
  const auto replied = rig().step(again.pin_notified);
  ASSERT_TRUE(replied.progress);
  EXPECT_EQ(replied.progress.value(), Progress::idle);
  EXPECT_FALSE(rig().channel().sent().empty());
  EXPECT_FALSE(rig().session().inbox().front_effect());
}

TEST_F(InternetLinkCtrlPin, FailsOnAPinRequestAfterTheSessionId) {
  constexpr std::string_view raw_id = "Jabcdefghijklmnopqrstuvwx";
  ASSERT_TRUE(rig().deliver(0x33, std::as_bytes(std::span{raw_id})));
  static_cast<void>(rig().session().inbox().take_effect());
  ASSERT_TRUE(rig().accept(remote_play::CtrlLoginPinRequest{}));
  ASSERT_TRUE(rig().accept(remote_play::CtrlHeartbeatRequest{}));
  const auto flagged = rig().step();
  ASSERT_TRUE(flagged.progress);
  EXPECT_EQ(flagged.progress.value(), Progress::ctrl_failed);
  EXPECT_FALSE(flagged.pin_notified);
  const auto replied = rig().step(flagged.pin_notified);
  ASSERT_TRUE(replied.progress);
  EXPECT_EQ(replied.progress.value(), Progress::idle);
  EXPECT_FALSE(rig().channel().sent().empty());
}

}  // namespace
}  // namespace play_test
