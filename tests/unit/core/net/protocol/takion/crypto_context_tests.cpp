#include "src/core/net/protocol/takion/detail/crypto_context.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;

struct CryptoContextFakeState final {
  bool fail_encrypt{};
  bool fail_decrypt{};
  bool fail_authenticate{};
  std::vector<std::byte> last_authenticated;
  std::uint64_t last_position{};
  std::uint64_t last_encrypt_position{};
  std::uint64_t last_authenticate_position{};
  std::uint64_t last_decrypt_position{};
  std::size_t decrypt_calls{};
};

class CryptoContextFakeMac final {
 public:
  static auto of(std::uint64_t position, std::span<const std::byte> bytes)
      -> takion::Mac {
    std::uint32_t value = static_cast<std::uint32_t>(position) ^
                          static_cast<std::uint32_t>(position >> 32) ^
                          0x9e3779b9u;
    for (const auto byte : bytes)
      value = (value << 5) ^ (value >> 2) ^ std::to_integer<std::uint8_t>(byte);
    return {
        static_cast<std::byte>(value >> 24),
        static_cast<std::byte>(value >> 16),
        static_cast<std::byte>(value >> 8),
        static_cast<std::byte>(value),
    };
  }
};

class CryptoContextFakeLocal final : public takion::CryptoDirection {
 public:
  explicit CryptoContextFakeLocal(std::shared_ptr<CryptoContextFakeState> state)
      : state_(std::move(state)) {}

  auto apply_keystream(std::uint64_t position, std::span<const std::byte> input,
                       std::span<std::byte> bytes)
      -> takion::Expected<void, takion::CryptoError> override {
    if (input.data() != bytes.data()) std::ranges::copy(input, bytes.begin());
    state_->last_position = position;
    state_->last_encrypt_position = position;
    if (state_->fail_encrypt)
      return takion::unexpected(takion::CryptoError::provider_failure);
    for (auto& byte : bytes) byte ^= std::byte{0x5a};
    return {};
  }

  auto authenticate(std::uint64_t position, std::span<const std::byte> bytes)
      -> takion::Expected<takion::Mac, takion::CryptoError> override {
    if (state_->fail_authenticate)
      return takion::unexpected(takion::CryptoError::provider_failure);
    state_->last_position = position;
    state_->last_authenticate_position = position;
    state_->last_authenticated.assign(bytes.begin(), bytes.end());
    return CryptoContextFakeMac::of(position, bytes);
  }

 private:
  std::shared_ptr<CryptoContextFakeState> state_;
};

class CryptoContextFakeRemote final : public takion::CryptoDirection {
 public:
  explicit CryptoContextFakeRemote(
      std::shared_ptr<CryptoContextFakeState> state)
      : state_(std::move(state)) {}

  auto authenticate(std::uint64_t position, std::span<const std::byte> bytes)
      -> takion::Expected<takion::Mac, takion::CryptoError> override {
    if (state_->fail_authenticate)
      return takion::unexpected(takion::CryptoError::provider_failure);
    state_->last_position = position;
    state_->last_authenticate_position = position;
    state_->last_authenticated.assign(bytes.begin(), bytes.end());
    return CryptoContextFakeMac::of(position, bytes);
  }

  auto apply_keystream(std::uint64_t position, std::span<const std::byte> input,
                       std::span<std::byte> bytes)
      -> takion::Expected<void, takion::CryptoError> override {
    if (input.data() != bytes.data()) std::ranges::copy(input, bytes.begin());
    state_->last_position = position;
    state_->last_decrypt_position = position;
    ++state_->decrypt_calls;
    if (state_->fail_decrypt)
      return takion::unexpected(takion::CryptoError::provider_failure);
    for (auto& byte : bytes) byte ^= std::byte{0x5a};
    return {};
  }

 private:
  std::shared_ptr<CryptoContextFakeState> state_;
};

class CryptoContextFakeProvider final : public takion::CryptoProvider {
 public:
  CryptoContextFakeProvider(std::shared_ptr<CryptoContextFakeState> local,
                            std::shared_ptr<CryptoContextFakeState> remote)
      : local_(std::move(local)), remote_(std::move(remote)) {}

  auto create_local(takion::CryptoMaterial material)
      -> takion::Expected<std::unique_ptr<takion::CryptoDirection>,
                          takion::CryptoError> override {
    EXPECT_FALSE(material.ecdh_secret().empty());
    return std::unique_ptr<takion::CryptoDirection>{
        new CryptoContextFakeLocal{local_}};
  }

