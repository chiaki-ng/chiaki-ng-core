#include "src/core/net/protocol/takion/control_chunk_codec.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/home_link_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace play_test {
namespace {

using namespace home_links;

class HomeLinkRouteFailure : public HomeLinkTest {
 protected:
  void open(bool dualsense_ctrl, bool calibration_peers = false,
            bool calibration_socket_fails = false) {
    providers_.emplace(platform::PlatformProviders{
        .clock = &clock_,
        .network =
            calibration_socket_fails
                ? static_cast<platform::NetworkProvider*>(&failing_network_)
                : nullptr,
        .crypto = &crypto_,
        .diagnostics = &diagnostics_});
    listener_ = Loopback::listener(providers_->network().sockets());
    ASSERT_NE(listener_, nullptr);
    const auto endpoint = listener_->local_endpoint();
    ASSERT_TRUE(endpoint);
    if (calibration_peers) {
      calibration_peer_ = Loopback::udp_peer(peer_network_.sockets());
      active_peer_ = Loopback::udp_peer(peer_network_.sockets());
      ASSERT_NE(calibration_peer_, nullptr);
      ASSERT_NE(active_peer_, nullptr);
    }
    play::HomeLinkConfiguration config;
    config.console.target = Target::ps5_1;
    config.host = "127.0.0.1";
    config.ctrl_endpoint = endpoint.value();
    config.enable_dualsense_ctrl = dualsense_ctrl;
    config.route.local = {platform::IpAddress::loopback_ipv6(), 0};
    config.route.remote = {platform::IpAddress::loopback_ipv4(), 9297};
    config.route.dont_fragment = remote_play::DontFragmentPolicy::disabled;
    if (calibration_peers) {
      config.route.local = {platform::IpAddress::loopback_ipv4(), 0};
      config.route.remote = calibration_peer_->local_endpoint().value();
      config.active_stream_port = active_peer_->local_endpoint().value().port;
    }
    auto made =
        play::HomeLinkConfiguration::link(std::move(config), *providers_);
    ASSERT_TRUE(made);
    session_ = std::move(made.value());
    session_->set_event_sink(sink_);
    ASSERT_TRUE(session_->start());
    SessionInitConsole init{*session_, *listener_, deadline_};
    ASSERT_NO_FATAL_FAILURE(init.open_ctrl("/sie/ps5/rp/sess/init", &clock_));
    console_ = init.take_ctrl();
    ASSERT_NE(console_, nullptr);
  }

  void answer(std::span<const std::byte> frames = {}) {
    auto reply = CtrlWire::bytes("HTTP/1.1 200 OK\r\n\r\n");
    reply.insert(reply.end(), frames.begin(), frames.end());
    std::size_t sent{};
    while (sent < reply.size() && test_support::LiveProviders::now() < deadline_) {
      const auto part =
          console_->send(std::span<const std::byte>(reply).subspan(sent));
      if (part)
        sent += part.value();
      else
        ASSERT_EQ(part.error(), NetError::would_block);
      test_support::LiveProviders::spin_pause();
    }
    ASSERT_EQ(sent, reply.size());
  }

  void make_up_session_id() {
    while (!diagnostics_.contains(
               "local stage: Ctrl startup accepted; awaiting session ID") &&
           test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(session_->pump_once());
      test_support::LiveProviders::spin_pause();
    }
    ASSERT_TRUE(diagnostics_.contains(
        "local stage: Ctrl startup accepted; awaiting session ID"));
    clock_.advance(std::chrono::microseconds{9'999'999});
    EXPECT_TRUE(session_->pump_once());
    EXPECT_EQ(sink_.failed_events(), 0U);
    clock_.advance(std::chrono::microseconds{1});
  }

  void await_route_failure() {
    bool saw_route_failure = false;
    while (!saw_route_failure && test_support::LiveProviders::now() < deadline_) {
      const auto progress = session_->pump_once();
      if (!progress) {
        EXPECT_EQ(progress.error(), StreamError::route_failed);
        saw_route_failure = true;
      }
      test_support::LiveProviders::spin_pause();
    }
    ASSERT_TRUE(saw_route_failure);
  }

