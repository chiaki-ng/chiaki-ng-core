#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace platform = libchiaking::platform;
using remote_play::CtrlCodec;
using remote_play::CtrlResponseError;
using remote_play::RegistrationTarget;

static_assert(!std::is_copy_constructible_v<remote_play::CtrlGrant>);
static_assert(std::is_nothrow_move_constructible_v<remote_play::CtrlGrant>);

class ResponseCrypto final : public platform::CryptoProvider {
 public:
  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }

  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte> input,
                   std::span<std::byte> output)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    EXPECT_EQ(input.size(), 24U);
    EXPECT_EQ(output.size(), 32U);
    for (std::size_t i = 16; i < input.size(); ++i)
      EXPECT_EQ(input[i], std::byte{0}) << "counter byte " << i;
    ++decrypt_calls;
    if (fail)
      return libchiaking::unexpected(platform::CryptoError::backend_failure);
    for (auto& byte : output) byte = std::byte{0};
    return {};
  }

  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>,
                            std::span<std::byte> output)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    for (auto& byte : output) byte = std::byte{0};
    return {};
  }

  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }

  unsigned decrypt_calls{};
  bool fail{false};
};

class CtrlResponse : public ::testing::Test {
 protected:
  static auto read(std::string_view text) {
    return CtrlCodec::parse_response(
        {reinterpret_cast<const std::byte*>(text.data()), text.size()});
  }

  static auto parse(std::string_view text)
      -> libchiaking::Expected<remote_play::CtrlGrant, CtrlResponseError> {
    auto parsed = read(text);
    if (!parsed) return libchiaking::unexpected(parsed.error());
    EXPECT_TRUE(parsed.value()) << "it was refused";
    if (!parsed.value()) return remote_play::CtrlGrant{};
    return std::move(parsed.value().value());
  }

  static auto refusal(std::string_view text) -> std::optional<std::uint16_t> {
    const auto parsed = read(text);
    if (!parsed || parsed.value()) return std::nullopt;
    return parsed.value().error().status();
  }

  static auto refused_as(std::string_view text, CtrlResponseError error)
      -> testing::AssertionResult {
    const auto parsed = read(text);
    if (parsed) return testing::AssertionFailure() << "it was read";
    if (parsed.error() != error)
      return testing::AssertionFailure() << "it failed with another error";
    return testing::AssertionSuccess();
  }

  static constexpr std::string_view accepted_frame =
      "HTTP/1.1 200 OK\r\n"
      "RP-Server-Type: AAECAwQFBgcICQoLDA0ODw==\r\n"
      "RP-Prohibit: 1\r\n\r\n";
};

TEST_F(CtrlResponse, IsIncompleteUntilTheHeadersEnd) {
  for (std::size_t n = 0; n < accepted_frame.size(); ++n)
    EXPECT_TRUE(
        refused_as(accepted_frame.substr(0, n), CtrlResponseError::incomplete))
        << "prefix " << n;
}

TEST_F(CtrlResponse, ReadsTheServerTypeAndProhibitHeaders) {
  const auto parsed = parse(accepted_frame);
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(parsed.value().prohibited());
  ASSERT_TRUE(parsed.value().encrypted_server_type());
  EXPECT_EQ(parsed.value().header_size(), accepted_frame.size());
  for (std::size_t i = 0; i < 16; ++i)
    EXPECT_EQ((*parsed.value().encrypted_server_type())[i],
              static_cast<std::byte>(i))
        << "byte " << i;
}

TEST_F(CtrlResponse, MeasuresOnlyTheHeadersWhenABodyFollows) {
  const auto with_body = parse(std::string(accepted_frame) + "body");
  ASSERT_TRUE(with_body);
  EXPECT_EQ(with_body.value().header_size(), accepted_frame.size());
}

TEST_F(CtrlResponse, TreatsBothHeadersAsOptional) {
  const auto empty = parse("HTTP/1.1 200 OK\r\n\r\n");
  ASSERT_TRUE(empty);
  EXPECT_FALSE(empty.value().encrypted_server_type());
  EXPECT_FALSE(empty.value().prohibited());
}

TEST_F(CtrlResponse, IgnoresAShortServerType) {
  const auto short_type =
      parse("HTTP/1.1 200 OK\r\nRP-Server-Type: AQI=\r\n\r\n");
  ASSERT_TRUE(short_type);
  EXPECT_FALSE(short_type.value().encrypted_server_type());
}

TEST_F(CtrlResponse, KeepsTheFirstOfRepeatedHeaders) {
  const auto first_type = parse(
      "HTTP/1.1 200 OK\r\nRP-Server-Type: AAECAwQFBgcICQoLDA0ODx==\r\n"
      "RP-Server-Type: AQI=\r\nRP-Prohibit: 1\r\nRP-Prohibit: 0\r\n\r\n");
  ASSERT_TRUE(first_type);
  ASSERT_TRUE(first_type.value().encrypted_server_type());
  EXPECT_TRUE(first_type.value().prohibited());
  EXPECT_EQ((*first_type.value().encrypted_server_type())[15], std::byte{0x0f});
}

