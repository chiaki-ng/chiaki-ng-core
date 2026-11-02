#include "src/core/net/protocol/remote_play/rudp_switch.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/internet_link_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace play_test {

using test_support::LiveProviders;
namespace {

using internet_links::ScriptedRemote;
using internet_links::Target;
namespace platform = libchiaking::platform;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace script = remote_start_script;
using Stage = remote_play::FailureStage;
using StreamError = remote_play::StreamError;
using StreamState = remote_play::StreamState;

struct Scenario {
  Target target{Target::ps5_1};
  bool downgrade{};
  bool queue_session_id{true};
  bool pin_before_session_id{};
  unsigned extra_pin_requests{};
  bool answer_echo{true};
  bool fail_switch_send{};
};

class InternetLinkStreaming : public internet_links::ScriptedInternetLinkTest {
 protected:
  void start(const Scenario& scenario) {
    rig_.http().start_status = 200;
    rig_.crypto().allow_registration_request = true;
    rig_.crypto().allow_stream_prepare = true;
    generate_before_ = rig_.crypto().generate_calls;
    auto& sockets = rig_.sockets();
    sockets.registration_ps4 = scenario.target == Target::ps4_10;
    rig_.answer_control_route();
    sockets.queue_session_id = scenario.queue_session_id;
    sockets.queue_pin_before_session_id = scenario.pin_before_session_id;
    sockets.extra_pin_requests = scenario.extra_pin_requests;
    sockets.respond_to_takion_init = true;
    sockets.respond_to_takion_cookie = true;
    sockets.respond_to_takion_data = true;
    sockets.respond_to_takion_big = true;
    sockets.respond_to_senkusha_echo = scenario.answer_echo;
    sockets.respond_to_rtt_echo = true;
    sockets.respond_to_outbound_mtu_echo = true;
    sockets.respond_to_echo_disable = true;
    sockets.respond_to_inbound_mtu = true;
    sockets.respond_to_client_mtu = true;
    sockets.respond_to_client_mtu_final = true;
    sockets.respond_to_switch_request = true;
    sockets.allow_idle_poll = true;
    if (scenario.downgrade)
      sockets.ctrl_server_type = "AAAAAAAAAAAAAAAAAAAAAA==";
    auto config = ScriptedRemote<>::configuration(scenario.target);
    config.enable_ipv6 = scenario.target == Target::ps4_10;
    if (scenario.downgrade) {
      config.codec = remote_play::VideoCodec::h265;
      config.video.width = 1920;
      config.video.height = 1080;
      config.video.bitrate_kbps = 15000;
    }
    link_ = rig_.open(std::move(config));
    ASSERT_NE(link_, nullptr);
    ASSERT_TRUE(link_->start());
    EXPECT_EQ(sockets.ipv6_creates, 0U);
    ASSERT_NE(sockets.first, nullptr);
    sockets.first->fail_switch_send = scenario.fail_switch_send;
    if (scenario.downgrade) sockets.first->require_heartbeat_for_switch = true;
  }

  void pump_at(std::uint64_t now) {
    rig_.clock().now = now;
    EXPECT_TRUE(link_->pump_once());
  }

  void hold_the_data_route_until_the_fallback(bool stop) {
    auto& sockets = rig_.sockets();
    pump_at(10'000'099);
    EXPECT_EQ(sockets.creates, 1U);
    pump_at(10'000'100);
    EXPECT_EQ(sockets.creates, 2U);
    EXPECT_EQ(sockets.takion_init_sends, 0U);
    if (stop) return;
    pump_at(15'000'099);
    EXPECT_EQ(sockets.takion_init_sends, 0U);
    pump_at(15'000'100);
  }

  void defer_the_data_route() {
    auto& sockets = rig_.sockets();
    EXPECT_TRUE(link_->pump_once());
    EXPECT_TRUE(rig_.threading().pending);
    EXPECT_EQ(sockets.creates, 1U);
    auto& ctrl_socket = *sockets.first;
    for (unsigned index = 0; index < 3; ++index) {
      SCOPED_TRACE(index);
      ctrl_socket.queue_heartbeat(static_cast<std::uint16_t>(0x567d + index));
      EXPECT_TRUE(link_->pump_once());
      EXPECT_EQ(ctrl_socket.heartbeat_replies, index + 1);
      EXPECT_TRUE(rig_.threading().pending);
      EXPECT_EQ(sockets.creates, 1U);
    }
  }

