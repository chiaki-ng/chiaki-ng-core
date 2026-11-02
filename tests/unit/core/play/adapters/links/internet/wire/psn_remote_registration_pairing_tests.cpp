#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "src/core/play/adapters/links/internet/wire/psn_remote_registration.hpp"
#include "src/core/play/adapters/links/link_failure.hpp"
#include "src/core/play/model/venue.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/random.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/psn_remote_registration_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace play_test {
namespace {

namespace internet = libchiaking::play::internet;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace rudp = libchiaking::net::transport::rudp;
namespace datagram = libchiaking::net::transport::datagram;
namespace platform = libchiaking::platform;
using psn_remote_registration::PairingChannel;
using psn_remote_registration::PairingCrypto;
using psn_remote_registration::Replies;
using PairingError = internet::PsnRemoteRegistrationError;
using Target = remote_play::RegistrationTarget;

class RudpPairing : public ::testing::Test {
 protected:
  auto pair_account(std::span<const std::byte> account_id,
                    platform::StopToken stop = {}) {
    return internet::RudpPairing(channel_, random_, crypto_, clock_)
        .pair(std::move(stop), 7, Target::ps5_1, account_id, data, data, data,
              "10.0.2.15");
  }

  auto pair(platform::StopToken stop = {}) {
    return pair_account(account, std::move(stop));
  }

  void queue_handshake() {
    channel_.inbox().push_back(Replies::init_response());
    channel_.inbox().push_back(Replies::cookie_response());
  }

  void queue_registration() {
    queue_handshake();
    channel_.inbox().push_back(Replies::registration_response());
  }

  void queue_refusal(std::string_view header) {
    queue_handshake();
    channel_.inbox().push_back(Replies::http_response(header));
    channel_.inbox().push_back(Replies::frame(0xC000, {}));
  }

  static auto ack_of(std::uint16_t local, std::uint16_t remote)
      -> datagram::OwnedDatagram {
    auto ack = rudp::FrameCodec::encode_ack_message(local, remote);
    EXPECT_TRUE(ack);
    if (!ack) return {};
    return std::move(ack.value());
  }

  static constexpr std::array<std::byte, 8> account{};
  static constexpr std::array<std::byte, 16> data{};

  test_support::CountingRandom random_;
  test_support::FakeClock clock_{0};
  PairingChannel channel_{clock_};

 private:
  PairingCrypto crypto_;
};

TEST_F(RudpPairing, RefusesABadRequestBeforeSending) {
  const auto bad = pair_account({account.data(), 7});
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error().kind, PairingError::request_failed);
  EXPECT_EQ(bad.error().cause,
            static_cast<std::uint32_t>(
                internet::PsnRequestError::invalid_account_id));
  EXPECT_TRUE(channel_.sent().empty());
  EXPECT_EQ(random_.calls, 0U);
}

TEST_F(RudpPairing, ReportsAHandshakeTimeoutWhenTheConsoleIsSilent) {
  const auto timed = pair();
  ASSERT_FALSE(timed);
  EXPECT_EQ(timed.error().kind, PairingError::handshake_failed);
  EXPECT_EQ(timed.error().cause,
            static_cast<std::uint32_t>(rudp::ConnectError::timeout));
  EXPECT_EQ(timed.error().received_bytes, 0U);
  EXPECT_EQ(clock_.now_us, 3'000'000U);
}

TEST_F(RudpPairing, ReportsARefusalWithItsStatusAndReason) {
  constexpr std::string_view header =
      "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nRP-Application-Reason: "
      "80108b15\r\n\r\n";
  queue_refusal(header);
  const auto refused = pair();
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind, PairingError::response_failed);
  EXPECT_EQ(refused.error().cause,
            static_cast<std::uint32_t>(internet::PsnResponseError::refused));
  EXPECT_EQ(refused.error().http_status, 403);
  EXPECT_EQ(refused.error().protocol_reason, 0x80108b15U);
  EXPECT_EQ(refused.error().received_bytes, header.size());
  EXPECT_TRUE(channel_.inbox().empty());
  EXPECT_EQ(channel_.sent().size(), 4U);
  EXPECT_EQ(clock_.now_us, 0U);
}

class RudpPairingOddRefusal
    : public RudpPairing,
      public ::testing::WithParamInterface<std::string_view> {};

TEST_P(RudpPairingOddRefusal, StillReadsTheStatusAndReason) {
  queue_refusal(GetParam());
  const auto refused = pair();
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind, PairingError::response_failed);
  EXPECT_EQ(refused.error().http_status, 403);
  EXPECT_EQ(refused.error().protocol_reason, 0x80108b12U);
}

INSTANTIATE_TEST_SUITE_P(
    EveryLayout, RudpPairingOddRefusal,
    ::testing::Values(
        std::string_view{
            "HTTP/1.1 403 \r\nRP-Application-Reason: 80108b12\r\n\r\n"},
        std::string_view{"HTTP/1.1 403 Forbidden\r\nServer: "
                         "\r\nRP-Application-Reason: 80108b12\r\n\r\n"},
        std::string_view{"HTTP/1.1 403 Forbidden\r\nRP-Application-Reason: "
                         "1\r\nRP-Application-Reason: 80108b12\r\n\r\n"}));

TEST_F(RudpPairing, RegistersAndAcknowledgesTheReply) {
  queue_registration();
  const auto result = pair();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().console.registration_key[15], std::byte{0x0f});
  EXPECT_NE(result.value().next_local_counter, 0);
  EXPECT_NE(result.value().local_header, 0U);
  EXPECT_EQ(result.value().remote_counter, 0x567B);
  ASSERT_EQ(channel_.sent().size(), 6U);
  EXPECT_EQ(random_.calls, 2U);
  EXPECT_EQ(clock_.now_us, 3'000'000U);
  EXPECT_EQ(channel_.sent().back(),
            ack_of(result.value().next_local_counter, 0x567B));
}

