#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/home_link_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace play_test {
namespace {

using namespace home_links;

class HomeLinkPin : public HomeLinkTest {
 protected:
  static constexpr std::array<std::byte, 4> pin{std::byte{'1'}, std::byte{'2'},
                                                std::byte{'3'}, std::byte{'4'}};
  static constexpr std::array<std::byte, 4> other_pin{
      std::byte{'5'}, std::byte{'6'}, std::byte{'7'}, std::byte{'8'}};

  void link(bool held_sends = false) {
    platform::SocketProvider* sockets{};
    if (held_sends) {
      network_.scripted_sockets().delegate_to(network_.default_sockets());
      network_.scripted_sockets().hold_sends(false);
      providers_.emplace(
          platform::PlatformProviders{.clock = &clock_,
                                      .network = &network_,
                                      .crypto = &crypto_,
                                      .diagnostics = &diagnostics_});
      sockets = &network_.default_sockets();
    } else {
      providers_.emplace(platform::PlatformProviders{
          .clock = &clock_, .crypto = &crypto_, .diagnostics = &diagnostics_});
      sockets = &providers_->network().sockets();
    }
    listener_ = Loopback::listener(*sockets);
    ASSERT_NE(listener_, nullptr);
    const auto endpoint = listener_->local_endpoint();
    ASSERT_TRUE(endpoint);
    play::HomeLinkConfiguration config;
    config.console.target = Target::ps5_1;
    config.host = "127.0.0.1";
    config.ctrl_endpoint = endpoint.value();
    auto made =
        play::HomeLinkConfiguration::link(std::move(config), *providers_);
    ASSERT_TRUE(made);
    session_ = std::move(made.value());
    session_->set_event_sink(sink_);
  }

  void open_ctrl() {
    deadline_ = test_support::LiveProviders::now() + std::chrono::seconds{3};
    SessionInitConsole init{*session_, *listener_, deadline_};
    ASSERT_NO_FATAL_FAILURE(init.open_ctrl("/sie/ps5/rp/sess/init", &clock_));
    console_ = init.take_ctrl();
    ASSERT_NE(console_, nullptr);
  }

  void start() {
    ASSERT_NO_FATAL_FAILURE(link());
    ASSERT_TRUE(session_->start());
    ASSERT_NO_FATAL_FAILURE(open_ctrl());
  }

  void request_pin(int requests = 1) {
    auto reply = CtrlWire::bytes("HTTP/1.1 200 OK\r\n\r\n");
    const auto request = CtrlWire::frame(0x4);
    for (int index = 0; index < requests; ++index)
      reply.insert(reply.end(), request.begin(), request.end());
    ASSERT_NO_FATAL_FAILURE(send(reply));
  }

  void send(std::span<const std::byte> bytes) {
    std::size_t sent{};
    const auto until = window();
    while (sent < bytes.size() && test_support::LiveProviders::now() < until) {
      const auto part = console_->send(bytes.subspan(sent));
      if (part)
        sent += part.value();
      else
        ASSERT_EQ(part.error(), NetError::would_block);
      test_support::LiveProviders::spin_pause();
    }
    ASSERT_EQ(sent, bytes.size());
  }

  void pump(int rounds = 50) {
    for (int round = 0; round < rounds; ++round) {
      ASSERT_TRUE(session_->pump_once());
      test_support::LiveProviders::nap(std::chrono::milliseconds{1});
    }
  }

  void pump_until_pin_events(unsigned count) {
    const auto until = window();
    while (sink_.pin_events() < count &&
           test_support::LiveProviders::now() < until) {
      ASSERT_TRUE(session_->pump_once());
      test_support::LiveProviders::spin_pause();
    }
  }

  void discard_sent() {
    std::array<std::byte, 2048> drain{};
    while (true) {
      const auto part = console_->receive(drain);
      if (!part) {
        ASSERT_EQ(part.error(), NetError::would_block);
        break;
      }
      ASSERT_NE(part.value(), 0U);
    }
  }

  auto take_sent() -> std::vector<std::byte> {
    std::vector<std::byte> wire;
    std::array<std::byte, 256> buffer{};
    while (true) {
      const auto part = console_->receive(buffer);
      if (!part) break;
      wire.insert(wire.end(), buffer.begin(),
                  buffer.begin() + static_cast<std::ptrdiff_t>(part.value()));
    }
    return wire;
  }

