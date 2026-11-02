#include "src/core/net/protocol/remote_play/session_codec.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using remote_play::RegistrationTarget;
using remote_play::ResponseCodecError;
using remote_play::SessionRefusal;

static_assert(!std::is_copy_constructible_v<remote_play::SessionRequest>);
static_assert(
    std::is_nothrow_move_constructible_v<remote_play::SessionRequest>);
static_assert(!std::is_copy_constructible_v<remote_play::SessionGrant>);
static_assert(
    std::is_nothrow_move_constructible_v<remote_play::SessionGrant>);

class SessionRequestEncoding : public ::testing::Test {
 protected:
  static constexpr std::array<std::byte, 16> key{
      std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78},
      std::byte{0x9a}, std::byte{0xbc}, std::byte{0xde}, std::byte{0xf0},
      std::byte{0},    std::byte{0x11}, std::byte{0x22}, std::byte{0x33},
      std::byte{0x44}, std::byte{0x55}, std::byte{0x66}, std::byte{0x77},
  };

  static auto full_key() -> std::array<std::byte, 16> {
    std::array<std::byte, 16> full{};
    for (std::size_t i = 0; i < full.size(); ++i)
      full[i] = static_cast<std::byte>(i + 1);
    return full;
  }
};

TEST_F(SessionRequestEncoding, MatchesThePs4EightVector) {
  const auto ps4_8 = remote_play::SessionCodec::encode_request(
      RegistrationTarget::ps4_8, "192.168.1.2", 9295, key);
  ASSERT_TRUE(ps4_8);
  EXPECT_EQ(ps4_8.value().view(),
            "GET /sce/rp/session HTTP/1.1\r\n"
            "Host: 192.168.1.2:9295\r\n"
            "User-Agent: remoteplay Windows\r\n"
            "Connection: close\r\n"
            "Content-Length: 0\r\n"
            "RP-Registkey: 123456789abcdef0\r\n"
            "Rp-Version: 8.0\r\n"
            "\r\n");
}

TEST_F(SessionRequestEncoding, MatchesThePs4NineVector) {
  const auto ps4_9 = remote_play::SessionCodec::encode_request(
      RegistrationTarget::ps4_9, "192.168.1.2", 9295, key);
  ASSERT_TRUE(ps4_9);
  EXPECT_EQ(ps4_9.value().view(),
            "GET /sce/rp/session HTTP/1.1\r\n"
            "Host: 192.168.1.2:9295\r\n"
            "User-Agent: remoteplay Windows\r\n"
            "Connection: close\r\n"
            "Content-Length: 0\r\n"
            "RP-Registkey: 123456789abcdef0\r\n"
            "Rp-Version: 9.0\r\n"
            "\r\n");
}

TEST_F(SessionRequestEncoding, MatchesThePs4TenVector) {
  const auto ps4_10 = remote_play::SessionCodec::encode_request(
      RegistrationTarget::ps4_10, "192.168.1.2", 9295, key);
  ASSERT_TRUE(ps4_10);
  EXPECT_EQ(ps4_10.value().view(),
            "GET /sie/ps4/rp/sess/init HTTP/1.1\r\n"
            "Host: 192.168.1.2:9295\r\n"
            "User-Agent: remoteplay Windows\r\n"
            "Connection: close\r\n"
            "Content-Length: 0\r\n"
            "RP-Registkey: 123456789abcdef0\r\n"
            "Rp-Version: 10.0\r\n"
            "\r\n");
}

TEST_F(SessionRequestEncoding, MatchesThePs5Vector) {
  const auto ps5_1 = remote_play::SessionCodec::encode_request(
      RegistrationTarget::ps5_1, "192.168.1.2", 9295, key);
  ASSERT_TRUE(ps5_1);
  EXPECT_EQ(ps5_1.value().view(),
            "GET /sie/ps5/rp/sess/init HTTP/1.1\r\n"
            "Host: 192.168.1.2:9295\r\n"
            "User-Agent: remoteplay Windows\r\n"
            "Connection: close\r\n"
            "Content-Length: 0\r\n"
            "RP-Registkey: 123456789abcdef0\r\n"
            "Rp-Version: 1.0\r\n"
            "\r\n");
}

