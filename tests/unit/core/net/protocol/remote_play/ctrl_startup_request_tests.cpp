#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace platform = libchiaking::platform;
using remote_play::CtrlCodec;
using remote_play::RegistrationTarget;
using remote_play::VideoCodec;

static_assert(!std::is_copy_constructible_v<remote_play::CtrlStartupRequest>);
static_assert(
    std::is_nothrow_move_constructible_v<remote_play::CtrlStartupRequest>);

class StartupRequestCrypto final : public platform::CryptoProvider {
 public:
  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }

  auto hmac_sha256(std::span<const std::byte> key,
                   std::span<const std::byte> input,
                   std::span<std::byte> digest)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    EXPECT_EQ(key.size(), 16U);
    EXPECT_EQ(input.size(), 24U);
    EXPECT_EQ(digest.size(), 32U);
    if (fail_hmac)
      return libchiaking::unexpected(platform::CryptoError::backend_failure);
    for (std::size_t i = 16; i < 23 && i < input.size(); ++i)
      EXPECT_EQ(input[i], std::byte{0}) << "counter byte " << i;
    if (input.size() > 23)
      counters.push_back(std::to_integer<unsigned>(input[23]));
    for (auto& byte : digest) byte = std::byte{0};
    return {};
  }

  auto aes128_encrypt_block(std::span<const std::byte> key,
                            std::span<const std::byte> block,
                            std::span<std::byte> output)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    EXPECT_EQ(key.size(), 16U);
    EXPECT_EQ(block.size(), 16U);
    EXPECT_EQ(output.size(), 16U);
    for (auto& byte : output) byte = std::byte{0};
    return {};
  }

  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }

  std::vector<unsigned> counters;
  bool fail_hmac{false};
};

class CtrlStartupRequest : public ::testing::Test {
 protected:
  static constexpr std::array<std::byte, 16> key{};
  static constexpr std::array<std::byte, 32> did{};

  static auto expected(std::string_view path, std::string_view version,
                       bool bitrate, std::string_view streaming)
      -> std::string {
    std::string result = "GET ";
    result += path;
    result +=
        " HTTP/1.1\r\n"
        "Host: 127.0.0.1:9295\r\n"
        "User-Agent: remoteplay Windows\r\n"
        "Connection: keep-alive\r\n"
        "Content-Length: 0\r\n"
        "RP-Auth: AAAAAAAAAAAAAAAAAAAAAA==\r\n"
        "RP-Version: ";
    result += version;
    result +=
        "\r\n"
        "RP-Did: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\r\n"
        "RP-ControllerType: 3\r\n"
        "RP-ClientType: 11\r\n"
        "RP-OSType: V2luMTAuMC4wAA==\r\n"
        "RP-ConPath: 1\r\n";
    if (bitrate) result += "RP-StartBitrate: AAAAAA==\r\n";
    if (!streaming.empty()) {
      result += "RP-StreamingType: ";
      result += streaming;
      result +=
          "\r\n"
          "RP-PadProcNo: 3\r\n";
    }
    result += "\r\n";
    return result;
  }

  void expect_request(RegistrationTarget target, VideoCodec codec,
                      std::string_view path, std::string_view version,
                      bool bitrate, std::string_view streaming,
                      std::uint64_t next_counter) {
    const auto request = CtrlCodec::encode_startup_request(
        crypto, target, material, "127.0.0.1", 9295, key, did, codec);
    ASSERT_TRUE(request);
    EXPECT_EQ(request.value().view(),
              expected(path, version, bitrate, streaming));
    EXPECT_EQ(request.value().next_crypt_counter(), next_counter);
    ASSERT_EQ(crypto.counters.size(), next_counter);
    for (std::size_t i = 0; i < crypto.counters.size(); ++i)
      EXPECT_EQ(crypto.counters[i], i) << "counter " << i;
  }

  auto encode(std::string_view host, std::uint16_t port,
              std::span<const std::byte> with_key,
              std::span<const std::byte> with_did,
              RegistrationTarget target = RegistrationTarget::ps5_1) {
    return CtrlCodec::encode_startup_request(crypto, target, material, host,
                                             port, with_key, with_did,
                                             VideoCodec::h264);
  }

  StartupRequestCrypto crypto;
  remote_play::AuthMaterial material;
};

TEST_F(CtrlStartupRequest, ForPs4Version8UsesTheSessionPathAndThreeCounters) {
  expect_request(RegistrationTarget::ps4_8, VideoCodec::h264,
                 "/sce/rp/session/ctrl", "8.0", false, "", 3);
}

TEST_F(CtrlStartupRequest, ForPs4Version9UsesTheSessionPathAndThreeCounters) {
  expect_request(RegistrationTarget::ps4_9, VideoCodec::h264,
                 "/sce/rp/session/ctrl", "9.0", false, "", 3);
}

TEST_F(CtrlStartupRequest, ForPs4Version10AddsTheStartBitrate) {
  expect_request(RegistrationTarget::ps4_10, VideoCodec::h264,
                 "/sie/ps4/rp/sess/ctrl", "10.0", true, "", 4);
}

TEST_F(CtrlStartupRequest, ForPs5NamesH264AsStreamingTypeOne) {
  expect_request(RegistrationTarget::ps5_1, VideoCodec::h264,
                 "/sie/ps5/rp/sess/ctrl", "1.0", true, "AQAAAA==", 5);
}

TEST_F(CtrlStartupRequest, ForPs5NamesH265AsStreamingTypeTwo) {
  expect_request(RegistrationTarget::ps5_1, VideoCodec::h265,
                 "/sie/ps5/rp/sess/ctrl", "1.0", true, "AgAAAA==", 5);
}

TEST_F(CtrlStartupRequest, ForPs5NamesH265HdrAsStreamingTypeThree) {
  expect_request(RegistrationTarget::ps5_1, VideoCodec::h265_hdr,
                 "/sie/ps5/rp/sess/ctrl", "1.0", true, "AwAAAA==", 5);
}

TEST_F(CtrlStartupRequest, RefusesInvalidArgumentsBeforeAnyCrypto) {
  const auto target =
      encode("h", 9295, key, did, static_cast<RegistrationTarget>(99));
  ASSERT_FALSE(target);
  EXPECT_EQ(target.error(), remote_play::CtrlRequestError::invalid_target);
  const auto host = encode("h\r\nX: y", 9295, key, did);
  ASSERT_FALSE(host);
  EXPECT_EQ(host.error(), remote_play::CtrlRequestError::invalid_host);
  const auto port = encode("h", 0, key, did);
  ASSERT_FALSE(port);
  EXPECT_EQ(port.error(), remote_play::CtrlRequestError::invalid_port);
  const auto short_key = encode("h", 9295, std::span(key).first(15), did);
  ASSERT_FALSE(short_key);
  EXPECT_EQ(short_key.error(), remote_play::CtrlRequestError::invalid_key);
  const auto short_did = encode("h", 9295, key, std::span(did).first(31));
  ASSERT_FALSE(short_did);
  EXPECT_EQ(short_did.error(), remote_play::CtrlRequestError::invalid_did);
  EXPECT_TRUE(crypto.counters.empty());
}

TEST_F(CtrlStartupRequest, RefusesARequestThatOverflows) {
  const auto overflow = encode(std::string(900, 'h'), 9295, key, did);
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error(), remote_play::CtrlRequestError::overflow);
}

TEST_F(CtrlStartupRequest, ReportsACryptoFailure) {
  crypto.fail_hmac = true;
  const auto failed = encode("h", 9295, key, did);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), remote_play::CtrlRequestError::crypto_failure);
}

}  // namespace
}  // namespace remote_play_test