  void expect_post_id_frames_before_ctrl_closes() {
    std::vector<std::byte> ctrl_wire;
    bool closed = false;
    while (!closed && test_support::LiveProviders::now() < deadline_) {
      std::array<std::byte, 1024> buffer{};
      const auto received = console_->receive(buffer);
      if (!received) {
        EXPECT_TRUE(received.error() == NetError::would_block ||
                    received.error() == NetError::closed)
            << testing::PrintToString(received.error());
        closed = received.error() == NetError::closed;
        test_support::LiveProviders::spin_pause();
        continue;
      }
      closed = received.value() == 0;
      ctrl_wire.insert(
          ctrl_wire.end(), buffer.begin(),
          buffer.begin() + static_cast<std::ptrdiff_t>(received.value()));
    }
    ASSERT_TRUE(closed);
    const std::string_view request(
        reinterpret_cast<const char*>(ctrl_wire.data()), ctrl_wire.size());
    const auto header_end = request.find("\r\n\r\n");
    ASSERT_NE(header_end, std::string_view::npos);
    auto frames = std::span<const std::byte>(ctrl_wire).subspan(header_end + 4);
    constexpr std::array<std::uint16_t, 5> expected_types{0x13, 0x11, 0x36,
                                                          0x36, 0x910};
    for (const auto expected_type : expected_types) {
      SCOPED_TRACE(expected_type);
      const auto decoded = remote_play::CtrlCodec::decode_frame(frames);
      ASSERT_TRUE(decoded);
      EXPECT_EQ(decoded.value().type, expected_type);
      frames = frames.subspan(decoded.value().consumed);
    }
  }

  void expect_failed_once() {
    EXPECT_TRUE(
        diagnostics_.contains("local route: invalid UDP configuration"));
    EXPECT_EQ(session_->state(), StreamState::failed);
    EXPECT_EQ(sink_.failed_events(), 1U);
    const auto* failed = sink_.last_as<remote_play::event::Failed>();
    ASSERT_NE(failed, nullptr);
    EXPECT_EQ(failed->reason,
              remote_play::StreamFailure::route_establishment_failed);
    EXPECT_TRUE(session_->stop());
    EXPECT_TRUE(session_->stop());
    EXPECT_EQ(sink_.failed_events(), 1U);
    EXPECT_TRUE(console_->close());
    EXPECT_TRUE(listener_->close());
  }

  void reach_calibration() {
    while (session_->state() != StreamState::negotiating_stream &&
           test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(session_->pump_once());
      test_support::LiveProviders::spin_pause();
    }
    ASSERT_EQ(session_->state(), StreamState::negotiating_stream);
  }

  void expect_active_route_opened() {
    EXPECT_TRUE(session_->pump_once());
    std::array<std::byte, 2048> wire{};
    auto received = active_peer_->receive_from(wire);
    while (!received && test_support::LiveProviders::now() < deadline_) {
      ASSERT_EQ(received.error(), NetError::would_block);
      ASSERT_TRUE(session_->pump_once());
      test_support::LiveProviders::spin_pause();
      received = active_peer_->receive_from(wire);
    }
    ASSERT_TRUE(received);
    const auto profile =
        takion::resolve_takion_wire_profile(takion::takion_version_ps5);
    ASSERT_TRUE(profile);
    const auto packet = takion::PacketCodec::decode_plain_packet(
        std::span<const std::byte>(wire).first(received.value().size),
        *profile);
    ASSERT_TRUE(packet);
    const auto control =
        takion::ControlChunkCodec::decode_packet(packet.value().payload);
    ASSERT_TRUE(control);
    const auto* init = std::get_if<takion::InitChunk>(&control.value());
    ASSERT_NE(init, nullptr);
    EXPECT_EQ(init->header.verification_tag.value, 0U);
    EXPECT_EQ(init->values.initial_sequence_number.value,
              init->values.tag.value);
    EXPECT_TRUE(diagnostics_.has_line(
        "local stage: Senkusha fallback; opening active route"));
    EXPECT_EQ(sink_.failed_events(), 0U);
    EXPECT_TRUE(session_->stop());
    EXPECT_TRUE(session_->stop());
    EXPECT_EQ(sink_.failed_events(), 0U);
    EXPECT_TRUE(console_->close());
    EXPECT_TRUE(listener_->close());
    EXPECT_TRUE(calibration_peer_->close());
    EXPECT_TRUE(active_peer_->close());
  }