TEST_F(SessionRequestEncoding, TruncatesTheKeyAtItsFirstZeroByte) {
  constexpr std::array<std::byte, 16> empty_key{};
  const auto empty = remote_play::SessionCodec::encode_request(
      RegistrationTarget::ps4_10, "h", 1, empty_key);
  ASSERT_TRUE(empty);
  EXPECT_NE(empty.value().view().find("RP-Registkey: \r\n"),
            std::string_view::npos);
}

TEST_F(SessionRequestEncoding, SendsEveryByteOfAKeyWithoutZeros) {
  const auto all = remote_play::SessionCodec::encode_request(
      RegistrationTarget::ps5_1, "h", 9295, full_key());
  ASSERT_TRUE(all);
  EXPECT_NE(all.value().view().find(
                "RP-Registkey: 0102030405060708090a0b0c0d0e0f10\r\n"),
            std::string_view::npos);
}

TEST_F(SessionRequestEncoding, RefusesInvalidTargetsHostsPortsAndKeys) {
  const auto full = full_key();
  const auto invalid_target = remote_play::SessionCodec::encode_request(
      static_cast<RegistrationTarget>(99), "h", 9295, full);
  ASSERT_FALSE(invalid_target);
  EXPECT_EQ(invalid_target.error(),
            remote_play::RequestCodecError::invalid_target);
  const auto invalid_host = remote_play::SessionCodec::encode_request(
      RegistrationTarget::ps5_1, "a\r\nb", 9295, full);
  ASSERT_FALSE(invalid_host);
  EXPECT_EQ(invalid_host.error(), remote_play::RequestCodecError::invalid_host);
  const auto invalid_port = remote_play::SessionCodec::encode_request(
      RegistrationTarget::ps5_1, "h", 0, full);
  ASSERT_FALSE(invalid_port);
  EXPECT_EQ(invalid_port.error(), remote_play::RequestCodecError::invalid_port);
  const auto invalid_key = remote_play::SessionCodec::encode_request(
      RegistrationTarget::ps5_1, "h", 9295,
      std::span<const std::byte>{full.data(), 15});
  ASSERT_FALSE(invalid_key);
  EXPECT_EQ(invalid_key.error(), remote_play::RequestCodecError::invalid_key);
}

TEST_F(SessionRequestEncoding, RefusesAHostThatOverflowsTheRequest) {
  const std::string huge_host(400, 'a');
  const auto overflow = remote_play::SessionCodec::encode_request(
      RegistrationTarget::ps5_1, huge_host, 9295, full_key());
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error(), remote_play::RequestCodecError::overflow);
}

class SessionResponseParsing : public ::testing::Test {
 protected:
  static constexpr std::string_view accepted =
      "HTTP/1.1 200 OK\r\nRP-Nonce: AAECAwQFBgcICQoLDA0ODw==\r\n\r\n";

  static auto parse(RegistrationTarget target, std::string_view text) {
    return remote_play::SessionCodec::parse_response(
        target,
        std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(text.data()), text.size()});
  }

  static auto rejection(std::string_view reason, std::string_view version)
      -> std::string {
    return std::string("HTTP/1.1 403 F\r\nRP-Application-Reason: ") +
           std::string(reason) + "\r\nRP-Version: " + std::string(version) +
           "\r\n\r\n";
  }
};

TEST_F(SessionResponseParsing, NeedsTheWholeHeaderBlock) {
  for (std::size_t n = 0; n < accepted.size(); ++n) {
    const auto partial =
        parse(RegistrationTarget::ps5_1, accepted.substr(0, n));
    ASSERT_FALSE(partial) << "length " << n;
    EXPECT_EQ(partial.error(), ResponseCodecError::incomplete)
        << "length " << n;
  }
}

