#include "ecdh_wire.hpp"

#include "src/core/utils/memory/constant_time.hpp"

#include <cstdint>
#include <utility>

namespace libchiaking::net::protocol::remote_play {

auto EcdhWireCodec::encode_public_key(platform::CryptoProvider& crypto,
                                      const platform::EcdhKeyPair& key_pair,
                                      platform::EcdhCurve curve,
                                      std::span<const std::byte> handshake_key)
    -> Expected<SignedEcdhPublicKey, EcdhWireError> {
  const auto coordinates_size = platform::ecdh_public_coordinates_size(curve);
  if (coordinates_size == 0 || coordinates_size + 1 > ecdh_public_wire_max_size)
    return unexpected(EcdhWireError::invalid_curve);
  if (handshake_key.size() != ecdh_handshake_key_size)
    return unexpected(EcdhWireError::invalid_handshake_key);

  SignedEcdhPublicKey result;
  result.size = coordinates_size + 1;
  result.bytes[0] = ec_point_uncompressed_tag;
  const auto coordinates =
      key_pair.public_coordinates({result.bytes.data() + 1, coordinates_size});
  if (!coordinates) return unexpected(EcdhWireError::coordinates_failure);
  const auto signature =
      crypto.hmac_sha256(handshake_key, result.view(), result.signature);
  if (!signature) return unexpected(EcdhWireError::signature_failure);
  return result;
}

auto EcdhWireCodec::decode_shared_secret(
    platform::CryptoProvider& crypto, const platform::EcdhKeyPair& key_pair,
    platform::EcdhCurve curve, std::span<const std::byte> handshake_key,
    std::span<const std::byte> remote_public_key,
    std::span<const std::byte> remote_signature)
    -> Expected<EcdhSharedSecret, EcdhWireError> {
  const auto coordinates_size = platform::ecdh_public_coordinates_size(curve);
  const auto secret_size = platform::ecdh_shared_secret_size(curve);
  if (coordinates_size == 0 || secret_size == 0)
    return unexpected(EcdhWireError::invalid_curve);
  if (handshake_key.size() != ecdh_handshake_key_size)
    return unexpected(EcdhWireError::invalid_handshake_key);
  if (remote_public_key.size() != coordinates_size + 1 ||
      remote_public_key[0] != ec_point_uncompressed_tag)
    return unexpected(EcdhWireError::invalid_wire_public_key);
  if (remote_signature.size() != ecdh_signature_size)
    return unexpected(EcdhWireError::invalid_remote_signature);

  SecretArray<ecdh_signature_size> expected_signature;
  if (!crypto.hmac_sha256(
          handshake_key, remote_public_key,
          {expected_signature.data(), expected_signature.size()}))
    return unexpected(EcdhWireError::signature_failure);
  if (!memory::constant_time_equal(
          {expected_signature.data(), expected_signature.size()},
          remote_signature))
    return unexpected(EcdhWireError::signature_mismatch);

  EcdhSharedSecret result;
  result.size = secret_size;
  if (!key_pair.derive_secret(remote_public_key.subspan(1),
                              {result.secret.data(), result.size}))
    return unexpected(EcdhWireError::derive_failure);
  return result;
}

auto EcdhWireCodec::prepare(platform::CryptoProvider& crypto,
                            platform::EcdhCurve curve,
                            std::span<const std::byte> handshake_key)
    -> Expected<PreparedEcdh, EcdhWireError> {
  if (platform::ecdh_public_coordinates_size(curve) == 0)
    return unexpected(EcdhWireError::invalid_curve);
  if (handshake_key.size() != ecdh_handshake_key_size)
    return unexpected(EcdhWireError::invalid_handshake_key);

  auto key_pair = crypto.generate_ecdh(curve);
  if (!key_pair || !key_pair.value())
    return unexpected(EcdhWireError::generation_failure);
  auto signed_key =
      encode_public_key(crypto, *key_pair.value(), curve, handshake_key);
  if (!signed_key) return unexpected(signed_key.error());
  return PreparedEcdh{curve, std::move(key_pair.value()), signed_key.value()};
}

}  // namespace libchiaking::net::protocol::remote_play
