#include "src/core/consoles/adapters/pairing/pairing_exchange.hpp"
#include "src/core/net/protocol/remote_play/pairing/cipher.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/unit/core/consoles/adapters/pairing/home_console.hpp"

#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/testing/in_memory_network.hpp>
#include <libchiaking/testing/manual_clock.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace consoles_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::consoles::PairingExchange;
using libchiaking::platform::NetError;
using Pumped =
    libchiaking::Expected<PairingExchange::Progress, PairingExchange::Error>;

class PairingExchangeTable : public ::testing::Test {
 protected:
  static constexpr auto target = remote_play::RegistrationTarget::ps4_10;

  template <class Reached>
  static auto reached(const Pumped& pumped) -> testing::AssertionResult {
    if (!pumped)
      return testing::AssertionFailure()
             << "failed with " << testing::PrintToString(pumped.error());
    if (!std::holds_alternative<Reached>(pumped.value()))
      return testing::AssertionFailure()
             << "reached progress " << pumped.value().index();
    return testing::AssertionSuccess();
  }

  static auto console_of(const Pumped& pumped)
      -> const remote_play::RegisteredConsole& {
    return std::get<remote_play::RegisteredConsole>(pumped.value());
  }

  static auto material() -> remote_play::RegistrationMaterial {
    remote_play::RegistrationMaterial result;
    result.bright.fill(std::byte{1});
    result.ambassador.fill(std::byte{2});
    return result;
  }

  auto encrypted_success_frame(std::string_view trailing = {})
      -> std::vector<std::byte> {
    constexpr std::string_view body_text =
        "PS4-Mac:001122334455\r\n"
        "PS4-RegistKey:0102\r\n"
        "RP-Key:00112233445566778899aabbccddeeff\r\n";
    auto body =
        HomeConsole::bytes(std::string(body_text) + std::string(trailing));
    auto keys = material();
    const auto transformed = remote_play::RegistrationCipher::transform(
        crypto, target, {keys.bright.data(), keys.bright.size()},
        {keys.ambassador.data(), keys.ambassador.size()}, 0,
        remote_play::CipherDirection::encrypt, body, body);
    EXPECT_TRUE(transformed);
    std::vector<std::byte> frame =
        HomeConsole::bytes("HTTP/1.1 200 OK\r\nContent-Length: " +
                           std::to_string(body_text.size()) + "\r\n\r\n");
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
  }

  auto open(std::span<const std::byte> header,
            std::span<const std::byte> payload) -> Pumped {
    if (auto staged = exchange.stage(header, payload, target); !staged)
      return libchiaking::unexpected(staged.error());
    return exchange.begin(console.endpoint(), material());
  }

  void send_the_request() {
    ASSERT_TRUE(reached<PairingExchange::Waiting>(
        open(HomeConsole::bytes("H"), HomeConsole::bytes("P"))));
    ASSERT_TRUE(
        reached<PairingExchange::Connected>(exchange.await_connection()));
    ASSERT_TRUE(reached<PairingExchange::HeaderSent>(exchange.send_header()));
    ASSERT_TRUE(reached<PairingExchange::RequestSent>(exchange.send_payload()));
    ASSERT_TRUE(console.accept());
  }

  libchiaking::platform::SteadyClock steady;
  libchiaking::testing::InMemoryNetwork network{
      test_support::process_threading(), steady};
  libchiaking::testing::ManualClock clock;
  XorCrypto crypto;
  HomeConsole console{network};
  PairingExchange exchange{network.sockets(), crypto, clock};
};