TEST_F(SessionResponseParsing, AcceptsAnOkWithItsNonce) {
  const auto result = parse(RegistrationTarget::ps5_1, accepted);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.value());
  EXPECT_EQ(result.value()->header_size(), accepted.size());
  for (std::size_t i = 0; i < 16; ++i)
    EXPECT_EQ(result.value()->nonce()[i], static_cast<std::byte>(i))
        << "index " << i;
}

TEST_F(SessionResponseParsing, StopsTheHeaderBeforeAnyBody) {
  const auto with_body =
      parse(RegistrationTarget::ps5_1, std::string(accepted) + "body");
  ASSERT_TRUE(with_body);
  EXPECT_EQ(with_body.value()->header_size(), accepted.size());
}

TEST_F(SessionResponseParsing, AVersionMismatchRetriesTheNamedPs4Version) {
  const auto known =
      parse(RegistrationTarget::ps4_10,
            "HTTP/1.1 403 Forbidden\r\nRP-Application-Reason: 80108b11\r\n"
            "rp-version: 9.0\r\n\r\n");
  ASSERT_TRUE(known);
  EXPECT_EQ(known.value().error().kind(), SessionRefusal::Kind::version_mismatch);
  EXPECT_EQ(known.value().error().retry_target(), RegistrationTarget::ps4_9);
  EXPECT_EQ(known.value().error().application_reason(), 0x80108b11U);
}

TEST_F(SessionResponseParsing, AnOlderPs4VersionFallsBackToNine) {
  const auto fallback =
      parse(RegistrationTarget::ps4_10, rejection("80108bff", "5.0"));
  ASSERT_TRUE(fallback);
  EXPECT_EQ(fallback.value().error().retry_target(), RegistrationTarget::ps4_9);
}

TEST_F(SessionResponseParsing, AnUnknownVersionIsAMismatchWithoutRetry) {
  const auto unknown =
      parse(RegistrationTarget::ps4_10, rejection("80108bff", "7.5"));
  ASSERT_TRUE(unknown);
  EXPECT_EQ(unknown.value().error().kind(), SessionRefusal::Kind::version_mismatch);
  EXPECT_FALSE(unknown.value().error().retry_target());
}

TEST_F(SessionResponseParsing, APs5NeverRetriesAPs4VersionItWasOffered) {
  const auto cross_family =
      parse(RegistrationTarget::ps5_1, rejection("80108b11", "9.0"));
  ASSERT_TRUE(cross_family);
  EXPECT_FALSE(cross_family.value().error().retry_target());
}

TEST_F(SessionResponseParsing, ABusyConsoleIsARejectionWithItsReason) {
  const auto busy =
      parse(RegistrationTarget::ps4_10,
            "HTTP/1.1 403 F\r\nRP-Application-Reason: 80108b10\r\n\r\n");
  ASSERT_TRUE(busy);
  EXPECT_EQ(busy.value().error().kind(), SessionRefusal::Kind::rejected);
  EXPECT_EQ(busy.value().error().application_reason(), 0x80108b10U);
}

TEST_F(SessionResponseParsing, TheVersionAlreadyTriedIsNotRetried) {
  const auto same_version =
      parse(RegistrationTarget::ps4_9, rejection("80108b11", "9.0"));
  ASSERT_TRUE(same_version);
  EXPECT_EQ(same_version.value().error().kind(), SessionRefusal::Kind::version_mismatch);
  EXPECT_FALSE(same_version.value().error().retry_target());
}