  auto create_remote(takion::CryptoMaterial material)
      -> takion::Expected<std::unique_ptr<takion::CryptoDirection>,
                          takion::CryptoError> override {
    EXPECT_FALSE(material.ecdh_secret().empty());
    return std::unique_ptr<takion::CryptoDirection>{
        new CryptoContextFakeRemote{remote_}};
  }

 private:
  std::shared_ptr<CryptoContextFakeState> local_;
  std::shared_ptr<CryptoContextFakeState> remote_;
};

class TakionCryptoContextTest : public ::testing::Test {
 protected:
  static auto secret() -> takion::SecretBytes {
    const std::array values{std::byte{1}, std::byte{2}, std::byte{3},
                            std::byte{4}};
    return takion::SecretBytes{values};
  }

  static auto profile(std::uint32_t version) -> const takion::WireProfile& {
    static const auto version_7 =
        takion::resolve_takion_wire_profile(7).value();
    static const auto version_12 =
        takion::resolve_takion_wire_profile(12).value();
    static const auto version_20 =
        takion::resolve_takion_wire_profile(20).value();
    return version == 7 ? version_7 : version == 20 ? version_20 : version_12;
  }

  static auto packet(std::size_t envelope_size = 0) -> std::vector<std::byte> {
    std::vector<std::byte> result(envelope_size + 32);
    for (std::size_t index = 0; index < result.size(); ++index)
      result[index] = static_cast<std::byte>(index + 1);
    result[envelope_size] = std::byte{0};
    return result;
  }

  static auto coverage(std::size_t envelope_size = 0)
      -> takion::PacketCryptoCoverage {
    return {envelope_size, 32, envelope_size + 20, 8};
  }

  static auto feedback_packet(std::size_t header_size)
      -> std::vector<std::byte> {
    std::vector<std::byte> result(header_size + 5);
    for (std::size_t index = 0; index < result.size(); ++index)
      result[index] = static_cast<std::byte>(index + 1);
    result[0] = std::byte{0x06};
    return result;
  }

  static auto byte_values(std::size_t size, std::uint8_t seed)
      -> std::vector<std::byte> {
    std::vector<std::byte> values(size);
    for (std::size_t index = 0; index < values.size(); ++index)
      values[index] = static_cast<std::byte>(seed + index);
    return values;
  }

  static auto bytes_with_size(std::size_t size, std::uint8_t seed)
      -> takion::SecretBytes {
    return takion::SecretBytes{byte_values(size, seed)};
  }

  static auto typed_material_matches(const takion::CryptoMaterial& material,
                                     std::span<const std::byte> handshake_key,
                                     std::span<const std::byte> ecdh_secret,
                                     std::uint8_t gkcrypt_index)
      -> testing::AssertionResult {
    if (!material.has_gkcrypt_material())
      return testing::AssertionFailure() << "no gkcrypt material";
    if (!std::ranges::equal(material.handshake_key(), handshake_key))
      return testing::AssertionFailure() << "the handshake key differs";
    if (!std::ranges::equal(material.ecdh_secret(), ecdh_secret))
      return testing::AssertionFailure() << "the ECDH secret differs";
    if (material.gkcrypt_index() != gkcrypt_index)
      return testing::AssertionFailure() << "the gkcrypt index differs";
    return testing::AssertionSuccess();
  }

  void install_both(takion::CryptoContext& target) {
    ASSERT_TRUE(
        target.install_local(provider, takion::CryptoMaterial{secret()}));
    ASSERT_TRUE(
        target.install_remote(provider, takion::CryptoMaterial{secret()}));
  }

  auto protected_packet(std::uint64_t position, std::size_t envelope_size = 0)
      -> std::vector<std::byte> {
    auto sender_state = std::make_shared<CryptoContextFakeState>();
    auto unused_remote = std::make_shared<CryptoContextFakeState>();
    CryptoContextFakeProvider sender_provider{sender_state, unused_remote};
    takion::CryptoContext sender{takion::CryptoPolicy::required};
    EXPECT_TRUE(sender.install_local(sender_provider,
                                     takion::CryptoMaterial{secret()}));
    EXPECT_TRUE(sender.set_local_key_position(position));
    auto wire = packet(envelope_size);
    auto reservation =
        sender.protect_local(wire, takion::PacketFamily::control, profile(12),
                             coverage(envelope_size), 8);
    EXPECT_TRUE(reservation);
    if (reservation) {
      EXPECT_TRUE(reservation.value().commit());
    }
    return wire;
  }

