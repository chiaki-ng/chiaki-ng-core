#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/home_link_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test {
namespace {

using namespace home_links;

class HomeLinkCtrlLifecycle : public HomeLinkTest {
 protected:
  static constexpr std::string_view startup_accepted =
      "local stage: Ctrl startup accepted; awaiting session ID";
  static constexpr std::string_view fallback_synthesized =
      "local stage: Ctrl session ID fallback synthesized";
  static constexpr std::string_view ctrl_failed_during_calibration =
      "local stage: Ctrl failed; checked when calibration completes";

  void open() {
    auto& sockets = providers_.network().sockets();
    listener_ = Loopback::listener(sockets);
    ASSERT_NE(listener_, nullptr);
    calibration_peer_ = Loopback::udp_peer(sockets);
    active_peer_ = Loopback::udp_peer(sockets);
    ASSERT_NE(calibration_peer_, nullptr);
    ASSERT_NE(active_peer_, nullptr);
    play::HomeLinkConfiguration config;
    config.console.target = Target::ps5_1;
    config.host = "127.0.0.1";
    config.ctrl_endpoint = listener_->local_endpoint().value();
    config.route.local = {platform::IpAddress::loopback_ipv4(), 0};
    config.route.remote = calibration_peer_->local_endpoint().value();
    config.route.dont_fragment = remote_play::DontFragmentPolicy::disabled;
    config.active_stream_port = active_peer_->local_endpoint().value().port;
    auto made =
        play::HomeLinkConfiguration::link(std::move(config), providers_);
    ASSERT_TRUE(made);
    session_ = std::move(made.value());
    session_->set_event_sink(sink_);
    ASSERT_TRUE(session_->start());
    refresh_deadline();
    SessionInitConsole init{*session_, *listener_, deadline_};
    ASSERT_NO_FATAL_FAILURE(init.open_ctrl("/sie/ps5/rp/sess/init", &clock_));
    console_ = init.take_ctrl();
    ASSERT_NE(console_, nullptr);
  }

  void accept_startup() {
    ASSERT_NO_FATAL_FAILURE(send(CtrlWire::bytes("HTTP/1.1 200 OK\r\n\r\n")));
    ASSERT_NO_FATAL_FAILURE(
        pump_until([&] { return diagnostics_.contains(startup_accepted); }));
  }

  void confirm_session_id() {
    ASSERT_NO_FATAL_FAILURE(
        send(CtrlWire::frame(0x33, CtrlWire::session_id())));
    ASSERT_NO_FATAL_FAILURE(pump_until(
        [&] { return session_->state() == StreamState::negotiating_stream; }));
  }

  void send(std::span<const std::byte> bytes) {
    std::size_t sent{};
    refresh_deadline();
    while (sent < bytes.size() && test_support::LiveProviders::now() < deadline_) {
      const auto part = console_->send(bytes.subspan(sent));
      if (part)
        sent += part.value();
      else
        ASSERT_EQ(part.error(), NetError::would_block);
      test_support::LiveProviders::spin_pause();
    }
    ASSERT_EQ(sent, bytes.size());
  }

  template <class Done>
  void pump_until(Done done) {
    refresh_deadline();
    while (!done() && test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(session_->pump_once());
      test_support::LiveProviders::spin_pause();
    }
    ASSERT_TRUE(done());
  }

  auto pump_until_ended() -> bool {
    bool ended = false;
    refresh_deadline();
    while (!ended && test_support::LiveProviders::now() < deadline_) {
      ended = !session_->pump_once();
      test_support::LiveProviders::spin_pause();
    }
    return ended;
  }

  auto failed() const -> bool {
    return !sink_.all<remote_play::event::Failed>().empty();
  }

  auto reached_active() -> testing::AssertionResult {
    std::array<std::byte, 2048> wire{};
    refresh_deadline();
    while (test_support::LiveProviders::now() < deadline_) {
      const auto received = active_peer_->receive_from(wire);
      if (received) return testing::AssertionSuccess();
      if (received.error() != NetError::would_block)
        return testing::AssertionFailure()
               << "the active peer failed to receive";
      if (!session_->pump_once())
        return testing::AssertionFailure() << "the link ended first";
      test_support::LiveProviders::spin_pause();
    }
    return testing::AssertionFailure() << "nothing reached the active peer";
  }

  void expect_failed_after_calibration() {
    ASSERT_NO_FATAL_FAILURE(pump_until(
        [&] { return diagnostics_.contains(ctrl_failed_during_calibration); }));
    EXPECT_FALSE(failed());
    EXPECT_EQ(session_->state(), StreamState::negotiating_stream);
    clock_.advance(std::chrono::seconds{31});
    EXPECT_TRUE(pump_until_ended());
    EXPECT_TRUE(failed());
    EXPECT_EQ(session_->state(), StreamState::failed);
    ASSERT_TRUE(session_->failure());
    EXPECT_EQ(session_->failure()->result(), StreamError::ctrl_failed);
    EXPECT_EQ(session_->quit().reason, remote_play::QuitReason::ctrl_unknown);
    EXPECT_TRUE(console_->close());
    EXPECT_TRUE(listener_->close());
  }