  void pump_to_takion_init() {
    for (int index = 0; index < 80; ++index) {
      ASSERT_TRUE(link_->pump_once());
      if (rig_.sockets().takion_init_sends == 1) return;
    }
    FAIL() << "the Takion init was never sent";
  }

  void reach_streaming(bool session_key_mismatch = false) {
    auto& sockets = rig_.sockets();
    ASSERT_NO_FATAL_FAILURE(pump_to_takion_init());
    EXPECT_EQ(rig_.crypto().generate_calls, generate_before_ + 1);
    for (int index = 0; index < 3 && sockets.takion_cookie_sends < 1; ++index)
      ASSERT_TRUE(link_->pump_once());
    EXPECT_EQ(sockets.takion_cookie_sends, 1U);
    for (int index = 0; index < 5 && sockets.active_big_ack_sends < 1; ++index)
      ASSERT_TRUE(link_->pump_once());
    EXPECT_EQ(sockets.takion_big_sends, 1U);
    EXPECT_EQ(sockets.active_big_ack_sends, 1U);
    EXPECT_EQ(sockets.active_stream_info_sends, 1U);
    for (int index = 0; index < 20 && link_->state() != StreamState::streaming;
         ++index)
      ASSERT_TRUE(link_->pump_once());
    ASSERT_EQ(link_->state(), StreamState::streaming);
    EXPECT_EQ(rig_.diagnostics().contains("session key does not match"),
              session_key_mismatch);
  }

  void use_the_stream() {
    auto& sockets = rig_.sockets();
    remote_play::ControllerState controller{};
    controller.buttons =
        static_cast<std::uint32_t>(remote_play::ControllerButton::cross);
    const auto data_sends_before = sockets.last->send_calls;
    EXPECT_TRUE(link_->send_controller_state(controller));
    auto invalid_controller = controller;
    invalid_controller.controller_id = 4;
    const auto rejected = link_->send_controller_state(invalid_controller);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error(), StreamError::controller_rejected);
    EXPECT_TRUE(link_->pump_once());
    EXPECT_GT(sockets.last->send_calls, data_sends_before);
    EXPECT_EQ(link_->state(), StreamState::streaming);
    EXPECT_GE(sockets.distinct_takion_data_sends, 3U);
    EXPECT_EQ(sockets.takion_big_sends, 1U);
    EXPECT_EQ(sockets.senkusha_control_sends, 0U);
    EXPECT_EQ(sockets.takion_probe_sends, 0U);
    EXPECT_EQ(sockets.client_mtu_final_sends, 0U);
    auto& data_socket = *sockets.last;
    auto& ctrl_socket = *sockets.first;
    std::array<std::byte, 40> opus{};
    const auto short_frame =
        link_->send_microphone(std::span<const std::byte>{opus}.first(20));
    ASSERT_FALSE(short_frame);
    EXPECT_EQ(short_frame.error(), StreamError::invalid_configuration);
    const auto raw_before = data_socket.raw_sends.size();
    for (std::uint8_t value = 1; value <= 3; ++value) {
      opus.fill(static_cast<std::byte>(value));
      EXPECT_TRUE(link_->send_microphone(opus));
    }
    EXPECT_TRUE(link_->pump_once());
    const auto microphone_packets = std::ranges::count_if(
        std::span{data_socket.raw_sends}.subspan(raw_before),
        [](const std::vector<std::byte>& wire) {
          return !wire.empty() && wire[0] == std::byte{0x03};
        });
    EXPECT_EQ(microphone_packets, 3);
    const auto idr_before = data_socket.send_calls;
    EXPECT_TRUE(link_->request_idr());
    EXPECT_TRUE(link_->pump_once());
    EXPECT_GT(data_socket.send_calls, idr_before);
    const auto ctrl_before = ctrl_socket.ctrl_sends.size();
    EXPECT_TRUE(link_->connect_microphone());
    EXPECT_TRUE(link_->set_microphone_muted(false));
    EXPECT_TRUE(link_->set_microphone_muted(true));
    ASSERT_EQ(ctrl_socket.ctrl_sends.size(), ctrl_before + 3);
    const auto& connect = ctrl_socket.ctrl_sends[ctrl_before];
    const auto& unmute = ctrl_socket.ctrl_sends[ctrl_before + 1];
    const auto& mute = ctrl_socket.ctrl_sends[ctrl_before + 2];
    EXPECT_EQ(connect.first, 0x30);
    EXPECT_EQ(connect.second.size(), 2U);
    EXPECT_EQ(unmute.first, 0x36);
    EXPECT_EQ(unmute.second.size(), 4U);
    EXPECT_EQ(mute.first, 0x36);
    EXPECT_EQ(mute.second.size(), 4U);
    EXPECT_EQ(link_->state(), StreamState::streaming);
  }