  std::shared_ptr<CryptoContextFakeState> local =
      std::make_shared<CryptoContextFakeState>();
  std::shared_ptr<CryptoContextFakeState> remote =
      std::make_shared<CryptoContextFakeState>();
  CryptoContextFakeProvider provider{local, remote};
  takion::CryptoContext context{takion::CryptoPolicy::required};
};

TEST_F(TakionCryptoContextTest, LeavesPacketsUntouchedWhenDisabled) {
  takion::CryptoContext disabled{takion::CryptoPolicy::disabled};
  auto wire = packet();
  const auto original = wire;
  auto reservation =
      disabled.protect_local(wire, takion::PacketFamily::feedback_history,
                             profile(12), coverage(), 19);
  ASSERT_TRUE(reservation);
  EXPECT_TRUE(reservation.value().commit());
  EXPECT_EQ(wire, original);
  EXPECT_EQ(disabled.local_key_position(), 0U);
  const auto received = disabled.authenticate_and_decrypt_remote(
      wire, takion::PacketFamily::feedback_history, profile(12), coverage());
  EXPECT_TRUE(received);
  EXPECT_EQ(wire, original);
}

TEST_F(TakionCryptoContextTest, PlacesTheMacAndKeyPositionAtEachFamilysOffset) {
  const std::array families{
      takion::PacketFamily::control,
      takion::PacketFamily::feedback_history,
      takion::PacketFamily::video,
      takion::PacketFamily::audio_and_haptics,
      takion::PacketFamily::length_prefixed,
      takion::PacketFamily::congestion,
      takion::PacketFamily::feedback_state,
      takion::PacketFamily::client_information,
      takion::PacketFamily::extended_message,
  };
  for (const auto& registration : takion::takion_version_registry) {
    const auto version = registration.version;
    const auto resolved_profile = takion::resolve_takion_wire_profile(version);
    ASSERT_TRUE(resolved_profile) << "version " << version;
    const auto& wire_profile = resolved_profile.value();
    for (const auto family : families) {
      const auto result =
          takion::CryptoContext::packet_offsets(family, wire_profile);
      if (family == takion::PacketFamily::length_prefixed ||
          family == takion::PacketFamily::client_information) {
        EXPECT_FALSE(result) << "version " << version;
        continue;
      }
      ASSERT_TRUE(result) << "version " << version;
      std::size_t mac{};
      std::size_t key{};
      bool zero_key{};
      std::uint64_t cipher_addition{};
      switch (family) {
        case takion::PacketFamily::control:
          mac = 5;
          key = 9;
          zero_key = true;
          break;
        case takion::PacketFamily::video:
        case takion::PacketFamily::audio_and_haptics:
          mac = 10;
          key = 14;
          cipher_addition = 16;
          zero_key = false;
          break;
        case takion::PacketFamily::congestion:
          mac = 7;
          key = 11;
          zero_key = true;
          break;
        case takion::PacketFamily::extended_message:
          mac = 1;
          key = 5;
          zero_key = false;
          cipher_addition = 16;
          break;
        case takion::PacketFamily::feedback_history:
        case takion::PacketFamily::feedback_state:
          mac = 8;
          key = 4;
          zero_key = false;
          cipher_addition = 16;
          break;
        default:
          FAIL() << "unexpected family";
      }
      const auto type = takion::PacketType{static_cast<std::uint8_t>(family)};
      const std::size_t shift = wire_profile.uses_extended_header(type)
                                    ? std::size_t{8}
                                    : std::size_t{0};
      EXPECT_EQ(result.value().mac, mac + shift) << "version " << version;
      EXPECT_EQ(result.value().key_position_low, key + shift)
          << "version " << version;
      EXPECT_EQ(result.value().zero_key_position_for_authentication, zero_key)
          << "version " << version;
      EXPECT_EQ(result.value().cipher_key_position_addition, cipher_addition)
          << "version " << version;
    }
  }
}