TEST_F(RudpPairing, StopsWaitingOnceTheConsoleFinishes) {
  queue_registration();
  channel_.inbox().push_back(Replies::frame(0xC000, {}));
  const auto finished = pair();
  ASSERT_TRUE(finished);
  EXPECT_EQ(finished.value().remote_counter, 0x567B);
  ASSERT_EQ(channel_.sent().size(), 4U);
  EXPECT_TRUE(channel_.inbox().empty());
  EXPECT_EQ(clock_.now_us, 0U);
  EXPECT_EQ(channel_.sent().back(),
            ack_of(finished.value().next_local_counter, 0x567B));
}

TEST_F(RudpPairing, AnswersAStrayMessageAfterTheReply) {
  queue_registration();
  channel_.inbox().push_back(
      Replies::frame(0x8030, {std::byte{0x12}, std::byte{0x34}}));
  channel_.inbox().push_back(Replies::frame(0xC000, {}));
  const auto finished = pair();
  ASSERT_TRUE(finished);
  EXPECT_EQ(finished.value().remote_counter, 0x567B);
  EXPECT_EQ(channel_.sent().size(), 5U);
  EXPECT_TRUE(channel_.inbox().empty());
  EXPECT_EQ(clock_.now_us, 0U);
}

TEST_F(RudpPairing, StopsAtAMalformedDatagramAfterTheReply) {
  queue_registration();
  channel_.inbox().push_back(datagram::OwnedDatagram{std::byte{0xFF}});
  channel_.inbox().push_back(Replies::frame(0xC000, {}));
  const auto finished = pair();
  ASSERT_TRUE(finished);
  EXPECT_EQ(finished.value().remote_counter, 0x567B);
  EXPECT_EQ(channel_.sent().size(), 4U);
  EXPECT_EQ(channel_.inbox().size(), 1U);
  EXPECT_EQ(clock_.now_us, 0U);
}

TEST_F(RudpPairing, StopsWhenAskedAfterTheReply) {
  auto stop_state = std::make_shared<test_support::StopState>();
  channel_.stop_on_fourth_receive(stop_state);
  queue_registration();
  channel_.inbox().push_back(Replies::frame(0x8030, {}));
  const auto stopped = pair(platform::StopToken{stop_state});
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error().kind, PairingError::stopped);
  EXPECT_EQ(channel_.sent().size(), 4U);
}

TEST_F(RudpPairing, ResendsTheRequestAfterAStrayAcknowledgement) {
  queue_handshake();
  channel_.inbox().push_back(Replies::frame(
      0x8030,
      {std::byte{0x56}, std::byte{0x79}, std::byte{0}, std::byte{0x92}}));
  channel_.inbox().push_back(Replies::registration_response());
  channel_.inbox().push_back(Replies::frame(0xC000, {}));
  const auto finished = pair();
  ASSERT_TRUE(finished);
  ASSERT_EQ(channel_.sent().size(), 5U);
  EXPECT_EQ(Replies::frame_type(channel_.sent()[2]),
            Replies::frame_type(channel_.sent()[3]));
  EXPECT_NE(channel_.sent()[2], channel_.sent()[3]);
  EXPECT_EQ(channel_.sent()[4],
            ack_of(finished.value().next_local_counter, 0x567B));
  EXPECT_EQ(clock_.now_us, 0U);
}

TEST_F(RudpPairing, GivesUpAfterTenSeconds) {
  for (int stage = 0; stage < 3; ++stage) {
    channel_.delay(PairingChannel::timeout_delay);
    channel_.delay(PairingChannel::timeout_delay);
    channel_.delay(stage == 2 ? 4'200'000 : 950'000);
  }
  queue_registration();
  const auto capped = pair();
  ASSERT_FALSE(capped);
  EXPECT_EQ(capped.error().kind, PairingError::timed_out);
  EXPECT_GE(clock_.now_us, 10'000'000U);
  EXPECT_LE(clock_.now_us, 12'100'000U);
  auto failure = play::LinkFailure::unpaired(capped.error());
  failure.reached_through(play::Venue::internet());
  const auto detail = failure.sentence();
  EXPECT_NE(detail.find("did not complete within 10 seconds"),
            std::string::npos);
}

TEST_F(RudpPairing, AcceptsAReplyThatArrivesWithinTheLimit) {
  channel_.delay(0);
  channel_.delay(0);
  channel_.delay(9'500'000);
  queue_registration();
  const auto late = pair();
  ASSERT_TRUE(late);
  EXPECT_EQ(late.value().console.registration_key[15], std::byte{0x0f});
  EXPECT_EQ(clock_.now_us, 12'500'000U);
  EXPECT_EQ(channel_.sent().size(), 6U);
}

TEST_F(RudpPairing, ReportsAnExchangeFailureForAnEmptyFrameBeforeTheReply) {
  queue_handshake();
  channel_.inbox().push_back(Replies::frame(0x8030, {}));
  channel_.inbox().push_back(Replies::registration_response());
  const auto failed = pair();
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().kind, PairingError::exchange_failed);
  EXPECT_EQ(failed.error().cause,
            static_cast<std::uint32_t>(
                remote_play::RudpCtrlHttpExchangeError::receive_failed));
  EXPECT_EQ(channel_.sent().size(), 3U);
}

}  // namespace
}  // namespace play_test