  void expect_streamed(const Scenario& scenario, bool session_id_fallback) {
    auto& sockets = rig_.sockets();
    if (scenario.downgrade) {
      EXPECT_EQ(sockets.first->heartbeat_replies, 1U);
    }
    EXPECT_EQ(sockets.switch_request_sends, 1U);
    EXPECT_EQ(sockets.switch_ack_sends, 1U);
    EXPECT_EQ(sockets.creates, 2U);
    EXPECT_EQ(sockets.closes, 0U);
    EXPECT_EQ(sockets.session_id_queued, session_id_fallback ? 0U : 1U);
    EXPECT_EQ(rig_.counts().connect, 1U);
    EXPECT_GE(rig_.counts().receive, 10U);
    EXPECT_EQ(rig_.counts().close, 0U);
    EXPECT_EQ(rig_.requests("stun"), 1);
    EXPECT_EQ(rig_.requests("message"), 8);
    const bool ps4 = scenario.target == Target::ps4_10;
    EXPECT_EQ(rig_.requests("wake"), ps4 ? 1 : 0);
    EXPECT_EQ(rig_.requests("start"), ps4 ? 0 : 1);
    const auto& heard = rig_.heard();
    EXPECT_EQ(heard.events, 4 + heard.pin_prompts.size());
    const auto prompts =
        scenario.pin_before_session_id ? 1U + scenario.extra_pin_requests : 0U;
    EXPECT_EQ(heard.pin_prompts.size(), prompts);
    if (ps4) {
      ASSERT_EQ(heard.nicknames.size(), 1U);
      EXPECT_EQ(heard.nicknames[0], "SyntheticBox");
      EXPECT_EQ(heard.registered_events, 0U);
    } else {
      EXPECT_TRUE(heard.nicknames.empty());
    }
    EXPECT_EQ(heard.holepunch_finished, (std::vector<bool>{false, true}));
    EXPECT_TRUE(heard.holepunch_before_info);
    EXPECT_EQ(heard.info_events, 1U);
    EXPECT_EQ(heard.streaming_events, 1U);
    EXPECT_TRUE(heard.info_before_streaming);
    ASSERT_TRUE(heard.last_info);
    ASSERT_EQ(heard.last_info->profiles.size(), 1U);
    EXPECT_EQ(heard.last_info->profiles[0].width, 1280U);
    EXPECT_EQ(heard.last_info->profiles[0].height, 720U);
    EXPECT_EQ(heard.last_info->profiles[0].video_header,
              (std::vector<std::byte>{std::byte{1}, std::byte{2}}));
    ASSERT_TRUE(heard.last_info->launch);
    if (scenario.downgrade) {
      EXPECT_EQ(*heard.last_info->launch,
                (remote_play::event::EffectiveLaunch{
                    remote_play::VideoCodec::h264, 1280, 720, 60, 10000}));
    }
    EXPECT_TRUE(
        std::holds_alternative<remote_play::event::StateChanged>(heard.last));
  }