TEST_F(TakionCryptoContextTest,
       ProtectsFeedbackAtItsKeyPositionAndRollsBackOnRequest) {
  ASSERT_NO_FATAL_FAILURE(install_both(context));
  ASSERT_TRUE(context.set_local_key_position(100));

  auto wire = feedback_packet(12);
  const auto original = wire;
  auto reservation =
      context.protect_local(wire, takion::PacketFamily::feedback_state,
                            profile(7), {0, 17, 12, 5}, 21);
  ASSERT_TRUE(reservation);
  EXPECT_EQ(local->last_encrypt_position, 116U);
  EXPECT_EQ(local->last_authenticate_position, 100U);
  EXPECT_EQ(wire[4], std::byte{0});
  EXPECT_EQ(wire[5], std::byte{0});
  EXPECT_EQ(wire[6], std::byte{0});
  EXPECT_EQ(wire[7], std::byte{100});
  for (std::size_t index = 0; index < 4; ++index)
    EXPECT_EQ(wire[index], original[index]) << "index " << index;
  for (std::size_t index = 12; index < 17; ++index)
    EXPECT_EQ(wire[index], original[index] ^ std::byte{0x5a})
        << "index " << index;
  reservation.value().rollback();
  EXPECT_EQ(context.local_key_position(), 100U);

  auto committed_wire = feedback_packet(12);
  auto committed = context.protect_local(committed_wire,
                                         takion::PacketFamily::feedback_state,
                                         profile(7), {0, 17, 12, 5}, 21);
  ASSERT_TRUE(committed);
  EXPECT_TRUE(committed.value().commit());
  EXPECT_EQ(context.local_key_position(), 126U);

  ASSERT_TRUE(context.set_local_key_position(100));
  auto v20_wire = feedback_packet(20);
  auto v20 =
      context.protect_local(v20_wire, takion::PacketFamily::feedback_state,
                            profile(20), {0, 25, 20, 5}, 21);
  ASSERT_TRUE(v20);
  EXPECT_EQ(local->last_encrypt_position, 116U);
  EXPECT_EQ(local->last_authenticate_position, 100U);
  EXPECT_EQ(v20_wire[12], std::byte{0});
  EXPECT_EQ(v20_wire[13], std::byte{0});
  EXPECT_EQ(v20_wire[14], std::byte{0});
  EXPECT_EQ(v20_wire[15], std::byte{100});
  v20.value().rollback();
}

TEST_F(TakionCryptoContextTest,
       AuthenticatesFeedbackBeforeDecryptingAndRefusesAnOverflowingPosition) {
  auto sender_state = std::make_shared<CryptoContextFakeState>();
  auto unused_remote = std::make_shared<CryptoContextFakeState>();
  CryptoContextFakeProvider sender_provider{sender_state, unused_remote};
  takion::CryptoContext sender{takion::CryptoPolicy::required};
  ASSERT_TRUE(
      sender.install_local(sender_provider, takion::CryptoMaterial{secret()}));
  ASSERT_TRUE(sender.set_local_key_position(100));
  auto wire = feedback_packet(12);
  const auto plaintext = wire;
  auto sent = sender.protect_local(wire, takion::PacketFamily::feedback_state,
                                   profile(7), {0, 17, 12, 5}, 21);
  ASSERT_TRUE(sent);
  EXPECT_TRUE(sent.value().commit());
  const auto ciphertext = wire;

  auto unused_local = std::make_shared<CryptoContextFakeState>();
  auto receiver_state = std::make_shared<CryptoContextFakeState>();
  CryptoContextFakeProvider receiver_provider{unused_local, receiver_state};
  takion::CryptoContext receiver{takion::CryptoPolicy::required};
  ASSERT_TRUE(receiver.install_remote(receiver_provider,
                                      takion::CryptoMaterial{secret()}));
  receiver.set_remote_key_position(90);
  const auto received = receiver.authenticate_and_decrypt_remote(
      wire, takion::PacketFamily::feedback_state, profile(7), {0, 17, 12, 5});
  ASSERT_TRUE(received);
  EXPECT_EQ(received.value(), 100U);
  EXPECT_EQ(receiver_state->last_authenticate_position, 100U);
  EXPECT_EQ(receiver_state->last_decrypt_position, 116U);
  EXPECT_EQ(receiver.remote_key_position(), 100U);
  EXPECT_TRUE(
      std::equal(wire.begin() + 12, wire.end(), plaintext.begin() + 12));

  auto bad_wire = ciphertext;
  bad_wire[8] ^= std::byte{1};
  auto bad_remote = std::make_shared<CryptoContextFakeState>();
  CryptoContextFakeProvider bad_provider{unused_local, bad_remote};
  takion::CryptoContext bad_receiver{takion::CryptoPolicy::required};
  ASSERT_TRUE(bad_receiver.install_remote(bad_provider,
                                          takion::CryptoMaterial{secret()}));
  bad_receiver.set_remote_key_position(90);
  const auto bad = bad_receiver.authenticate_and_decrypt_remote(
      bad_wire, takion::PacketFamily::feedback_state, profile(7),
      {0, 17, 12, 5});
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error(), takion::CryptoError::authentication_failed);
  EXPECT_EQ(bad_remote->decrypt_calls, 0U);
  EXPECT_EQ(bad_receiver.remote_key_position(), 90U);
  EXPECT_TRUE(std::equal(bad_wire.begin() + 12, bad_wire.end(),
                         ciphertext.begin() + 12));

  takion::CryptoContext overflow{takion::CryptoPolicy::required};
  ASSERT_TRUE(overflow.install_local(sender_provider,
                                     takion::CryptoMaterial{secret()}));
  ASSERT_TRUE(overflow.set_local_key_position(UINT64_MAX - 8));
  auto overflow_wire = feedback_packet(12);
  const auto overflow_original = overflow_wire;
  const auto overflow_result = overflow.protect_local(
      overflow_wire, takion::PacketFamily::feedback_state, profile(7),
      {0, 17, 12, 5}, 0);
  ASSERT_FALSE(overflow_result);
  EXPECT_EQ(overflow_result.error(), takion::CryptoError::counter_overflow);
  EXPECT_EQ(overflow_wire, overflow_original);
  EXPECT_EQ(overflow.local_key_position(), UINT64_MAX - 8);
}