TEST_F(SessionResponseParsing, ReadsUpperCaseReasonsButNotPrefixedOnes) {
  const auto upper_hex =
      parse(RegistrationTarget::ps4_9,
            "HTTP/1.1 403 F\r\nRP-Application-Reason: 80108B10\r\n\r\n");
  ASSERT_TRUE(upper_hex);
  EXPECT_EQ(upper_hex.value().error().application_reason(), 0x80108b10U);
  const auto prefixed_hex =
      parse(RegistrationTarget::ps4_9,
            "HTTP/1.1 403 F\r\nRP-Application-Reason: 0X80108B10\r\n\r\n");
  ASSERT_TRUE(prefixed_hex);
  EXPECT_EQ(prefixed_hex.value().error().application_reason(), 0U);
}

TEST_F(SessionResponseParsing, APs5OfferedAnOldVersionRetriesPs4Nine) {
  const auto ps5_nonsense =
      parse(RegistrationTarget::ps5_1, rejection("80108b11", "5.0"));
  ASSERT_TRUE(ps5_nonsense);
  EXPECT_EQ(ps5_nonsense.value().error().kind(), SessionRefusal::Kind::version_mismatch);
  EXPECT_EQ(ps5_nonsense.value().error().retry_target(), RegistrationTarget::ps4_9);
}

TEST_F(SessionResponseParsing, ABlankVersionIsAMismatchWithoutRetry) {
  for (const auto* empty : {"RP-Version: \r\n", "RP-Version:\r\n"}) {
    const auto blank = parse(
        RegistrationTarget::ps5_1,
        std::string("HTTP/1.1 403 F\r\nRP-Application-Reason: 80108bff\r\n") +
            empty + "\r\n");
    ASSERT_TRUE(blank) << empty;
    EXPECT_EQ(blank.value().error().kind(), SessionRefusal::Kind::version_mismatch)
        << empty;
    EXPECT_FALSE(blank.value().error().retry_target()) << empty;
  }
}

TEST_F(SessionResponseParsing, AcceptsBareLineFeedsButNotBareReturns) {
  const auto bare_lf =
      parse(RegistrationTarget::ps4_10,
            "HTTP/1.1 403 F\nRP-Application-Reason: 80108b11\nRP-Version: "
            "9.0\r\n\r\n");
  ASSERT_TRUE(bare_lf);
  EXPECT_EQ(bare_lf.value().error().retry_target(), RegistrationTarget::ps4_9);
  const auto bare_cr =
      parse(RegistrationTarget::ps4_10,
            "HTTP/1.1 403 F\r\nRP-Application-Reason: 80108b11\rRP-Version: "
            "8.0\r\n\r\n");
  ASSERT_FALSE(bare_cr);
  EXPECT_EQ(bare_cr.error(), ResponseCodecError::invalid);
}

TEST_F(SessionResponseParsing, TrimsPaddingAroundTheVersion) {
  const auto padded =
      parse(RegistrationTarget::ps4_10, rejection("80108b11", " 9.0"));
  ASSERT_TRUE(padded);
  EXPECT_EQ(padded.value().error().kind(), SessionRefusal::Kind::version_mismatch);
  EXPECT_EQ(padded.value().error().retry_target(), RegistrationTarget::ps4_9);
}

TEST_F(SessionResponseParsing, ReadsTheStatusAsAPlainInteger) {
  const auto leading_zero =
      parse(RegistrationTarget::ps5_1,
            "HTTP/1.1 0200 OK\r\nRP-Nonce: AAECAwQFBgcICQoLDA0ODw==\r\n\r\n");
  ASSERT_TRUE(leading_zero);
  EXPECT_TRUE(leading_zero.value());
  const auto odd_status =
      parse(RegistrationTarget::ps5_1,
            "HTTP/1.1 999\r\nRP-Application-Reason: 80108b10\r\n\r\n");
  ASSERT_TRUE(odd_status);
  EXPECT_EQ(odd_status.value().error().status(), 999);
  EXPECT_EQ(odd_status.value().error().kind(), SessionRefusal::Kind::rejected);
  const auto wide_status =
      parse(RegistrationTarget::ps4_9, "HTTP/1.1 65736\r\n\r\n");
  ASSERT_TRUE(wide_status);
  EXPECT_EQ(wide_status.value().error().status(), 65736);
  EXPECT_EQ(wide_status.value().error().http_status(), 0);
  EXPECT_EQ(wide_status.value().error().kind(), SessionRefusal::Kind::rejected);
  const auto negative_status =
      parse(RegistrationTarget::ps4_9, "HTTP/1.1 -403\r\n\r\n");
  ASSERT_TRUE(negative_status);
  EXPECT_EQ(negative_status.value().error().status(), -403);
}