  void stop_and_release(bool close_failure) {
    auto& sockets = rig_.sockets();
    rig_.counts().fail_close = close_failure;
    rig_.counts().on_close = [this] { EXPECT_EQ(rig_.sockets().closes, 2U); };
    const auto stopped = link_->stop();
    EXPECT_EQ(static_cast<bool>(stopped), !close_failure);
    if (close_failure) {
      ASSERT_TRUE(link_->cleanup_failure());
      EXPECT_TRUE(link_->cleanup_failure()->as<play::PsnFailure>());
    }
    EXPECT_EQ(sockets.closes, 2U);
    EXPECT_EQ(rig_.counts().close, 1U);
    EXPECT_EQ(rig_.requests("release"), 1);
    EXPECT_TRUE(link_->stop());
    EXPECT_EQ(rig_.counts().close, 1U);
    EXPECT_EQ(sockets.closes, 2U);
  }

  void stream(const Scenario& scenario, bool session_id_fallback = false) {
    ASSERT_NO_FATAL_FAILURE(reach_streaming(session_id_fallback));
    ASSERT_NO_FATAL_FAILURE(use_the_stream());
    expect_streamed(scenario, session_id_fallback);
    stop_and_release(false);
  }

  auto pin_sends() -> std::vector<std::vector<std::byte>> {
    std::vector<std::vector<std::byte>> sends;
    for (const auto& [type, payload] : rig_.sockets().first->ctrl_sends)
      if (type == 0x8004) sends.push_back(payload);
    return sends;
  }

  static auto pin(std::string_view digits) -> std::vector<std::byte> {
    const auto bytes = std::as_bytes(std::span{digits});
    return {bytes.begin(), bytes.end()};
  }

  auto rig() -> ScriptedRemote<>& { return rig_; }
  auto link() -> play::Link& { return *link_; }

 private:
  ScriptedRemote<> rig_{{script::created_body, script::members_body,
                         script::ready_members_body, script::ready_custom_body,
                         script::route_offer_body, script::route_result_body,
                         script::route_accept_body, script::data_offer_body,
                         script::data_result_body, script::data_accept_body,
                         script::members_deleted_body}};
  play::Link* link_{};
  unsigned generate_before_{};
};

TEST_F(InternetLinkStreaming, SettlesOnTheLaunchTheConsoleServerTypeAllows) {
  const Scenario scenario{.downgrade = true};
  ASSERT_NO_FATAL_FAILURE(start(scenario));
  stream(scenario);
}

TEST_F(InternetLinkStreaming, StreamsFromAPs4) {
  const Scenario scenario{.target = Target::ps4_10};
  ASSERT_NO_FATAL_FAILURE(start(scenario));
  stream(scenario);
}

TEST_F(InternetLinkStreaming,
       KeepsTheCtrlOpenAfterAMalformedPacketWhileStreaming) {
  ASSERT_NO_FATAL_FAILURE(start({}));
  ASSERT_NO_FATAL_FAILURE(reach_streaming());
  auto& ctrl_socket = *rig().sockets().first;
  internet_links::rudp::Frame broken{0x0230, {std::byte{1}}};
  const auto wire = test_support::Setup::value(
      internet_links::rudp::FrameCodec::encode({&broken, 1}),
      "the frame encodes");
  ctrl_socket.queue_raw({wire.begin(), wire.end()});
  ASSERT_TRUE(link().pump_once());
  EXPECT_EQ(link().state(), StreamState::streaming);
  EXPECT_FALSE(rig().diagnostics().contains("Ctrl lost while streaming"));
  const auto replies = ctrl_socket.heartbeat_replies;
  ctrl_socket.queue_heartbeat(0x6000);
  for (int index = 0; index < 5 && ctrl_socket.heartbeat_replies == replies;
       ++index)
    ASSERT_TRUE(link().pump_once());
  EXPECT_EQ(ctrl_socket.heartbeat_replies, replies + 1);
  EXPECT_TRUE(rig().diagnostics().contains("remote ctrl frame: subtype="));
  for (int index = 0; index < 3; ++index) {
    ctrl_socket.queue_raw({wire.begin(), wire.end()});
    rig().clock().now += 6'000'000;
    ASSERT_TRUE(link().pump_once());
  }
  EXPECT_EQ(link().state(), StreamState::streaming);
  EXPECT_FALSE(rig().diagnostics().contains("Ctrl lost while streaming"));
}