TEST_F(TakionCryptoContextTest,
       EncryptsAnExtendedMessageSixteenPastItsKeyPosition) {
  ASSERT_NO_FATAL_FAILURE(install_both(context));
  ASSERT_TRUE(context.set_local_key_position(90001));
  std::vector<std::byte> wire(13 + 4, std::byte{0x11});
  wire[0] = std::byte{0x0e};
  auto reservation =
      context.protect_local(wire, takion::PacketFamily::extended_message,
                            profile(12), {0, 17, 13, 4}, 41);
  ASSERT_TRUE(reservation);
  EXPECT_EQ(local->last_encrypt_position, 90001U + 16U);
  EXPECT_EQ(local->last_authenticate_position, 90001U);
  reservation.value().rollback();
}

TEST_F(TakionCryptoContextTest,
       EncryptsAControlDirectMessageSixteenPastItsKeyPosition) {
  ASSERT_NO_FATAL_FAILURE(install_both(context));
  ASSERT_TRUE(context.set_local_key_position(500));
  std::vector<std::byte> wire(24, std::byte{0x11});
  wire[0] = std::byte{0x00};
  auto reservation = context.protect_local(
      wire, takion::PacketFamily::control, profile(12),
      {0, 24, 20, 4, takion::takion_crypto_block_size}, 24);
  ASSERT_TRUE(reservation);
  EXPECT_EQ(local->last_encrypt_position, 500U + 16U);
  EXPECT_EQ(local->last_authenticate_position, 500U);
  EXPECT_EQ(wire[20], std::byte{0x11} ^ std::byte{0x5a});
  EXPECT_EQ(wire[19], std::byte{0x11});
  reservation.value().rollback();
}

TEST_F(TakionCryptoContextTest,
       AdvancesTheLocalKeyPositionOnlyWhenAReservationCommits) {
  ASSERT_NO_FATAL_FAILURE(install_both(context));
  ASSERT_TRUE(context.set_local_key_position(100));

  auto first_wire = packet();
  {
    auto first = context.protect_local(
        first_wire, takion::PacketFamily::control, profile(12), coverage(), 17);
    ASSERT_TRUE(first);
    EXPECT_EQ(first.value().key_position(), 100U);
    EXPECT_EQ(context.local_key_position(), 100U);
    auto blocked = context.protect_local(
        first_wire, takion::PacketFamily::control, profile(12), coverage(), 17);
    ASSERT_FALSE(blocked);
    EXPECT_EQ(blocked.error(), takion::CryptoError::reservation_in_progress);
  }
  EXPECT_EQ(context.local_key_position(), 100U);

  auto second_wire = packet();
  auto second = context.protect_local(
      second_wire, takion::PacketFamily::control, profile(12), coverage(), 17);
  ASSERT_TRUE(second);
  EXPECT_TRUE(second.value().commit());
  EXPECT_EQ(context.local_key_position(), 118U);
}