TEST_F(SessionResponseParsing, RefusesANulInAHeader) {
  const auto nul =
      parse(RegistrationTarget::ps5_1,
            std::string("HTTP/1.1 200 OK\r\nX: a") + std::string(1, '\0') +
                "\r\nRP-Nonce: AAECAwQFBgcICQoLDA0ODw==\r\n\r\n");
  ASSERT_FALSE(nul);
  EXPECT_EQ(nul.error(), ResponseCodecError::invalid);
}

TEST_F(SessionResponseParsing, RefusesAStatusLineWithoutANumber) {
  const auto zero_status =
      parse(RegistrationTarget::ps5_1, "HTTP/1.1 OK\r\n\r\n");
  ASSERT_FALSE(zero_status);
  EXPECT_EQ(zero_status.error(), ResponseCodecError::invalid);
}

TEST_F(SessionResponseParsing, MatchesTheNonceHeaderCaseSensitively) {
  const auto wrong_case =
      parse(RegistrationTarget::ps5_1,
            "HTTP/1.1 200 OK\r\nrp-nonce: AAECAwQFBgcICQoLDA0ODw==\r\n\r\n");
  ASSERT_TRUE(wrong_case);
  EXPECT_EQ(wrong_case.value().error().kind(), SessionRefusal::Kind::rejected);
}

TEST_F(SessionResponseParsing, RefusesAShortNonce) {
  const auto short_nonce =
      parse(RegistrationTarget::ps5_1,
            "HTTP/1.1 200 OK\r\nRP-Nonce: AAECAwQFBgcICQoLDA0O\r\n\r\n");
  ASSERT_FALSE(short_nonce);
  EXPECT_EQ(short_nonce.error(), ResponseCodecError::invalid_nonce);
}

TEST_F(SessionResponseParsing, AcceptsNoncanonicalAndUnpaddedNonces) {
  const auto noncanonical =
      parse(RegistrationTarget::ps5_1,
            "HTTP/1.1 200 OK\r\nRP-Nonce: AAECAwQFBgcICQoLDA0ODx==\r\n\r\n");
  ASSERT_TRUE(noncanonical);
  EXPECT_TRUE(noncanonical.value());
  EXPECT_EQ(noncanonical.value()->nonce()[15], std::byte{0x0f});
  const auto unpadded =
      parse(RegistrationTarget::ps5_1,
            "HTTP/1.1 200 OK\r\nRP-Nonce: AAECAwQFBgcICQoLDA0ODw\r\n\r\n");
  ASSERT_TRUE(unpadded);
  EXPECT_TRUE(unpadded.value());
  EXPECT_EQ(unpadded.value()->nonce()[15], std::byte{0x0f});
}

TEST_F(SessionResponseParsing, RefusesANonceWithANonBase64Character) {
  const auto bad_character =
      parse(RegistrationTarget::ps5_1,
            "HTTP/1.1 200 OK\r\nRP-Nonce: AAECAwQFBgcICQoLDA0OD!==\r\n\r\n");
  ASSERT_FALSE(bad_character);
  EXPECT_EQ(bad_character.error(), ResponseCodecError::invalid_nonce);
}