  auto read_console() -> testing::AssertionResult {
    for (;;) {
      std::array<std::byte, 1024> buffer{};
      const auto received = console_->receive(buffer);
      if (!received) {
        if (received.error() == NetError::would_block)
          return testing::AssertionSuccess();
        return testing::AssertionFailure() << "Ctrl read failed";
      }
      ctrl_wire_.insert(
          ctrl_wire_.end(), buffer.begin(),
          buffer.begin() + static_cast<std::ptrdiff_t>(received.value()));
    }
  }

  void refresh_deadline() {
    deadline_ = test_support::LiveProviders::now() + std::chrono::seconds{10};
  }

  auto session() -> play::Link& { return *session_; }
  auto console() -> platform::TcpSocket& { return *console_; }
  auto listener() -> platform::TcpListener& { return *listener_; }
  auto clock() -> FixedClock& { return clock_; }
  auto sink() const -> const RecordingSink& { return sink_; }
  auto diagnostics() const -> const CapturingDiagnostics& {
    return diagnostics_;
  }
  auto deadline() const -> Deadline { return deadline_; }
  auto ctrl_wire() const -> const std::vector<std::byte>& { return ctrl_wire_; }

 private:
  ZeroCrypto crypto_;
  FixedClock clock_;
  CapturingDiagnostics diagnostics_;
  platform::DefaultPlatformFactory providers_{platform::PlatformProviders{
      .clock = &clock_, .crypto = &crypto_, .diagnostics = &diagnostics_}};
  std::unique_ptr<platform::TcpListener> listener_;
  std::unique_ptr<platform::UdpSocket> calibration_peer_;
  std::unique_ptr<platform::UdpSocket> active_peer_;
  std::unique_ptr<play::Link> session_;
  RecordingSink sink_;
  std::unique_ptr<platform::TcpSocket> console_;
  std::vector<std::byte> ctrl_wire_;
  Deadline deadline_{test_support::LiveProviders::now() + std::chrono::seconds{10}};
};

TEST_F(HomeLinkCtrlLifecycle,
       MakesUpASessionIdWhileTheCtrlRequestIsUnanswered) {
  ASSERT_NO_FATAL_FAILURE(open());
  std::string request;
  refresh_deadline();
  while (request.find("\r\n\r\n") == std::string::npos &&
         test_support::LiveProviders::now() < deadline()) {
    ASSERT_TRUE(session().pump_once());
    std::array<std::byte, 1024> buffer{};
    const auto received = console().receive(buffer);
    if (received)
      request.append(reinterpret_cast<const char*>(buffer.data()),
                     received.value());
    else
      ASSERT_EQ(received.error(), NetError::would_block);
    test_support::LiveProviders::spin_pause();
  }
  EXPECT_NE(request.find("/ctrl"), std::string::npos) << request;
  clock().advance(std::chrono::milliseconds{4'900});
  EXPECT_TRUE(session().pump_once());
  clock().advance(std::chrono::milliseconds{4'900});
  EXPECT_TRUE(session().pump_once());
  EXPECT_EQ(session().state(), StreamState::establishing_route);
  clock().advance(std::chrono::milliseconds{200});
  ASSERT_NO_FATAL_FAILURE(pump_until(
      [&] { return session().state() == StreamState::negotiating_stream; }));
  EXPECT_FALSE(failed());
  EXPECT_TRUE(diagnostics().contains(fallback_synthesized));
  EXPECT_TRUE(session().stop());
  EXPECT_TRUE(console().close());
  EXPECT_TRUE(listener().close());
}

TEST_F(HomeLinkCtrlLifecycle,
       StartsOverWhenAPinIsRequestedAfterAMadeUpSessionId) {
  ASSERT_NO_FATAL_FAILURE(open());
  ASSERT_NO_FATAL_FAILURE(accept_startup());
  clock().advance(std::chrono::seconds{10});
  ASSERT_NO_FATAL_FAILURE(
      pump_until([&] { return diagnostics().contains(fallback_synthesized); }));
  EXPECT_FALSE(failed());
  ASSERT_NO_FATAL_FAILURE(send(CtrlWire::frame(0x4)));
  EXPECT_TRUE(pump_until_ended());
  EXPECT_TRUE(failed());
  EXPECT_TRUE(
      diagnostics().contains("local ctrl: Login PIN requested after a made-up "
                             "session ID; a fresh session will ask for it"));
  EXPECT_EQ(session().state(), StreamState::failed);
  ASSERT_TRUE(session().failure());
  EXPECT_EQ(session().failure()->result(), StreamError::ctrl_timeout);
  EXPECT_TRUE(sink().all<remote_play::event::PinRequired>().empty());
  EXPECT_TRUE(console().close());
  EXPECT_TRUE(listener().close());
}

TEST_F(HomeLinkCtrlLifecycle,
       CalibratesAnywayWhenCtrlClosesBeforeTheSessionId) {
  ASSERT_NO_FATAL_FAILURE(open());
  ASSERT_NO_FATAL_FAILURE(accept_startup());
  EXPECT_TRUE(console().close());
  for (int round = 0; round < 50; ++round) ASSERT_TRUE(session().pump_once());
  EXPECT_FALSE(failed());
  EXPECT_EQ(session().state(), StreamState::establishing_route);
  clock().advance(std::chrono::microseconds{9'999'999});
  EXPECT_TRUE(session().pump_once());
  EXPECT_EQ(session().state(), StreamState::establishing_route);
  clock().advance(std::chrono::microseconds{1});
  ASSERT_NO_FATAL_FAILURE(pump_until(
      [&] { return session().state() == StreamState::negotiating_stream; }));
  EXPECT_FALSE(failed());
  EXPECT_TRUE(session().stop());
  EXPECT_TRUE(listener().close());
}

TEST_F(HomeLinkCtrlLifecycle, AnswersEveryQueuedHeartbeatInOnePump) {
  ASSERT_NO_FATAL_FAILURE(open());
  ASSERT_NO_FATAL_FAILURE(accept_startup());
  ASSERT_NO_FATAL_FAILURE(confirm_session_id());
  for (int round = 0; round < 20; ++round) {
    ASSERT_TRUE(session().pump_once());
    test_support::LiveProviders::nap(std::chrono::milliseconds{1});
  }
  ASSERT_TRUE(read_console());
  const auto before = ctrl_wire().size();
  std::vector<std::byte> burst;
  for (int index = 0; index < 3; ++index) {
    const auto request = CtrlWire::frame(0xfe);
    burst.insert(burst.end(), request.begin(), request.end());
  }
  ASSERT_NO_FATAL_FAILURE(send(burst));
  test_support::LiveProviders::nap(std::chrono::milliseconds{50});
  EXPECT_TRUE(session().pump_once());
  test_support::LiveProviders::nap(std::chrono::milliseconds{50});
  ASSERT_TRUE(read_console());
  const auto reply = CtrlWire::frame(0x1fe);
  ASSERT_EQ(ctrl_wire().size() - before, 3 * reply.size());
  for (std::size_t offset = before; offset < ctrl_wire().size();
       offset += reply.size()) {
    SCOPED_TRACE(offset);
    EXPECT_TRUE(
        std::equal(reply.begin(), reply.end(),
                   ctrl_wire().begin() + static_cast<std::ptrdiff_t>(offset)));
  }
  EXPECT_TRUE(session().stop());
  EXPECT_TRUE(console().close());
  EXPECT_TRUE(listener().close());
}

TEST_F(HomeLinkCtrlLifecycle, FailsAfterCalibrationWhenCtrlBreaksDuringIt) {
  ASSERT_NO_FATAL_FAILURE(open());
  ASSERT_NO_FATAL_FAILURE(accept_startup());
  ASSERT_NO_FATAL_FAILURE(confirm_session_id());
  auto oversized = CtrlWire::frame(0x34);
  ASSERT_FALSE(oversized.empty());
  oversized[0] = std::byte{0x7f};
  ASSERT_NO_FATAL_FAILURE(send(oversized));
  expect_failed_after_calibration();
}

TEST_F(HomeLinkCtrlLifecycle,
       FailsAfterCalibrationWhenAPinFollowsTheSessionId) {
  ASSERT_NO_FATAL_FAILURE(open());
  ASSERT_NO_FATAL_FAILURE(accept_startup());
  ASSERT_NO_FATAL_FAILURE(confirm_session_id());
  ASSERT_NO_FATAL_FAILURE(send(CtrlWire::frame(0x4)));
  expect_failed_after_calibration();
}

TEST_F(HomeLinkCtrlLifecycle,
       OpensTheActiveRouteWhenCtrlClosesDuringCalibration) {
  ASSERT_NO_FATAL_FAILURE(open());
  ASSERT_NO_FATAL_FAILURE(accept_startup());
  ASSERT_NO_FATAL_FAILURE(confirm_session_id());
  EXPECT_TRUE(console().close());
  ASSERT_NO_FATAL_FAILURE(pump_until([&] {
    return diagnostics().contains(
        "local stage: console closed Ctrl; continuing without it");
  }));
  EXPECT_FALSE(failed());
  EXPECT_EQ(session().state(), StreamState::negotiating_stream);
  clock().advance(std::chrono::seconds{31});
  EXPECT_TRUE(reached_active());
  EXPECT_FALSE(failed());
  EXPECT_TRUE(session().stop());
  EXPECT_TRUE(listener().close());
}

}  // namespace
}  // namespace play_test