TEST_F(PairingExchangeTable, SendsTheRequestAcrossPartialSends) {
  network.stall_connects();
  const std::array<std::byte, 3> payload{std::byte{1}, std::byte{2},
                                         std::byte{3}};
  const auto header = HomeConsole::bytes("ABCD");
  ASSERT_TRUE(reached<PairingExchange::Waiting>(open(header, payload)));
  ASSERT_TRUE(reached<PairingExchange::Waiting>(exchange.await_connection()));
  network.stall_connects(false);
  ASSERT_TRUE(reached<PairingExchange::Connected>(exchange.await_connection()));
  network.limit_stream_sends(2);
  ASSERT_TRUE(reached<PairingExchange::Waiting>(exchange.send_header()));
  network.fail_sends_with(NetError::would_block);
  ASSERT_TRUE(reached<PairingExchange::Waiting>(exchange.send_header()));
  network.clear_failures();
  network.limit_stream_sends(2);
  ASSERT_TRUE(reached<PairingExchange::HeaderSent>(exchange.send_header()));
  network.limit_stream_sends(1);
  ASSERT_TRUE(reached<PairingExchange::Waiting>(exchange.send_payload()));
  network.limit_stream_sends(2);
  ASSERT_TRUE(reached<PairingExchange::RequestSent>(exchange.send_payload()));
  ASSERT_TRUE(console.accept());
  ASSERT_TRUE(console.read());
  EXPECT_EQ(console.request,
            (std::vector<std::byte>{std::byte{'A'}, std::byte{'B'},
                                    std::byte{'C'}, std::byte{'D'},
                                    std::byte{1}, std::byte{2}, std::byte{3}}));
}

TEST_F(PairingExchangeTable, ClosesOnceWhenCancelled) {
  ASSERT_NO_FATAL_FAILURE(send_the_request());
  EXPECT_NE(exchange.pollable(), nullptr);
  EXPECT_FALSE(console.hung_up());
  EXPECT_TRUE(exchange.cancel());
  EXPECT_EQ(exchange.pollable(), nullptr);
  EXPECT_TRUE(console.hung_up());
  EXPECT_TRUE(exchange.cancel());
}

TEST_F(PairingExchangeTable, TimesOutAConnectionAndClosesOnce) {
  network.stall_connects();
  ASSERT_TRUE(open(HomeConsole::bytes("H"), HomeConsole::bytes("P")));
  clock.now_us += 3'000'000;
  const auto timed_out = exchange.await_connection();
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error(), PairingExchange::Error::timeout);
  EXPECT_EQ(exchange.pollable(), nullptr);
  EXPECT_TRUE(exchange.cancel());
  EXPECT_TRUE(exchange.cancel());
  network.stall_connects(false);
  EXPECT_FALSE(console.accept());
  const auto after_cancel = exchange.await_connection();
  ASSERT_FALSE(after_cancel);
  EXPECT_EQ(after_cancel.error(), PairingExchange::Error::invalid_state);
}

TEST_F(PairingExchangeTable, KeepsTheFirstDeadlineForAPendingConnection) {
  network.stall_connects();
  ASSERT_TRUE(open(HomeConsole::bytes("H"), HomeConsole::bytes("P")));
  clock.now_us += 2'000'000;
  network.stall_connects(false);
  ASSERT_TRUE(reached<PairingExchange::Connected>(exchange.await_connection()));
  network.fail_sends_with(NetError::not_connected);
  ASSERT_TRUE(
      reached<PairingExchange::ConnectionPending>(exchange.send_header()));
  clock.now_us += 1'000'000;
  const auto timed_out = exchange.await_connection();
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error(), PairingExchange::Error::timeout);
  network.clear_failures();
  EXPECT_TRUE(console.accept());
  EXPECT_TRUE(console.hung_up());
}

TEST_F(PairingExchangeTable, RefusesAnOversizedHeaderWithoutConnecting) {
  const std::array<std::byte, 257> too_large{};
  EXPECT_FALSE(open(too_large, HomeConsole::bytes("P")));
  EXPECT_FALSE(console.accept());
}

TEST_F(PairingExchangeTable, TimesOutABlockedSend) {
  ASSERT_TRUE(open(HomeConsole::bytes("H"), HomeConsole::bytes("P")));
  network.fail_sends_with(NetError::would_block);
  ASSERT_TRUE(reached<PairingExchange::Connected>(exchange.await_connection()));
  ASSERT_TRUE(reached<PairingExchange::Waiting>(exchange.send_header()));
  clock.now_us += 3'000'000;
  const auto timed_out = exchange.send_header();
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error(), PairingExchange::Error::timeout);
  network.clear_failures();
  EXPECT_TRUE(console.accept());
  EXPECT_TRUE(console.hung_up());
}