TEST_F(SessionResponseParsing, AnOkWithoutANonceStillReportsAMismatch) {
  const auto missing_nonce =
      parse(RegistrationTarget::ps4_10,
            "HTTP/1.1 200 OK\r\nRP-Application-Reason: 80108b11\r\n"
            "RP-Version: 9.0\r\n\r\n");
  ASSERT_TRUE(missing_nonce);
  EXPECT_EQ(missing_nonce.value().error().kind(), SessionRefusal::Kind::version_mismatch);
  EXPECT_EQ(missing_nonce.value().error().retry_target(), RegistrationTarget::ps4_9);
}

TEST_F(SessionResponseParsing, RefusesSomethingThatIsNotHttp) {
  const auto malformed = parse(RegistrationTarget::ps5_1, "garbage\r\n\r\n");
  ASSERT_FALSE(malformed);
  EXPECT_EQ(malformed.error(), ResponseCodecError::invalid);
}

TEST_F(SessionResponseParsing, RefusesAnUnknownTarget) {
  const auto invalid_target =
      parse(static_cast<RegistrationTarget>(99), "HTTP/1.1 403\r\n\r\n");
  ASSERT_FALSE(invalid_target);
  EXPECT_EQ(invalid_target.error(), ResponseCodecError::invalid_target);
}

TEST_F(SessionResponseParsing, RefusesHeadersOverTheSizeLimit) {
  const auto too_large =
      parse(RegistrationTarget::ps5_1,
            "HTTP/1.1 200 OK\r\nX-Pad: " + std::string(500, 'a') + "\r\n\r\n");
  ASSERT_FALSE(too_large);
  EXPECT_EQ(too_large.error(), ResponseCodecError::too_large);
  const auto unterminated =
      parse(RegistrationTarget::ps5_1, std::string(512, 'x'));
  ASSERT_FALSE(unterminated);
  EXPECT_EQ(unterminated.error(), ResponseCodecError::too_large);
}

TEST_F(SessionResponseParsing, OnlyTheFirstNonceHeaderCounts) {
  const auto duplicate = parse(RegistrationTarget::ps5_1,
                               "HTTP/1.1 200 OK\r\nRP-Nonce: bad\r\n"
                               "RP-Nonce: AAECAwQFBgcICQoLDA0ODw==\r\n\r\n");
  ASSERT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error(), ResponseCodecError::invalid_nonce);
  const auto first_nonce =
      parse(RegistrationTarget::ps5_1,
            "HTTP/1.1 200 OK\r\nRP-Nonce: AAECAwQFBgcICQoLDA0ODw==\r\n"
            "RP-Nonce: bad\r\n\r\n");
  ASSERT_TRUE(first_nonce);
  EXPECT_TRUE(first_nonce.value());
}

TEST_F(SessionResponseParsing, TakesTheFirstReadableReason) {
  const auto skipped = parse(RegistrationTarget::ps5_1,
                             "HTTP/1.1 403 F\r\nRP-Application-Reason: xyz\r\n"
                             "RP-Application-Reason: 80108b10\r\n"
                             "RP-Application-Reason: 80108b11\r\n\r\n");
  ASSERT_TRUE(skipped);
  EXPECT_EQ(skipped.value().error().application_reason(), 0x80108b10U);
}

TEST_F(SessionResponseParsing, AnUnreadableReasonCountsAsZero) {
  const auto bad_reason =
      parse(RegistrationTarget::ps5_1,
            "HTTP/1.1 403 F\r\nRP-Application-Reason: xyz\r\n\r\n");
  ASSERT_TRUE(bad_reason);
  EXPECT_EQ(bad_reason.value().error().kind(), SessionRefusal::Kind::rejected);
  EXPECT_EQ(bad_reason.value().error().application_reason(), 0U);
  const auto suffixed_reason =
      parse(RegistrationTarget::ps4_10, rejection("80108b11;x", "9.0"));
  ASSERT_TRUE(suffixed_reason);
  EXPECT_EQ(suffixed_reason.value().error().application_reason(), 0U);
  EXPECT_FALSE(suffixed_reason.value().error().retry_target());
}

}  // namespace
}  // namespace remote_play_test
