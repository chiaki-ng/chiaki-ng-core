#include "src/core/net/kakusan/identity_proof.hpp"

#include "src/core/net/kakusan/digest.hpp"
#include "src/core/utils/bytes/byte_io.hpp"
#include "src/core/utils/memory/constant_time.hpp"

namespace libchiaking::net::kakusan {

auto IdentityProof::signed_bytes(Role sender, const Transcript& transcript,
                                 std::span<const std::byte> exporter,
                                 const std::optional<Hosting>& hosting)
    -> std::vector<std::byte> {
  std::vector<std::byte> input;
  bytes::Appender writer{input};
  writer.text(label);
  writer.u8(0);
  writer.u8(sender == Role::host ? host_role : guest_role);
  writer.bytes(transcript);
  writer.bytes(exporter);
  writer.text(key_policy);
  if (hosting) {
    writer.bytes(hosting->session_id);
    writer.u8(hosting->local_seats);
    writer.u8(hosting->remote_seats);
    writer.u8(hosting->reserved_seats);
  }
  return input;
}

auto IdentityProof::sign(platform::CryptoProvider& crypto, Role sender,
                         const Transcript& transcript,
                         std::span<const std::byte> exporter,
                         std::span<const std::byte> identity_seed,
                         std::optional<Hosting> hosting)
    -> Result<control::Identity> {
  if (identity_seed.size() != platform::ed25519_seed_size ||
      hosting.has_value() != (sender == Role::host))
    return fail(Error::internal);
  control::Identity identity;
  identity.hosting = hosting;
  if (const auto derived =
          crypto.ed25519_public_key(identity_seed, identity.key);
      !derived)
    return fail(Hash::failure(derived.error()));
  const auto input = signed_bytes(sender, transcript, exporter, hosting);
  const auto proven =
      crypto.ed25519_sign(identity_seed, input, identity.signature);
  if (!proven) return fail(Hash::failure(proven.error()));
  return identity;
}

auto IdentityProof::verify(platform::CryptoProvider& crypto, Role sender,
                           const Transcript& transcript,
                           std::span<const std::byte> exporter,
                           const control::Identity& identity,
                           const IdentityHint& promised) -> Result<void> {
  if (identity.hosting.has_value() != (sender == Role::host))
    return fail(Error::protocol);
  if (!memory::constant_time_equal(ShortBlockCodec::hint_of(identity.key),
                                   promised))
    return fail(Error::identity_changed);
  const auto input =
      signed_bytes(sender, transcript, exporter, identity.hosting);
  const auto verified =
      crypto.ed25519_verify(identity.key, input, identity.signature);
  if (!verified) return fail(Hash::failure(verified.error()));
  if (!verified.value()) return fail(Error::identity_changed);
  return {};
}

}  // namespace libchiaking::net::kakusan
