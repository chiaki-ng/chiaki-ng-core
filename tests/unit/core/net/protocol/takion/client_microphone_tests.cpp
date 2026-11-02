#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/net/protocol/remote_play/stream/microphone_sender.hpp"
#include "src/core/net/protocol/takion/client/outbound.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/protocol/takion/client_outbound_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;
namespace remote_play = libchiaking::net::protocol::remote_play;

class MicrophoneLocal final : public takion::CryptoDirection {
 public:
  static auto keystream(std::uint64_t position) -> std::uint8_t {
    const auto low = static_cast<std::uint32_t>(position);
    return static_cast<std::uint8_t>(
        ((low * 0x9e3779b1U) >> 13U) ^ low ^
        static_cast<std::uint32_t>(position >> 32U));
  }

  auto apply_keystream(std::uint64_t position, std::span<const std::byte> input,
                       std::span<std::byte> bytes)
      -> takion::Expected<void, takion::CryptoError> override {
    if (input.data() != bytes.data()) std::ranges::copy(input, bytes.begin());
    for (std::size_t i = 0; i < bytes.size(); ++i)
      bytes[i] ^= static_cast<std::byte>(keystream(position + i));
    return {};
  }

  auto authenticate(std::uint64_t position, std::span<const std::byte> bytes)
      -> takion::Expected<takion::Mac, takion::CryptoError> override {
    std::uint32_t hash = 2166136261U ^ static_cast<std::uint32_t>(position) ^
                         static_cast<std::uint32_t>(position >> 32U);
    for (const auto byte : bytes)
      hash = (hash ^ std::to_integer<std::uint32_t>(byte)) * 16777619U;
    return takion::Mac{static_cast<std::byte>(hash >> 24U),
                       static_cast<std::byte>(hash >> 16U),
                       static_cast<std::byte>(hash >> 8U),
                       static_cast<std::byte>(hash)};
  }
};

class MicrophoneRemote final : public takion::CryptoDirection {
 public:
  auto authenticate(std::uint64_t, std::span<const std::byte>)
      -> takion::Expected<takion::Mac, takion::CryptoError> override {
    return takion::Mac{};
  }

  auto apply_keystream(std::uint64_t, std::span<const std::byte> input,
                       std::span<std::byte> bytes)
      -> takion::Expected<void, takion::CryptoError> override {
    if (input.data() != bytes.data()) std::ranges::copy(input, bytes.begin());
    return {};
  }
};

class MicrophoneCryptoProvider final : public takion::CryptoProvider {
 public:
  auto create_local(takion::CryptoMaterial)
      -> takion::Expected<std::unique_ptr<takion::CryptoDirection>,
                          takion::CryptoError> override {
    return std::unique_ptr<takion::CryptoDirection>{new MicrophoneLocal};
  }

  auto create_remote(takion::CryptoMaterial)
      -> takion::Expected<std::unique_ptr<takion::CryptoDirection>,
                          takion::CryptoError> override {
    return std::unique_ptr<takion::CryptoDirection>{new MicrophoneRemote};
  }
};

class TakionMicrophone : public ::testing::Test {
 protected:
  struct ReferenceCase final {
    std::uint32_t version;
    bool ps5;
    std::uint8_t wrapper;
    std::array<std::string_view, 2> datagrams;
  };

