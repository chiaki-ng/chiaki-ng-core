#include "src/core/net/protocol/remote_play/remote_session_init.hpp"
#include "src/core/net/protocol/remote_play/rudp_switch.hpp"
#include "src/core/play/adapters/links/link_failure.hpp"
#include "src/core/play/adapters/links/session_diagnostics.hpp"
#include "src/core/play/model/venue.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/rudp_ctrl_rig.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/rudp_ctrl_session_init_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace play_test {
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace rudp = libchiaking::net::transport::rudp;
using rudp_ctrl::ExchangeChannel;
using StreamError = remote_play::StreamError;
using Quit = remote_play::QuitReason;

class SessionInitRefusals : public rudp_ctrl::SessionInitRig {
 public:
  struct Case {
    std::string_view response;
    StreamError expected;
    std::uint16_t status;
    std::uint32_t reason;
    std::string_view description;
    std::size_t short_send_call;
  };

  static constexpr std::array cases{
      Case{"HTTP/1.1 403 Forbidden\r\nRP-Application-Reason: 80108b10\r\n\r\n",
           StreamError::session_init_in_use, 403, 0x80108b10,
           "Remote Play is already in use", 0},
      Case{"HTTP/1.1 503 Unavailable\r\nRP-Application-Reason: "
           "80108b15\r\n\r\n",
           StreamError::session_init_refused, 503, 0x80108b15,
           "the console reported that Remote Play crashed", 0},
      Case{"HTTP/1.1 400 Bad Request\r\nRP-Application-Reason: "
           "80108b11\r\nRP-Version: 9.0\r\n\r\n",
           StreamError::session_init_version_mismatch, 400, 0x80108b11,
           "the console and client protocol versions disagree", 0},
      Case{"HTTP/1.1 200 OK\r\nRP-Nonce:", StreamError::session_init_timeout, 0,
           0, "Received 26 response bytes.", 0},
      Case{"HTTP/1.1 200 OK\r\nRP-Nonce: bad\r\n\r\n",
           StreamError::session_init_invalid_response, 0, 0,
           "the peer response was invalid", 0},
      Case{"", StreamError::session_init_timeout, 0, 0,
           "the response timed out", 0},
      Case{"", StreamError::session_init_network_failure, 0, 0,
           "the network operation failed", 3},
  };

 protected:
  void queue_reply(std::string_view response) {
    channel_.inbox().push_back(ExchangeChannel::packet(
        0xD000, {std::byte{0x56}, std::byte{0x78}, std::byte{0}, std::byte{0},
                 std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
                 std::byte{0xC0}, std::byte{0x0E}}));
    channel_.inbox().push_back(
        ExchangeChannel::packet(0xA030, {std::byte{0x56}, std::byte{0x79}}));
    if (response.empty()) return;
    std::vector<std::byte> payload{std::byte{0x56}, std::byte{0x7A}};
    for (const auto character : response)
      payload.push_back(static_cast<std::byte>(character));
    channel_.inbox().push_back(
        ExchangeChannel::packet(0x0230, std::move(payload)));
  }
};

class RemoteSessionInitRefusal
    : public SessionInitRefusals,
      public ::testing::WithParamInterface<std::size_t> {};

TEST_P(RemoteSessionInitRefusal, DescribesWhyTheConsoleRefused) {
  const auto& item = cases.at(GetParam());
  channel_.short_send_on_call(item.short_send_call);
  queue_reply(item.response);
  const auto result = run(5, reference_handshake);
  ASSERT_FALSE(result);
  auto detail = play::LinkFailure::refused(result.error());
  detail.reached_through(play::Venue::internet());
  EXPECT_EQ(detail.stage(), remote_play::FailureStage::session_init);
  EXPECT_TRUE(detail.as<remote_play::RemoteSessionInitFailure>());
  EXPECT_EQ(detail.result(), item.expected);
  EXPECT_EQ(detail.http_status(), item.status);
  EXPECT_EQ(detail.console_reason(), item.reason);
  EXPECT_EQ(detail.received_bytes(), item.response.size());
  const auto description = detail.sentence();
  EXPECT_NE(description.find("Remote console: Session setup"),
            std::string::npos);
  EXPECT_NE(description.find(item.description), std::string::npos)
      << description;
  if (item.status) {
    EXPECT_NE(description.find("HTTP " + std::to_string(item.status)),
              std::string::npos);
  }
}

INSTANTIATE_TEST_SUITE_P(
    EveryRefusal, RemoteSessionInitRefusal,
    ::testing::Range<std::size_t>(0, SessionInitRefusals::cases.size()));

TEST(RemoteSessionInitQuit, GivesNoQuitReasonForAFailedHandshake) {
  remote_play::RemoteSessionInitFailure handshake{};
  handshake.kind = remote_play::RemoteSessionInitError::handshake_failed;
  handshake.cause = static_cast<std::uint32_t>(rudp::ConnectError::timeout);
  EXPECT_EQ(play::LinkFailure::refused(handshake).quit().reason, Quit::none);
}

TEST(RemoteSessionInitQuit, QuitsAFailedExchangeAsAnUnknownSessionRequest) {
  remote_play::RemoteSessionInitFailure exchange{};
  exchange.kind = remote_play::RemoteSessionInitError::exchange_failed;
  const auto exchanged = play::LinkFailure::refused(exchange);
  EXPECT_EQ(exchanged.quit().reason, Quit::session_request_unknown);
}

TEST(RemoteSessionInitQuit, GivesNoQuitReasonForARouteSwitchUntilCancelled) {
  play::LinkFailure switched{remote_play::FailureStage::route_switch,
                             StreamError::pipeline_failed,
                             remote_play::RudpSwitchBarrierError::timeout};
  EXPECT_EQ(switched.quit().reason, Quit::none);
  switched.cancel();
  EXPECT_EQ(switched.quit().reason, Quit::stopped);
}

}  // namespace
}  // namespace play_test