  auto read_sent(std::size_t size) -> std::vector<std::byte> {
    std::vector<std::byte> wire;
    const auto until = window();
    while (wire.size() < size && test_support::LiveProviders::now() < until) {
      const auto pumped = session_->pump_once();
      EXPECT_TRUE(pumped);
      if (!pumped) break;
      std::array<std::byte, 32> buffer{};
      const auto part = console_->receive(buffer);
      if (part) {
        wire.insert(wire.end(), buffer.begin(),
                    buffer.begin() + static_cast<std::ptrdiff_t>(part.value()));
      } else {
        EXPECT_EQ(part.error(), NetError::would_block);
        if (part.error() != NetError::would_block) break;
      }
      test_support::LiveProviders::spin_pause();
    }
    return wire;
  }

  auto last_request() const -> std::optional<bool> {
    const auto* request = sink_.last_as<remote_play::event::PinRequired>();
    if (!request) return std::nullopt;
    return request->incorrect;
  }

  void close() {
    EXPECT_TRUE(console_->close());
    EXPECT_TRUE(listener_->close());
  }

  auto session() -> play::Link& { return *session_; }
  auto sink() const -> const EventCounter& { return sink_; }
  auto clock() -> FixedClock& { return clock_; }
  auto diagnostics() const -> const CapturingDiagnostics& {
    return diagnostics_;
  }
  auto network() -> ScriptedNetwork& { return network_; }
  auto deadline() const -> Deadline { return deadline_; }

 private:
  static auto window() -> Deadline {
    return test_support::LiveProviders::now() + std::chrono::seconds{3};
  }