TEST_F(TakionCryptoContextTest, ABadMacNeitherCommitsNorExposesThePayload) {
  auto wire = protected_packet(77);
  wire[5] ^= std::byte{1};
  const auto ciphertext =
      std::vector<std::byte>{wire.begin() + 20, wire.begin() + 28};

  takion::CryptoContext receiver{takion::CryptoPolicy::required};
  ASSERT_TRUE(
      receiver.install_remote(provider, takion::CryptoMaterial{secret()}));
  receiver.set_remote_key_position(70);
  const auto result = receiver.authenticate_and_decrypt_remote(
      wire, takion::PacketFamily::control, profile(12), coverage());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), takion::CryptoError::authentication_failed);
  EXPECT_EQ(receiver.remote_key_position(), 70U);
  EXPECT_TRUE(
      std::equal(ciphertext.begin(), ciphertext.end(), wire.begin() + 20));
}

TEST_F(TakionCryptoContextTest,
       AFailedDecryptionNeitherCommitsNorExposesThePayload) {
  auto wire = protected_packet(77);
  const auto ciphertext =
      std::vector<std::byte>{wire.begin() + 20, wire.begin() + 28};

  remote->fail_decrypt = true;
  takion::CryptoContext receiver{takion::CryptoPolicy::required};
  ASSERT_TRUE(
      receiver.install_remote(provider, takion::CryptoMaterial{secret()}));
  receiver.set_remote_key_position(70);
  const auto result = receiver.authenticate_and_decrypt_remote(
      wire, takion::PacketFamily::control, profile(12), coverage());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), takion::CryptoError::provider_failure);
  EXPECT_EQ(receiver.remote_key_position(), 70U);
  EXPECT_TRUE(
      std::equal(ciphertext.begin(), ciphertext.end(), wire.begin() + 20));
}

TEST_F(TakionCryptoContextTest,
       ReconstructsARemoteKeyPositionWhoseLow32BitsWrapped) {
  constexpr std::uint64_t position = 0x0000000100000005ull;
  auto wire = protected_packet(position);
  takion::CryptoContext receiver{takion::CryptoPolicy::required};
  ASSERT_TRUE(
      receiver.install_remote(provider, takion::CryptoMaterial{secret()}));
  receiver.set_remote_key_position(0x00000000fffffff0ull);
  const auto result = receiver.authenticate_and_decrypt_remote(
      wire, takion::PacketFamily::control, profile(12), coverage());
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value(), position);
  EXPECT_EQ(receiver.remote_key_position(), position);
}

TEST_F(TakionCryptoContextTest, RefusesKeyPositionsThatWouldOverflow) {
  ASSERT_TRUE(
      context.install_local(provider, takion::CryptoMaterial{secret()}));
  ASSERT_TRUE(context.set_local_key_position(UINT64_MAX - 1));
  auto wire = packet();
  const auto result = context.protect_local(wire, takion::PacketFamily::control,
                                            profile(12), coverage(), 1);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), takion::CryptoError::counter_overflow);
  EXPECT_EQ(context.local_key_position(), UINT64_MAX - 1);

  takion::CryptoContext receiver{takion::CryptoPolicy::required};
  ASSERT_TRUE(
      receiver.install_remote(provider, takion::CryptoMaterial{secret()}));
  receiver.set_remote_key_position(0xfffffffffffffff0ull);
  auto received_wire = packet();
  received_wire[9] = std::byte{0};
  received_wire[10] = std::byte{0};
  received_wire[11] = std::byte{0};
  received_wire[12] = std::byte{5};
  const auto received = receiver.authenticate_and_decrypt_remote(
      received_wire, takion::PacketFamily::control, profile(12), coverage());
  ASSERT_FALSE(received);
  EXPECT_EQ(received.error(), takion::CryptoError::counter_overflow);
  EXPECT_EQ(receiver.remote_key_position(), 0xfffffffffffffff0ull);
}