  static constexpr std::array<ReferenceCase, 3> reference_cases{{
      {9,
       false,
       0,
       {"030000000100022821054bee9e5d0000123400125589cd094df03470b4e8237f"
        "83c70b578ace024ef23579b5e92d609ce024688bcf0347f3367abec206498de9"
        "2d61a4f03478bc0f4387db074a8ef2ce824519d5995de0ac6024f88b4f33f7a3"
        "663afe9256e9ad692df1b57034c88c7823e7ab77cb8e420ed2965915e9ad613c"
        "e0a46814af6327f3b77a3e",
        "03000100020002282105aa3b6352000012c40005498dd11c5094e8347fa3e713"
        "57eb2e6aaef23671b5c90d39fca06430f44f03cf93571ad6aa6e22adf1357945"
        "f8bc703ce0ab6f1bdf935612d69a5ef9bd7135e1a4682cd89c570bf7bb7fc2be"
        "7236ea8d4105f9b5793ce0ac60249f4b0fd3974206fabe7a3de1a56125985c68"
        "acf03b47bbff337ea2e62a"}},
      {12,
       true,
       2,
       {"0000000203000000010002282105afbbd39f000012340000125589cd094df034"
        "70b4e8237f83c70b578ace024ef23579b5e92d609ce024688bcf0347f3367abe"
        "c206498de92d61a4f03478bc0f4387db074a8ef2ce824519d5995de0ac6024f8"
        "8b4f33f7a3663afe9256e9ad692df1b57034c88c7823e7ab77cb8e420ed29659"
        "15e9ad613ce0a46814af6327f3b77a3e",
        "0000000203000100020002282105616107d4000012c4000005498dd11c5094e8"
        "347fa3e71357eb2e6aaef23671b5c90d39fca06430f44f03cf93571ad6aa6e22"
        "adf1357945f8bc703ce0ab6f1bdf935612d69a5ef9bd7135e1a4682cd89c570b"
        "f7bb7fc2be7236ea8d4105f9b5793ce0ac60249f4b0fd3974206fabe7a3de1a5"
        "6125985c68acf03b47bbff337ea2e62a"}},
      {20,
       true,
       0,
       {"03000003e800000000000000010002200105287b54de000012340000125589cd"
        "094df03470b4e8237f83c70b578ace024ef23579b5e92d609ce024688bcf0347"
        "f3367abec206498de92d61a4f03478bc0f4387db074a8ef2ce824519d5995de0"
        "ac6024f88b4f33f7a3663afe9256e9ad692df1b57034c88c7823e7ab77cb8e42"
        "0ed2965915e9ad613ce0a46814af6327f3b77a3e",
        "03000003e900000001000100020002200105c3ada35d000012c4000005498dd1"
        "1c5094e8347fa3e71357eb2e6aaef23671b5c90d39fca06430f44f03cf93571a"
        "d6aa6e22adf1357945f8bc703ce0ab6f1bdf935612d69a5ef9bd7135e1a4682c"
        "d89c570bf7bb7fc2be7236ea8d4105f9b5793ce0ac60249f4b0fd3974206fabe"
        "7a3de1a56125985c68acf03b47bbff337ea2e62a"}},
  }};

  static auto bytes_of(std::string_view hex) -> std::vector<std::byte> {
    std::vector<std::byte> bytes;
    const auto nibble = [](char c) -> unsigned {
      return c <= '9' ? static_cast<unsigned>(c - '0')
                      : static_cast<unsigned>(c - 'a' + 10);
    };
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
      bytes.push_back(
          static_cast<std::byte>((nibble(hex[i]) << 4U) | nibble(hex[i + 1])));
    return bytes;
  }

  remote_play::MicrophoneSender sender;
  const std::array<std::byte, 40> frame{};
};

using TakionMicrophoneSender = TakionMicrophone;