TEST_F(CtrlResponse, RefusesAServerTypeThatIsNotBase64InAnyCopy) {
  EXPECT_TRUE(refused_as(
      "HTTP/1.1 200 OK\r\nRP-Server-Type: AAECAwQFBgcICQoLDA0ODw==\r\n"
      "RP-Server-Type: not!base64\r\n\r\n",
      CtrlResponseError::invalid));
}

TEST_F(CtrlResponse, MatchesHeaderNamesCaseSensitively) {
  const auto wrong_case = parse(
      "HTTP/1.1 200 OK\r\nrp-prohibit: 1\r\n"
      "rp-server-type: AAECAwQFBgcICQoLDA0ODw==\r\n\r\n");
  ASSERT_TRUE(wrong_case);
  EXPECT_FALSE(wrong_case.value().prohibited());
  EXPECT_FALSE(wrong_case.value().encrypted_server_type());
}

TEST_F(CtrlResponse, ReportsARejectionWithItsStatus) {
  EXPECT_EQ(refusal("HTTP/1.1 403 Forbidden\r\n\r\n"), 403U);
}

TEST_F(CtrlResponse, RefusesAServerTypeThatIsNotBase64) {
  EXPECT_TRUE(
      refused_as("HTTP/1.1 200 OK\r\nRP-Server-Type: not!base64\r\n\r\n",
                 CtrlResponseError::invalid));
}

TEST_F(CtrlResponse, IgnoresANonCanonicalServerType) {
  const auto noncanonical =
      parse("HTTP/1.1 200 OK\r\nRP-Server-Type: AQJ=\r\n\r\n");
  ASSERT_TRUE(noncanonical);
  EXPECT_FALSE(noncanonical.value().encrypted_server_type());
}

TEST_F(CtrlResponse, AcceptsAnUnpaddedServerType) {
  const auto unpadded = parse(
      "HTTP/1.1 200 OK\r\nRP-Server-Type: AAECAwQFBgcICQoLDA0ODw\r\n\r\n");
  ASSERT_TRUE(unpadded);
  EXPECT_TRUE(unpadded.value().encrypted_server_type());
}

TEST_F(CtrlResponse, RefusesAnOversizedServerType) {
  EXPECT_TRUE(refused_as(
      "HTTP/1.1 200 OK\r\nRP-Server-Type: AAECAwQFBgcICQoLDA0ODxAR\r\n\r\n",
      CtrlResponseError::invalid));
}

TEST_F(CtrlResponse, RefusesAMalformedStatusLine) {
  EXPECT_TRUE(refused_as("garbage\r\n\r\n", CtrlResponseError::invalid));
}

TEST_F(CtrlResponse, RefusesHeadersLongerThanItsBuffer) {
  EXPECT_TRUE(refused_as(
      "HTTP/1.1 200 OK\r\nX-Pad: " + std::string(500, 'x') + "\r\n\r\n",
      CtrlResponseError::too_large));
  EXPECT_TRUE(refused_as(std::string(512, 'x'), CtrlResponseError::too_large));
}

TEST_F(CtrlResponse, ParsesTheStatusCodeStrictly) {
  EXPECT_TRUE(refused_as("HTTP/1.1 200OK\r\n\r\n", CtrlResponseError::invalid));
  const auto leading_zero = parse("HTTP/1.1 0200 OK\r\n\r\n");
  ASSERT_TRUE(leading_zero);
  EXPECT_TRUE(
      refused_as("HTTP/1.1  200 OK\r\n\r\n", CtrlResponseError::invalid));
  EXPECT_TRUE(
      refused_as("HTTP/1.1 +200 OK\r\n\r\n", CtrlResponseError::invalid));
  EXPECT_TRUE(refused_as("HTTP/1.1 abc\r\n\r\n", CtrlResponseError::invalid));
}

TEST_F(CtrlResponse, KeepsWideAndWrappedStatusCodesAsRejections) {
  EXPECT_EQ(refusal("HTTP/1.1 2000 OK\r\n\r\n"), 2000U);
  EXPECT_EQ(refusal("HTTP/1.1 4294967496\r\n\r\n"), 0U);
}

TEST_F(CtrlResponse, AcceptsBareLineFeeds) {
  const auto bare_lf = parse("HTTP/1.1 200 OK\r\nA: b\nRP-Prohibit: 1\r\n\r\n");
  ASSERT_TRUE(bare_lf);
  EXPECT_TRUE(bare_lf.value().prohibited());
  const auto lf_status = parse("HTTP/1.1 200 OK\nRP-Prohibit: 1\r\n\r\n");
  ASSERT_TRUE(lf_status);
  EXPECT_TRUE(lf_status.value().prohibited());
}

