#include "local_prepare.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"

#include "src/core/net/protocol/remote_play/pairing/rp_version.hpp"
#include "src/core/net/protocol/remote_play/stream/limits.hpp"
#include "src/core/net/protocol/takion/platform_crypto_provider.hpp"
#include "src/core/net/protocol/takion/versions.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace libchiaking::net::protocol::remote_play {

auto LocalStreamPreparation::prepare(const LocalStreamInputs& inputs)
    -> Expected<PreparedLocalStream, LocalPrepareError> {
  const auto& calibration = inputs.calibration;
  const auto profile =
      takion::resolve_takion_wire_profile(calibration.takion_version);
  if (!profile || calibration.wire_profile.version() != profile->version() ||
      calibration.wire_profile.ecdh_curve() != profile->ecdh_curve())
    return unexpected(LocalPrepareError::invalid_calibration);
  if (inputs.session_id.view().empty())
    return unexpected(LocalPrepareError::invalid_session_id);
  if (calibration.inbound_mtu == 0 || calibration.outbound_mtu == 0)
    return unexpected(LocalPrepareError::invalid_mtu);
  if (calibration.round_trip_time.adjusted_mean < std::chrono::microseconds{0})
    return unexpected(LocalPrepareError::invalid_rtt);
  const auto curve = profile->ecdh_curve();
  if (platform::ecdh_shared_secret_size(curve) == 0)
    return unexpected(LocalPrepareError::unsupported_curve);
  if (inputs.nonce.size() != rp_nonce_size)
    return unexpected(LocalPrepareError::invalid_nonce);
  auto material = AuthMaterial::derive(
      inputs.console.target, inputs.nonce,
      {inputs.console.key.data(), inputs.console.key.size()});
  if (!material) return unexpected(LocalPrepareError::auth_material_failure);

  SecretArray<ecdh_handshake_key_size> handshake_key;
  if (!inputs.random.fill_bytes({handshake_key.data(), handshake_key.size()}))
    return unexpected(LocalPrepareError::random_failure);
  const auto key =
      std::span<const std::byte>{handshake_key.data(), handshake_key.size()};

  auto prepared_ecdh = EcdhWireCodec::prepare(inputs.crypto, curve, key);
  if (!prepared_ecdh) return unexpected(LocalPrepareError::ecdh_failure);

  auto launch_spec = LaunchSpecCodec::encode(
      inputs.crypto, inputs.console.target, inputs.codec, inputs.video,
      calibration.inbound_mtu,
      static_cast<std::uint64_t>(
          calibration.round_trip_time.adjusted_mean.count()),
      key, material.value(), inputs.requested_audio_channels);
  if (!launch_spec) return unexpected(LocalPrepareError::launch_spec_failure);

  return PreparedLocalStream{
      std::move(handshake_key), std::move(prepared_ecdh.value()),
      std::make_shared<const SecretString>(inputs.session_id.view()),
      std::make_shared<const SecretString>(std::move(launch_spec).value())};
}

auto LocalStreamPreparation::console_active_profile(
    RegistrationTarget target, std::uint32_t takion_version_override)
    -> std::optional<takion::WireProfile> {
  const auto rp_version = RpVersion::of(target);
  if (!rp_version) return std::nullopt;
  const auto version = rp_version->ps5() ? takion::takion_version_ps5
                                         : takion::takion_version_ps4;
  return takion::resolve_takion_wire_profile(
      takion::select_takion_version(version, takion_version_override));
}

auto LocalStreamPreparation::make_reference_fallback_calibration(
    RegistrationTarget target, std::uint32_t takion_version_override)
    -> std::optional<senkusha::Result> {
  const auto profile = console_active_profile(target, takion_version_override);
  if (!profile) return std::nullopt;
  senkusha::RttStatistics rtt{};
  rtt.adjusted_mean = std::chrono::microseconds{fallback_rtt_us};
  return senkusha::Result{profile->version(), rtt, senkusha::mtu_upper_bound,
                          senkusha::mtu_upper_bound, *profile};
}

auto LocalStreamPreparation::resolve_local_did(
    const std::array<std::byte, rp_did_size>& supplied,
    platform::SecureRandomProvider& random)
    -> std::optional<std::array<std::byte, rp_did_size>> {
  if (std::ranges::any_of(
          supplied, [](std::byte value) { return value != std::byte{0}; }))
    return supplied;
  constexpr std::array<std::uint8_t, 10> prefix{0x00, 0x18, 0x00, 0x00, 0x00,
                                                0x07, 0x00, 0x40, 0x00, 0x80};
  constexpr std::size_t random_size = 16;
  std::array<std::byte, rp_did_size> did{};
  std::ranges::transform(prefix, did.begin(),
                         [](std::uint8_t value) { return std::byte{value}; });
  if (!random.fill_bytes(
          std::span<std::byte>{did}.subspan(prefix.size(), random_size)))
    return std::nullopt;
  return did;
}

}  // namespace libchiaking::net::protocol::remote_play