TEST_F(TakionMicrophone, SendsTheReferenceDatagrams) {
  for (const auto& reference : reference_cases) {
    const auto profile =
        *takion::resolve_takion_wire_profile(reference.version);
    OutboundTransport transport;
    test_support::FakeClock clock{0};
    MicrophoneCryptoProvider provider;
    takion::CryptoContext crypto{takion::CryptoPolicy::required};
    EXPECT_TRUE(crypto.install_local(
        provider, takion::CryptoMaterial{takion::SecretBytes{}}))
        << "version " << reference.version;
    EXPECT_TRUE(crypto.set_local_key_position(0x1234))
        << "version " << reference.version;
    takion::ClientConfig config;
    config.crypto_policy = takion::CryptoPolicy::required;
    if (reference.wrapper)
      config.envelope = takion::PsnEnvelope{reference.wrapper};
    takion::client_detail::ClientOutbound outbound{&transport, clock, crypto,
                                                   config};
    outbound.set_profile(&profile);
    outbound.set_crypto_installed(true);
    remote_play::MicrophoneSender reference_sender;
    const bool unit_count_only = profile.features().contains(
        takion::Feature::av_unit_count_only_encoding);
    for (std::uint8_t step = 0; step < 2; ++step) {
      std::array<std::byte, 40> stepped{};
      for (std::size_t i = 0; i < stepped.size(); ++i)
        stepped[i] = static_cast<std::byte>(step * 40 + i);
      clock.now_us = 1000U + step;
      auto command =
          reference_sender.push(stepped, reference.ps5, unit_count_only);
      ASSERT_TRUE(command) << "version " << reference.version;
      EXPECT_TRUE(outbound.send_microphone(takion::SendMicrophone{
          std::move(command->packet), command->payload_offset}))
          << "version " << reference.version;
      ASSERT_EQ(transport.sent.size(), static_cast<std::size_t>(step + 1))
          << "version " << reference.version;
      EXPECT_EQ(transport.sent.back(), bytes_of(reference.datagrams[step]))
          << "version " << reference.version << " step " << +step;
    }
    EXPECT_EQ(crypto.local_key_position(), 0x1234U + 2U * 144U)
        << "version " << reference.version;
  }
}

TEST_F(TakionMicrophoneSender, RejectsFramesThatAreNotFortyBytes) {
  const std::array<std::byte, 41> long_frame{};
  EXPECT_FALSE(
      sender.push(std::span<const std::byte>{frame}.first(3), true, false));
  EXPECT_FALSE(sender.push(long_frame, true, false));
  auto first = sender.push(frame, true, false);
  EXPECT_FALSE(
      sender.push(std::span<const std::byte>{frame}.first(39), true, false));
  ASSERT_TRUE(first);
  EXPECT_EQ(first->packet.size(), 140U);
  EXPECT_EQ(first->payload_offset, 20U);
  EXPECT_EQ(first->packet[1], std::byte{0});
  EXPECT_EQ(first->packet[2], std::byte{0});
  auto second = sender.push(frame, false, true);
  ASSERT_TRUE(second);
  EXPECT_EQ(second->packet.size(), 139U);
  EXPECT_EQ(second->payload_offset, 19U);
  EXPECT_EQ(second->packet[2], std::byte{1});
  EXPECT_EQ(second->packet[4], std::byte{2});
  EXPECT_EQ(second->packet[7], std::byte{0x20});
  EXPECT_EQ(second->packet[8], std::byte{0x01});
}

TEST_F(TakionMicrophoneSender, WrapsTheFrameIndexLikeTheReference) {
  std::optional<takion::SendMicrophone> last;
  for (std::uint32_t i = 0; i < 65537; ++i)
    last = sender.push(frame, false, false);
  ASSERT_TRUE(last);
  EXPECT_EQ(last->packet[1], std::byte{0});
  EXPECT_EQ(last->packet[2], std::byte{0});
  EXPECT_EQ(last->packet[3], std::byte{0});
  EXPECT_EQ(last->packet[4], std::byte{1});
  auto previous = sender.push(frame, false, false);
  ASSERT_TRUE(previous);
  EXPECT_EQ(previous->packet[2], std::byte{1});
}

TEST_F(TakionMicrophone, IsBlockedBeforeCryptoIsInstalled) {
  const auto profile = *takion::resolve_takion_wire_profile(12);
  OutboundTransport transport;
  test_support::FakeClock clock{0};
  takion::CryptoContext crypto{takion::CryptoPolicy::required};
  takion::ClientConfig config;
  config.crypto_policy = takion::CryptoPolicy::required;
  takion::client_detail::ClientOutbound outbound{&transport, clock, crypto,
                                                 config};
  outbound.set_profile(&profile);
  auto command = sender.push(frame, true, false);
  ASSERT_TRUE(command);
  const auto sent = outbound.send_microphone(takion::SendMicrophone{
      std::move(command->packet), command->payload_offset});
  ASSERT_FALSE(sent);
  EXPECT_EQ(sent.error(), takion::ClientError::crypto_not_installed);
  EXPECT_TRUE(transport.sent.empty());
}

}  // namespace
}  // namespace takion_test