  auto diagnostics() const -> const CapturingDiagnostics& {
    return diagnostics_;
  }
  auto clock() -> FixedClock& { return clock_; }
  auto failing_network() -> FirstUdpFailsNetwork& { return failing_network_; }

 private:
  ZeroCrypto crypto_;
  FixedClock clock_;
  CapturingDiagnostics diagnostics_;
  FirstUdpFailsNetwork failing_network_;
  platform::DefaultNetworkProvider peer_network_{test_support::LiveProviders::threading(),
                                                 test_support::LiveProviders::steady()};
  std::optional<platform::DefaultPlatformFactory> providers_;
  std::unique_ptr<platform::TcpListener> listener_;
  std::unique_ptr<platform::UdpSocket> calibration_peer_;
  std::unique_ptr<platform::UdpSocket> active_peer_;
  std::unique_ptr<play::Link> session_;
  EventCounter sink_;
  std::unique_ptr<platform::TcpSocket> console_;
  Deadline deadline_{test_support::LiveProviders::now() + std::chrono::seconds{3}};
};

TEST_F(HomeLinkRouteFailure, FailsOnceWhenTheRouteIsInvalid) {
  ASSERT_NO_FATAL_FAILURE(open(false));
  ASSERT_NO_FATAL_FAILURE(
      answer(CtrlWire::frame(0x33, CtrlWire::session_id())));
  ASSERT_NO_FATAL_FAILURE(await_route_failure());
  expect_failed_once();
}

TEST_F(HomeLinkRouteFailure, FailsOnceAfterMakingUpASessionId) {
  ASSERT_NO_FATAL_FAILURE(open(true));
  ASSERT_NO_FATAL_FAILURE(answer());
  ASSERT_NO_FATAL_FAILURE(make_up_session_id());
  ASSERT_NO_FATAL_FAILURE(await_route_failure());
  EXPECT_TRUE(diagnostics().has_line(
      "local stage: Ctrl session ID fallback synthesized"));
  ASSERT_NO_FATAL_FAILURE(expect_post_id_frames_before_ctrl_closes());
  expect_failed_once();
}

TEST_F(HomeLinkRouteFailure, FailsOnceAfterAMalformedSessionId) {
  ASSERT_NO_FATAL_FAILURE(open(true));
  ASSERT_NO_FATAL_FAILURE(answer(CtrlWire::frame(0x33, CtrlWire::of({0}))));
  ASSERT_NO_FATAL_FAILURE(await_route_failure());
  EXPECT_TRUE(diagnostics().has_line(
      "local stage: Ctrl session ID fallback synthesized"));
  ASSERT_NO_FATAL_FAILURE(expect_post_id_frames_before_ctrl_closes());
  expect_failed_once();
}

TEST_F(HomeLinkRouteFailure, OpensTheActiveRouteWhenCalibrationTimesOut) {
  ASSERT_NO_FATAL_FAILURE(open(true, true));
  ASSERT_NO_FATAL_FAILURE(answer());
  ASSERT_NO_FATAL_FAILURE(make_up_session_id());
  ASSERT_NO_FATAL_FAILURE(reach_calibration());
  clock().advance(std::chrono::seconds{31});
  expect_active_route_opened();
}

TEST_F(HomeLinkRouteFailure, OpensTheActiveRouteWhenTheCalibrationSocketFails) {
  ASSERT_NO_FATAL_FAILURE(open(true, true, true));
  ASSERT_NO_FATAL_FAILURE(answer());
  ASSERT_NO_FATAL_FAILURE(make_up_session_id());
  ASSERT_NO_FATAL_FAILURE(reach_calibration());
  EXPECT_GE(failing_network().scripted().udp_calls(), 2U);
  expect_active_route_opened();
}

}  // namespace
}  // namespace play_test