  ZeroCrypto crypto_;
  FixedClock clock_;
  CapturingDiagnostics diagnostics_;
  ScriptedNetwork network_;
  std::optional<platform::DefaultPlatformFactory> providers_;
  std::unique_ptr<platform::TcpListener> listener_;
  std::unique_ptr<play::Link> session_;
  EventCounter sink_;
  std::unique_ptr<platform::TcpSocket> console_;
  Deadline deadline_{test_support::LiveProviders::now() + std::chrono::seconds{3}};
};

TEST_F(HomeLinkPin, AsksForThePinOnceAndRefusesAnOversizedOne) {
  ASSERT_NO_FATAL_FAILURE(start());
  ASSERT_NO_FATAL_FAILURE(request_pin());
  ASSERT_NO_FATAL_FAILURE(pump_until_pin_events(1));
  EXPECT_EQ(sink().pin_events(), 1U);
  EXPECT_EQ(last_request(), false);
  EXPECT_TRUE(session().pump_once());
  EXPECT_EQ(sink().pin_events(), 1U);
  const std::array<std::byte, 505> oversized{};
  EXPECT_FALSE(session().submit_pin(oversized));
  EXPECT_TRUE(session().stop());
  close();
}

TEST_F(HomeLinkPin, SendsTheFirstPinAndHoldsTheNextUntilTheConsoleRefuses) {
  ASSERT_NO_FATAL_FAILURE(start());
  ASSERT_NO_FATAL_FAILURE(request_pin());
  ASSERT_NO_FATAL_FAILURE(pump_until_pin_events(1));
  ASSERT_EQ(sink().pin_events(), 1U);
  ASSERT_NO_FATAL_FAILURE(discard_sent());
  clock().advance(std::chrono::seconds{20});
  EXPECT_TRUE(session().submit_pin(pin));
  EXPECT_TRUE(session().submit_pin(other_pin));
  const auto expected = CtrlWire::frame(0x8004, pin);
  EXPECT_EQ(read_sent(expected.size()), expected);
  clock().advance(std::chrono::microseconds{9'999'999});
  EXPECT_TRUE(session().pump_once());
  EXPECT_FALSE(diagnostics().has_line(
      "local stage: Ctrl session ID fallback synthesized"));
  EXPECT_EQ(sink().media_events(), 0U);
  ASSERT_NO_FATAL_FAILURE(send(CtrlWire::frame(0x5, CtrlWire::of({1}))));
  ASSERT_NO_FATAL_FAILURE(pump_until_pin_events(2));
  EXPECT_EQ(sink().pin_events(), 2U);
  EXPECT_EQ(last_request(), true);
  const auto expected_retry = CtrlWire::frame(0x8004, other_pin);
  EXPECT_EQ(read_sent(expected_retry.size()), expected_retry);
  EXPECT_EQ(sink().pin_events(), 2U);
  EXPECT_TRUE(session().submit_pin(pin));
  EXPECT_TRUE(session().stop());
  EXPECT_EQ(sink().media_events(), 0U);
  EXPECT_FALSE(session().submit_pin(pin));
  EXPECT_EQ(sink().pin_events(), 2U);
  close();
}

TEST_F(HomeLinkPin, CoalescesRepeatedRequests) {
  ASSERT_NO_FATAL_FAILURE(start());
  ASSERT_NO_FATAL_FAILURE(request_pin(3));
  ASSERT_NO_FATAL_FAILURE(pump());
  EXPECT_EQ(sink().pin_events(), 1U);
  EXPECT_EQ(last_request(), false);
  std::vector<std::byte> during_entry;
  for (int index = 0; index < 2; ++index) {
    const auto request = CtrlWire::frame(0x4);
    during_entry.insert(during_entry.end(), request.begin(), request.end());
  }
  ASSERT_NO_FATAL_FAILURE(send(during_entry));
  ASSERT_NO_FATAL_FAILURE(pump());
  EXPECT_EQ(sink().pin_events(), 1U);
  EXPECT_TRUE(session().submit_pin(pin));
  ASSERT_NO_FATAL_FAILURE(pump());
  EXPECT_EQ(sink().pin_events(), 2U);
  EXPECT_EQ(last_request(), true);
  EXPECT_TRUE(session().submit_pin(pin));
  ASSERT_NO_FATAL_FAILURE(pump());
  EXPECT_EQ(sink().pin_events(), 2U);
  EXPECT_TRUE(session().stop());
  close();
}

TEST_F(HomeLinkPin, AsksAgainWhenTheConsoleRepeatsTheRequestUntilStopped) {
  ASSERT_NO_FATAL_FAILURE(start());
  ASSERT_NO_FATAL_FAILURE(request_pin());
  ASSERT_NO_FATAL_FAILURE(pump());
  EXPECT_EQ(sink().pin_events(), 1U);
  EXPECT_EQ(last_request(), false);
  static_cast<void>(take_sent());
  EXPECT_TRUE(session().submit_pin(pin));
  ASSERT_NO_FATAL_FAILURE(pump());
  EXPECT_EQ(take_sent(), CtrlWire::frame(0x8004, pin));
  EXPECT_EQ(sink().pin_events(), 1U);
  const auto request = CtrlWire::frame(0x4);
  ASSERT_NO_FATAL_FAILURE(send(request));
  ASSERT_NO_FATAL_FAILURE(pump());
  EXPECT_EQ(sink().pin_events(), 2U);
  EXPECT_EQ(last_request(), true);
  EXPECT_TRUE(session().stop());
  ASSERT_NO_FATAL_FAILURE(send(request));
  for (int round = 0; round < 20; ++round) {
    static_cast<void>(session().pump_once());
    test_support::LiveProviders::nap(std::chrono::milliseconds{1});
  }
  EXPECT_FALSE(session().submit_pin(pin));
  EXPECT_EQ(sink().pin_events(), 2U);
  close();
}

TEST_F(HomeLinkPin, KeepsWaitingForThePinWhenASessionIdArrivesDuringThePrompt) {
  ASSERT_NO_FATAL_FAILURE(start());
  ASSERT_NO_FATAL_FAILURE(request_pin());
  ASSERT_NO_FATAL_FAILURE(pump());
  EXPECT_EQ(sink().pin_events(), 1U);
  EXPECT_EQ(last_request(), false);
  ASSERT_NO_FATAL_FAILURE(send(CtrlWire::frame(0x33, CtrlWire::session_id())));
  ASSERT_NO_FATAL_FAILURE(send(CtrlWire::frame(0x5, CtrlWire::of({1}))));
  clock().advance(std::chrono::seconds{20});
  ASSERT_NO_FATAL_FAILURE(pump());
  EXPECT_EQ(sink().pin_events(), 1U);
  EXPECT_EQ(session().state(), StreamState::establishing_route);
  static_cast<void>(take_sent());
  EXPECT_TRUE(session().submit_pin(pin));
  ASSERT_NO_FATAL_FAILURE(pump());
  EXPECT_EQ(sink().pin_events(), 2U);
  EXPECT_EQ(last_request(), true);
  EXPECT_EQ(session().state(), StreamState::establishing_route);
  auto wire = take_sent();
  const auto pin_frame = CtrlWire::frame(0x8004, pin);
  EXPECT_EQ(std::ranges::search(wire, pin_frame).begin(), wire.begin());
  EXPECT_TRUE(session().submit_pin(other_pin));
  const auto retry_frame = CtrlWire::frame(0x8004, other_pin);
  wire.clear();
  for (int round = 0;
       round < 50 && session().state() == StreamState::establishing_route;
       ++round) {
    static_cast<void>(session().pump_once());
    const auto more = take_sent();
    wire.insert(wire.end(), more.begin(), more.end());
    test_support::LiveProviders::nap(std::chrono::milliseconds{1});
  }
  const auto rest = take_sent();
  wire.insert(wire.end(), rest.begin(), rest.end());
  EXPECT_EQ(std::ranges::search(wire, retry_frame).begin(), wire.begin());
  EXPECT_NE(session().state(), StreamState::establishing_route);
  EXPECT_EQ(sink().pin_events(), 2U);
  static_cast<void>(session().stop());
  close();
}

TEST_F(HomeLinkPin, SendsOnlyThePinSubmittedAfterStarting) {
  ASSERT_NO_FATAL_FAILURE(link());
  const std::array<std::byte, 4> early{std::byte{'9'}, std::byte{'9'},
                                       std::byte{'9'}, std::byte{'9'}};
  EXPECT_TRUE(session().submit_pin(early));
  ASSERT_TRUE(session().start());
  EXPECT_TRUE(session().submit_pin(pin));
  ASSERT_NO_FATAL_FAILURE(open_ctrl());
  ASSERT_NO_FATAL_FAILURE(request_pin());
  const auto expected = CtrlWire::frame(0x8004, pin);
  std::vector<std::byte> wire;
  const auto ends_with_pin = [&] {
    return wire.size() >= expected.size() &&
           std::equal(
               expected.begin(), expected.end(),
               wire.end() - static_cast<std::ptrdiff_t>(expected.size()));
  };
  while (!ends_with_pin() && test_support::LiveProviders::now() < deadline()) {
    ASSERT_TRUE(session().pump_once());
    const auto more = read_sent(1);
    wire.insert(wire.end(), more.begin(), more.end());
  }
  EXPECT_EQ(sink().pin_events(), 1U);
  EXPECT_EQ(last_request(), false);
  EXPECT_TRUE(ends_with_pin());
  EXPECT_EQ(std::ranges::search(wire, early).begin(), wire.end());
  EXPECT_TRUE(session().stop());
  EXPECT_FALSE(session().submit_pin(pin));
  close();
}

TEST_F(HomeLinkPin, QueuesThePinBehindAPendingHeartbeatReply) {
  ASSERT_NO_FATAL_FAILURE(link(true));
  ASSERT_TRUE(session().start());
  ASSERT_NO_FATAL_FAILURE(open_ctrl());
  ASSERT_NO_FATAL_FAILURE(request_pin());
  ASSERT_NO_FATAL_FAILURE(pump_until_pin_events(1));
  EXPECT_EQ(sink().pin_events(), 1U);
  ASSERT_NO_FATAL_FAILURE(discard_sent());
  network().scripted_sockets().hold_sends(true);
  ASSERT_NO_FATAL_FAILURE(send(CtrlWire::frame(0xfe)));
  const auto settle = test_support::LiveProviders::now() + std::chrono::milliseconds{200};
  while (test_support::LiveProviders::now() < settle) {
    ASSERT_TRUE(session().pump_once());
    test_support::LiveProviders::spin_pause();
  }
  EXPECT_TRUE(session().submit_pin(pin));
  EXPECT_TRUE(session().submit_pin(other_pin));
  network().scripted_sockets().hold_sends(false);
  auto expected = CtrlWire::frame(0x1fe);
  const auto pin_frame = CtrlWire::frame(0x8004, other_pin);
  expected.insert(expected.end(), pin_frame.begin(), pin_frame.end());
  EXPECT_EQ(read_sent(expected.size()), expected);
  EXPECT_EQ(sink().pin_events(), 1U);
  EXPECT_EQ(sink().failed_events(), 0U);
  EXPECT_EQ(session().state(), StreamState::establishing_route);
  EXPECT_TRUE(session().stop());
  close();
}

}  // namespace
}  // namespace play_test