TEST_F(CtrlResponse, TrimsWhitespaceAroundHeaderValues) {
  const auto two_spaces = parse(
      "HTTP/1.1 200 OK\r\nRP-Server-Type:  AAECAwQFBgcICQoLDA0ODw==\r\n\r\n");
  ASSERT_TRUE(two_spaces);
  EXPECT_TRUE(two_spaces.value().encrypted_server_type());
  const auto no_space = parse(
      "HTTP/1.1 200 OK\r\nRP-Server-Type:AAECAwQFBgcICQoLDA0ODw==\r\n\r\n");
  ASSERT_TRUE(no_space);
  EXPECT_TRUE(no_space.value().encrypted_server_type());
  const auto tab = parse("HTTP/1.1 200 OK\r\nRP-Prohibit:\t1\r\n\r\n");
  ASSERT_TRUE(tab);
  EXPECT_TRUE(tab.value().prohibited());
}

TEST_F(CtrlResponse, TreatsEmptyOrNonNumericValuesAsAbsent) {
  const auto empty_type = parse("HTTP/1.1 200 OK\r\nRP-Server-Type:\r\n\r\n");
  ASSERT_TRUE(empty_type);
  EXPECT_FALSE(empty_type.value().encrypted_server_type());
  const auto empty_prohibit = parse("HTTP/1.1 200 OK\r\nRP-Prohibit: \r\n\r\n");
  ASSERT_TRUE(empty_prohibit);
  EXPECT_FALSE(empty_prohibit.value().prohibited());
  const auto trailing =
      parse("HTTP/1.1 200 OK\r\nRP-Prohibit: 01trailing\r\n\r\n");
  ASSERT_TRUE(trailing);
  EXPECT_FALSE(trailing.value().prohibited());
  const auto overflow =
      parse("HTTP/1.1 200 OK\r\nRP-Prohibit: 4294967297\r\n\r\n");
  ASSERT_TRUE(overflow);
  EXPECT_FALSE(overflow.value().prohibited());
}

TEST_F(CtrlResponse, RefusesHeaderLinesWithoutANameAndColon) {
  EXPECT_TRUE(refused_as("HTTP/1.1 200 OK\r\nNoColon\r\n\r\n",
                         CtrlResponseError::invalid));
  EXPECT_TRUE(refused_as("HTTP/1.1 200 OK\r\nX\r\nRP-Prohibit: 1\r\n\r\n",
                         CtrlResponseError::invalid));
  EXPECT_TRUE(
      refused_as("HTTP/1.1 200 OK\r\n: v\r\n\r\n", CtrlResponseError::invalid));
}

class CtrlFinalizedResponse : public CtrlResponse {
 protected:
  auto finalize(const remote_play::CtrlGrant& response) {
    return CtrlCodec::finalize_response(response, crypto,
                                        RegistrationTarget::ps5_1, material);
  }

  ResponseCrypto crypto;
  remote_play::AuthMaterial material;
  static constexpr std::string_view typed =
      "HTTP/1.1 200 OK\r\n"
      "RP-Server-Type: AgAAAAAAAAAAAAAAAAAAAA==\r\n"
      "RP-Prohibit: 1\r\n\r\n";
};

TEST_F(CtrlFinalizedResponse, DecryptsTheServerTypeWithOneCounter) {
  const auto accepted = parse(typed);
  ASSERT_TRUE(accepted);
  const auto final = finalize(accepted.value());
  ASSERT_TRUE(final.server_type);
  EXPECT_EQ(*final.server_type, 2);
  EXPECT_TRUE(final.prohibited);
  EXPECT_EQ(final.next_remote_counter, 1U);
  EXPECT_EQ(crypto.decrypt_calls, 1U);
}

TEST_F(CtrlFinalizedResponse, SpendsTheCounterEvenWhenDecryptionFails) {
  const auto accepted = parse(typed);
  ASSERT_TRUE(accepted);
  crypto.fail = true;
  const auto failed = finalize(accepted.value());
  ASSERT_FALSE(failed.server_type);
  EXPECT_EQ(failed.server_type.error(),
            remote_play::ServerTypeError::undecryptable);
  EXPECT_TRUE(failed.prohibited);
  EXPECT_EQ(failed.next_remote_counter, 1U);
  EXPECT_EQ(crypto.decrypt_calls, 1U);
}

TEST_F(CtrlFinalizedResponse, SpendsNoCounterWithoutAServerType) {
  const auto absent = parse("HTTP/1.1 200 OK\r\n\r\n");
  ASSERT_TRUE(absent);
  const auto no_field = finalize(absent.value());
  ASSERT_FALSE(no_field.server_type);
  EXPECT_EQ(no_field.server_type.error(), remote_play::ServerTypeError::absent);
  EXPECT_EQ(no_field.next_remote_counter, 0U);
  EXPECT_EQ(crypto.decrypt_calls, 0U);
}

}  // namespace
}  // namespace remote_play_test