TEST_F(InternetLinkStreaming, SendsTheDisplayDevicesAgainOnceStreaming) {
  ASSERT_NO_FATAL_FAILURE(start({}));
  ASSERT_NO_FATAL_FAILURE(reach_streaming());
  const auto& ctrl_socket = *rig().sockets().first;
  const auto display_devices = [&] {
    return std::ranges::count_if(ctrl_socket.ctrl_sends, [](const auto& sent) {
      return sent.first == 0x910;
    });
  };
  ASSERT_TRUE(link().pump_once());
  EXPECT_EQ(display_devices(), 2);
  for (int index = 0; index < 3; ++index) ASSERT_TRUE(link().pump_once());
  EXPECT_EQ(display_devices(), 2);
}

TEST_F(InternetLinkStreaming, SendsAHeartbeatReplyUnaskedEveryFiveSeconds) {
  ASSERT_NO_FATAL_FAILURE(start({}));
  ASSERT_NO_FATAL_FAILURE(reach_streaming());
  auto& ctrl_socket = *rig().sockets().first;
  ASSERT_TRUE(link().pump_once());
  const auto replies = ctrl_socket.heartbeat_replies;
  rig().clock().now += 4'900'000;
  ASSERT_TRUE(link().pump_once());
  EXPECT_EQ(ctrl_socket.heartbeat_replies, replies);
  rig().clock().now += 100'000;
  ASSERT_TRUE(link().pump_once());
  EXPECT_EQ(ctrl_socket.heartbeat_replies, replies + 1);
  const auto after_first = ctrl_socket.heartbeat_replies;
  rig().clock().now += 4'950'000;
  ASSERT_TRUE(link().pump_once());
  EXPECT_GT(ctrl_socket.heartbeat_replies, after_first);
}

TEST_F(InternetLinkStreaming, AsksForAPinBeforeTheSessionId) {
  const Scenario scenario{.pin_before_session_id = true};
  ASSERT_NO_FATAL_FAILURE(start(scenario));
  EXPECT_TRUE(link().pump_once());
  EXPECT_TRUE(std::holds_alternative<remote_play::event::PinRequired>(
      rig().heard().last));
  EXPECT_TRUE(link().pump_once());
  EXPECT_EQ(rig().sockets().creates, 1U);
  EXPECT_EQ(link().state(), StreamState::establishing_route);
  const std::array pin_digits{std::byte{1}, std::byte{2}, std::byte{3},
                              std::byte{4}};
  EXPECT_TRUE(link().submit_pin(pin_digits));
  stream(scenario);
}

TEST_F(InternetLinkStreaming, MakesUpASessionIdWhenTheConsoleSendsNone) {
  const Scenario scenario{.queue_session_id = false};
  ASSERT_NO_FATAL_FAILURE(start(scenario));
  hold_the_data_route_until_the_fallback(false);
  stream(scenario, true);
}

TEST_F(InternetLinkStreaming, ClosesBothRoutesWhenStoppedAfterTheHolepunch) {
  ASSERT_NO_FATAL_FAILURE(start({.queue_session_id = false}));
  hold_the_data_route_until_the_fallback(true);
  auto& sockets = rig().sockets();
  EXPECT_TRUE(link().stop());
  EXPECT_EQ(sockets.closes, 2U);
  EXPECT_TRUE(link().stop());
  EXPECT_EQ(sockets.closes, 2U);
  EXPECT_EQ(rig().counts().close, 1U);
}

TEST_F(InternetLinkStreaming, WaitsForALateSessionId) {
  const Scenario scenario{};
  ASSERT_NO_FATAL_FAILURE(start(scenario));
  auto& sockets = rig().sockets();
  sockets.first->pending_ = false;
  hold_the_data_route_until_the_fallback(true);
  rig().clock().now = 14'000'000;
  sockets.first->pending_ = true;
  EXPECT_TRUE(link().pump_once());
  stream(scenario);
}