TEST_F(TakionCryptoContextTest, AFailedLocalProtectLeavesThePacketUntouched) {
  local->fail_authenticate = true;
  ASSERT_TRUE(
      context.install_local(provider, takion::CryptoMaterial{secret()}));
  auto wire = packet();
  const auto original = wire;
  const auto result = context.protect_local(wire, takion::PacketFamily::control,
                                            profile(12), coverage(), 17);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), takion::CryptoError::provider_failure);
  EXPECT_EQ(wire, original);
  EXPECT_EQ(context.local_key_position(), 0U);
}

TEST_F(TakionCryptoContextTest, AuthenticatesOnlyThePacketAfterItsEnvelope) {
  ASSERT_TRUE(
      context.install_local(provider, takion::CryptoMaterial{secret()}));
  auto wire = packet(4);
  const auto envelope = std::vector<std::byte>{wire.begin(), wire.begin() + 4};
  auto reservation = context.protect_local(wire, takion::PacketFamily::control,
                                           profile(12), coverage(4), 8);
  ASSERT_TRUE(reservation);
  ASSERT_EQ(local->last_authenticated.size(), 32U);
  EXPECT_EQ(local->last_authenticated.front(), std::byte{0});
  EXPECT_TRUE(std::equal(envelope.begin(), envelope.end(), wire.begin()));
}

TEST_F(TakionCryptoContextTest, TypedCryptoMaterialKeepsItsKeysAndIndex) {
  for (const std::size_t ecdh_size : {std::size_t{32}, std::size_t{66}}) {
    takion::CryptoMaterial local_material{bytes_with_size(16, 0x21),
                                          bytes_with_size(ecdh_size, 0x43),
                                          std::uint8_t{0x5a}};
    EXPECT_TRUE(typed_material_matches(local_material, byte_values(16, 0x21),
                                       byte_values(ecdh_size, 0x43),
                                       std::uint8_t{0x5a}))
        << "ECDH size " << ecdh_size;

    takion::CryptoMaterial remote_material{bytes_with_size(16, 0x67),
                                           bytes_with_size(ecdh_size, 0x89),
                                           std::uint8_t{0xa5}};
    EXPECT_TRUE(typed_material_matches(remote_material, byte_values(16, 0x67),
                                       byte_values(ecdh_size, 0x89),
                                       std::uint8_t{0xa5}))
        << "ECDH size " << ecdh_size;
  }

  takion::CryptoMaterial invalid_local{
      bytes_with_size(15, 0x01), bytes_with_size(31, 0x02), std::uint8_t{0}};
  EXPECT_TRUE(invalid_local.has_gkcrypt_material());
  EXPECT_EQ(invalid_local.handshake_key().size(), 15U);
  EXPECT_EQ(invalid_local.ecdh_secret().size(), 31U);

  takion::CryptoMaterial invalid_remote{
      bytes_with_size(15, 0x03), bytes_with_size(31, 0x04), std::uint8_t{1}};
  EXPECT_TRUE(invalid_remote.has_gkcrypt_material());
  EXPECT_EQ(invalid_remote.handshake_key().size(), 15U);
  EXPECT_EQ(invalid_remote.ecdh_secret().size(), 31U);
}

TEST_F(TakionCryptoContextTest, LegacyCryptoMaterialCarriesOnlyTheSecret) {
  auto legacy_secret = secret();
  const auto expected = legacy_secret.bytes();
  takion::CryptoMaterial material{std::move(legacy_secret)};
  EXPECT_FALSE(material.has_gkcrypt_material());
  EXPECT_TRUE(std::ranges::equal(material.ecdh_secret(), expected));
  EXPECT_FALSE(material.gkcrypt_index());
}

TEST_F(TakionCryptoContextTest, SecretsAreMoveOnlyAndClearable) {
  static_assert(!std::is_copy_constructible_v<takion::SecretBytes>);
  static_assert(std::is_nothrow_move_constructible_v<takion::SecretBytes>);
  static_assert(!std::is_copy_constructible_v<takion::CryptoMaterial>);
  static_assert(!std::is_copy_assignable_v<takion::CryptoMaterial>);
  static_assert(std::is_nothrow_move_constructible_v<takion::CryptoMaterial>);
  static_assert(std::is_nothrow_move_assignable_v<takion::CryptoMaterial>);
  auto first = secret();
  auto second = std::move(first);
  EXPECT_TRUE(first.empty());
  EXPECT_EQ(second.bytes().size(), 4U);
  second.clear();
  EXPECT_TRUE(second.empty());
}

}  // namespace
}  // namespace takion_test