TEST_F(PairingExchangeTable, ReadsASplitReplyAndClosesOnce) {
  ASSERT_NO_FATAL_FAILURE(send_the_request());
  const auto frame = encrypted_success_frame();
  const auto whole = std::span<const std::byte>{frame};
  console.send(whole.first(10));
  EXPECT_TRUE(reached<PairingExchange::Waiting>(exchange.read_reply_header()));
  console.send(whole.subspan(10, 38));
  EXPECT_TRUE(reached<PairingExchange::ReplyHeaderAccepted>(
      exchange.read_reply_header()));
  EXPECT_FALSE(console.hung_up());
  console.send(whole.subspan(48));
  const auto result = exchange.read_reply_body();
  ASSERT_TRUE(reached<remote_play::RegisteredConsole>(result));
  EXPECT_EQ(console_of(result).mac[0], std::byte{0});
  EXPECT_EQ(console_of(result).registration_key[0], std::byte{1});
  EXPECT_EQ(console_of(result).registration_key[1], std::byte{2});
  EXPECT_EQ(console_of(result).key[15], std::byte{0xff});
  EXPECT_TRUE(console.hung_up());
  EXPECT_EQ(exchange.pollable(), nullptr);
  EXPECT_FALSE(exchange.read_reply_body());
  EXPECT_TRUE(exchange.cancel());
}

TEST_F(PairingExchangeTable, ReportsTheRefusalReason) {
  ASSERT_NO_FATAL_FAILURE(send_the_request());
  console.send(
      "HTTP/1.1 403 Forbidden\r\n"
      "RP-Application-Reason: 80108b15\r\n\r\n");
  const auto refused = exchange.read_reply_header();
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), PairingExchange::Error::refused);
  EXPECT_EQ(exchange.ps_reason(), 0x80108b15U);
  EXPECT_EQ(exchange.http_status(), std::uint16_t{403});
  EXPECT_TRUE(console.hung_up());
}

TEST_F(PairingExchangeTable, TimesOutASilentConsole) {
  ASSERT_NO_FATAL_FAILURE(send_the_request());
  clock.now_us += 3'000'000;
  const auto timed_out = exchange.read_reply_header();
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(timed_out.error(), PairingExchange::Error::timeout);
  EXPECT_TRUE(console.hung_up());
}

TEST_F(PairingExchangeTable, RefusesAnOversizedReply) {
  ASSERT_NO_FATAL_FAILURE(send_the_request());
  console.send(
      std::span<const std::byte>{std::vector<std::byte>(1500, std::byte{'X'})});
  const auto rejected = exchange.read_reply_header();
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), PairingExchange::Error::too_large);
  EXPECT_TRUE(console.hung_up());
}

TEST_F(PairingExchangeTable, ReportsACryptoFailure) {
  ASSERT_NO_FATAL_FAILURE(send_the_request());
  console.send(std::span<const std::byte>{encrypted_success_frame()});
  crypto.fail_hmac = true;
  const auto failed = exchange.read_reply_header();
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), PairingExchange::Error::crypto_failure);
  EXPECT_TRUE(console.hung_up());
}

TEST_F(PairingExchangeTable, ParsesBytesPastTheContentLength) {
  ASSERT_NO_FATAL_FAILURE(send_the_request());
  console.send(std::span<const std::byte>{
      encrypted_success_frame("PS4-Nickname:Den\r\nRP-KeyType:2\r\n")});
  const auto result = exchange.read_reply_header();
  ASSERT_TRUE(reached<remote_play::RegisteredConsole>(result));
  EXPECT_EQ(std::string_view(console_of(result).nickname.data()), "Den");
  EXPECT_EQ(console_of(result).key_type, 2U);
  EXPECT_TRUE(console.hung_up());
}

TEST_F(PairingExchangeTable, RefusesGarbagePastTheContentLength) {
  ASSERT_NO_FATAL_FAILURE(send_the_request());
  console.send(
      std::span<const std::byte>{encrypted_success_frame("garbage\r\n")});
  const auto rejected = exchange.read_reply_header();
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), PairingExchange::Error::invalid_payload);
}

TEST_F(PairingExchangeTable, RefusesAnUnknownFamilyBeforeConnecting) {
  const auto refused =
      exchange.stage(HomeConsole::bytes("H"), HomeConsole::bytes("P"),
                     static_cast<remote_play::RegistrationTarget>(99));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), PairingExchange::Error::invalid_argument);
  EXPECT_FALSE(console.accept());
  const auto idle = exchange.await_connection();
  ASSERT_FALSE(idle);
  EXPECT_EQ(idle.error(), PairingExchange::Error::invalid_state);
}

}  // namespace
}  // namespace consoles_test