TEST_F(InternetLinkStreaming, FailsTheCtrlWhenRandomBytesRunOut) {
  ASSERT_NO_FATAL_FAILURE(start({.queue_session_id = false}));
  hold_the_data_route_until_the_fallback(true);
  auto& sockets = rig().sockets();
  rig().random().fail = true;
  rig().clock().now = 15'000'100;
  const auto result = link().pump_once();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), StreamError::ctrl_failed);
  EXPECT_EQ(link().state(), StreamState::failed);
  EXPECT_EQ(sockets.closes, 2U);
  EXPECT_EQ(rig().counts().close, 1U);
  EXPECT_EQ(sockets.takion_init_sends, 0U);
  rig().close_link();
  EXPECT_EQ(sockets.closes, 2U);
}

TEST_F(InternetLinkStreaming, StreamsOnceADeferredDataRouteFinishes) {
  const Scenario scenario{};
  rig().threading().defer = true;
  ASSERT_NO_FATAL_FAILURE(start(scenario));
  defer_the_data_route();
  rig().threading().run_pending();
  EXPECT_TRUE(link().pump_once());
  EXPECT_EQ(rig().sockets().creates, 2U);
  stream(scenario);
}

TEST_F(InternetLinkStreaming, AbandonsADeferredDataRouteOnStop) {
  rig().threading().defer = true;
  ASSERT_NO_FATAL_FAILURE(start({}));
  defer_the_data_route();
  auto& sockets = rig().sockets();
  EXPECT_TRUE(link().stop());
  EXPECT_FALSE(rig().threading().pending);
  EXPECT_EQ(sockets.creates, 1U);
  EXPECT_EQ(sockets.closes, 1U);
  EXPECT_EQ(rig().counts().close, 1U);
  EXPECT_TRUE(link().stop());
  EXPECT_EQ(sockets.closes, 1U);
}

TEST_F(InternetLinkStreaming, AbandonsADeferredDataRouteWhenTheCtrlFails) {
  rig().threading().defer = true;
  ASSERT_NO_FATAL_FAILURE(start({}));
  defer_the_data_route();
  auto& sockets = rig().sockets();
  sockets.first->fail_receive = true;
  sockets.first->pending_ = true;
  EXPECT_FALSE(link().pump_once());
  ASSERT_TRUE(link().failure());
  EXPECT_EQ(link().failure()->stage(), Stage::ctrl_receive);
  EXPECT_FALSE(rig().threading().pending);
  EXPECT_EQ(sockets.creates, 1U);
  EXPECT_EQ(sockets.closes, 1U);
  EXPECT_EQ(rig().counts().close, 1U);
  EXPECT_TRUE(link().stop());
  EXPECT_EQ(sockets.closes, 1U);
}

TEST_F(InternetLinkStreaming, FailsTheDataHolepunchWhenItsWorkerCannotStart) {
  rig().threading().fail_start = true;
  ASSERT_NO_FATAL_FAILURE(start({}));
  EXPECT_FALSE(link().pump_once());
  const auto detail = link().failure();
  ASSERT_TRUE(detail);
  EXPECT_EQ(detail->stage(), Stage::data_holepunch);
  const auto* error = detail->as<platform::PlatformError>();
  ASSERT_TRUE(error);
  EXPECT_EQ(*error, platform::PlatformError::thread_failure);
  EXPECT_EQ(rig().sockets().closes, 1U);
  EXPECT_EQ(rig().counts().close, 1U);
}

TEST_F(InternetLinkStreaming, StreamsWithoutASenkushaEchoReply) {
  const Scenario scenario{.answer_echo = false};
  ASSERT_NO_FATAL_FAILURE(start(scenario));
  stream(scenario);
}

TEST_F(InternetLinkStreaming,
       KeepsStreamingThroughALostPushAndFailedHeartbeatReplies) {
  ASSERT_NO_FATAL_FAILURE(start({}));
  ASSERT_NO_FATAL_FAILURE(reach_streaming());
  auto& ctrl_socket = *rig().sockets().first;
  std::vector<std::byte> late_probe(88, std::byte{});
  late_probe[0] = std::byte{0x06};
  ctrl_socket.queue_raw(std::move(late_probe));
  EXPECT_TRUE(link().pump_once());
  EXPECT_EQ(link().state(), StreamState::streaming);
  const auto replies_before = ctrl_socket.heartbeat_replies;
  ctrl_socket.queue_heartbeat(0x5698);
  EXPECT_TRUE(link().pump_once());
  EXPECT_EQ(ctrl_socket.heartbeat_replies, replies_before + 1);
  rig().counts().fail_live = true;
  for (int index = 0; index < 5000 && rig().counts().live_failures.load() == 0;
       ++index)
    LiveProviders::nap(std::chrono::milliseconds{1});
  EXPECT_EQ(rig().counts().live_failures.load(), 1U);
  LiveProviders::nap(std::chrono::milliseconds{20});
  for (int index = 0; index < 3; ++index) {
    SCOPED_TRACE(index);
    EXPECT_TRUE(link().pump_once());
    EXPECT_EQ(link().state(), StreamState::streaming);
  }
  EXPECT_FALSE(link().failure());
  ctrl_socket.fail_heartbeat_send = true;
  ctrl_socket.queue_heartbeat(0x5699);
  EXPECT_TRUE(link().pump_once());
  EXPECT_EQ(link().state(), StreamState::streaming);
  EXPECT_FALSE(link().failure());
  ctrl_socket.queue_heartbeat(0x569a);
  EXPECT_TRUE(link().pump_once());
  EXPECT_EQ(link().state(), StreamState::streaming);
  EXPECT_TRUE(link().stop());
}

TEST_F(InternetLinkStreaming, FailsTheRouteSwitchWhenItsRequestCannotBeSent) {
  ASSERT_NO_FATAL_FAILURE(
      start({.answer_echo = false, .fail_switch_send = true}));
  bool switch_failed = false;
  for (int index = 0; index < 80; ++index) {
    const auto pumped = link().pump_once();
    if (!pumped) {
      switch_failed = true;
      break;
    }
    ASSERT_NE(rig().sockets().takion_init_sends, 1U);
  }
  ASSERT_TRUE(switch_failed);
  const auto detail = link().failure();
  ASSERT_TRUE(detail);
  EXPECT_EQ(detail->stage(), Stage::route_switch);
  const auto* barrier = detail->as<remote_play::RudpSwitchBarrierError>();
  ASSERT_TRUE(barrier);
  EXPECT_EQ(*barrier, remote_play::RudpSwitchBarrierError::send_failed);
  EXPECT_NE(
      detail->sentence().find("sending the transport switch request failed"),
      std::string::npos);
  EXPECT_EQ(link().state(), StreamState::failed);
  EXPECT_EQ(rig().sockets().closes, 2U);
  EXPECT_EQ(rig().counts().close, 1U);
}

TEST_F(InternetLinkStreaming, ReportsAPsnCleanupFailureOnStop) {
  const Scenario scenario{};
  ASSERT_NO_FATAL_FAILURE(start(scenario));
  ASSERT_NO_FATAL_FAILURE(reach_streaming());
  ASSERT_NO_FATAL_FAILURE(use_the_stream());
  expect_streamed(scenario, false);
  stop_and_release(true);
}

TEST_F(InternetLinkStreaming, HoldsAPinSubmittedBeforeThePrompt) {
  const Scenario scenario{.pin_before_session_id = true};
  ASSERT_NO_FATAL_FAILURE(start(scenario));
  EXPECT_TRUE(link().submit_pin(pin("1234")));
  EXPECT_TRUE(pin_sends().empty());
  EXPECT_TRUE(link().pump_once());
  EXPECT_EQ(rig().heard().pin_prompts, (std::vector<bool>{false}));
  EXPECT_EQ(pin_sends(), (std::vector<std::vector<std::byte>>{pin("1234")}));
  stream(scenario);
}

TEST_F(InternetLinkStreaming, AsksAgainAfterAWrongPin) {
  const Scenario scenario{.pin_before_session_id = true,
                          .extra_pin_requests = 1};
  ASSERT_NO_FATAL_FAILURE(start(scenario));
  EXPECT_TRUE(link().pump_once());
  EXPECT_EQ(rig().heard().pin_prompts, (std::vector<bool>{false}));
  EXPECT_TRUE(link().pump_once());
  EXPECT_EQ(rig().heard().pin_prompts, (std::vector<bool>{false}));
  EXPECT_TRUE(link().submit_pin(pin("1234")));
  EXPECT_EQ(pin_sends(), (std::vector<std::vector<std::byte>>{pin("1234")}));
  EXPECT_TRUE(link().submit_pin(pin("5678")));
  EXPECT_EQ(pin_sends().size(), 1U);
  EXPECT_TRUE(link().pump_once());
  EXPECT_EQ(rig().heard().pin_prompts, (std::vector<bool>{false, true}));
  EXPECT_EQ(pin_sends(),
            (std::vector<std::vector<std::byte>>{pin("1234"), pin("5678")}));
  stream(scenario);
}

TEST_F(InternetLinkStreaming, DropsAPinOnceStopped) {
  ASSERT_NO_FATAL_FAILURE(start({.pin_before_session_id = true}));
  EXPECT_TRUE(link().pump_once());
  EXPECT_EQ(rig().heard().pin_prompts, (std::vector<bool>{false}));
  EXPECT_TRUE(pin_sends().empty());
  EXPECT_TRUE(link().stop());
  EXPECT_FALSE(link().submit_pin(pin("1234")));
  static_cast<void>(link().pump_once());
  EXPECT_EQ(rig().heard().pin_prompts, (std::vector<bool>{false}));
}

class InternetLinkPinTable : public InternetLinkStreaming {
 protected:
  auto walk(unsigned extra, std::string_view script) -> std::string {
    start({.pin_before_session_id = true, .extra_pin_requests = extra});
    std::string trace;
    for (const auto step : script) {
      if (step == 'p') static_cast<void>(link().pump_once());
      if (step == 's') static_cast<void>(link().submit_pin(pin("1234")));
      if (step == 'x') static_cast<void>(link().stop());
      trace += "|p";
      for (const bool again : rig().heard().pin_prompts)
        trace += again ? "1" : "0";
      trace += " s" + std::to_string(pin_sends().size());
      trace += " r" + std::to_string(static_cast<int>(link().state()));
    }
    return trace;
  }
};

TEST_F(InternetLinkPinTable, EarlyPin) {
  EXPECT_EQ(walk(0, "spp"), "|p s0 r1|p0 s1 r1|p0 s1 r1") << "early_pin";
}

TEST_F(InternetLinkPinTable, PromptThenPin) {
  EXPECT_EQ(walk(0, "psp"), "|p0 s0 r1|p0 s1 r1|p0 s1 r1") << "prompt_then_pin";
}

TEST_F(InternetLinkPinTable, WrongThenRight) {
  EXPECT_EQ(walk(1, "pspsp"),
            "|p0 s0 r1|p0 s1 r1|p01 s1 r1|p01 s2 r1|p01 s2 r1")
      << "wrong_then_right";
}

TEST_F(InternetLinkPinTable, RightBeforeReprompt) {
  EXPECT_EQ(walk(1, "psspp"), "|p0 s0 r1|p0 s1 r1|p0 s1 r1|p01 s2 r1|p01 s2 r1")
      << "right_before_reprompt";
}

TEST_F(InternetLinkPinTable, TwiceWrong) {
  EXPECT_EQ(
      walk(2, "pspspsp"),
      "|p0 s0 r1|p0 s1 r1|p01 s1 r1|p01 s2 r1|p01 s2 r1|p01 s2 r1|p01 s2 r2")
      << "twice_wrong";
}

TEST_F(InternetLinkPinTable, NeverAnswered) {
  EXPECT_EQ(walk(0, "ppp"), "|p0 s0 r1|p0 s0 r1|p0 s0 r1") << "never_answered";
}

TEST_F(InternetLinkPinTable, Stopped) {
  EXPECT_EQ(walk(0, "pxsp"), "|p0 s0 r1|p0 s0 r5|p0 s0 r5|p0 s0 r5")
      << "stopped";
}

}  // namespace
}  // namespace play_test
