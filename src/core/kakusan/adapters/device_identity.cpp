#include "libchiaking/detail/kakusan/device_identity.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"

#include "src/core/kakusan/adapters/protocol_refusal.hpp"
#include "src/core/net/kakusan/digest.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::kakusan {

auto DeviceIdentity::create(platform::CryptoProvider& crypto,
                            platform::SecureRandomProvider& random)
    -> Expected<DeviceIdentity, Refusal> {
  SavedIdentity fresh;
  if (!random.fill_bytes(
          std::span<std::byte>{fresh.seed.data(), fresh.seed.size()}))
    return unexpected(Refusal{Refusal::Kind::internal});
  return restore(crypto, fresh);
}

auto DeviceIdentity::restore(platform::CryptoProvider& crypto,
                             const SavedIdentity& saved)
    -> Expected<DeviceIdentity, Refusal> {
  IdentityKey::Bytes key{};
  const auto derived = crypto.ed25519_public_key(
      std::span<const std::byte>{saved.seed.data(), saved.seed.size()}, key);
  if (!derived)
    return unexpected(ProtocolRefusal::of(net::kakusan::Hash::failure(derived.error())));
  const auto print = fingerprint_of(crypto, IdentityKey{key});
  if (!print) return unexpected(print.error());
  return DeviceIdentity{saved.seed.clone(), IdentityKey{key}, print.value()};
}

auto DeviceIdentity::fingerprint_of(platform::CryptoProvider& crypto,
                                    const IdentityKey& key)
    -> Expected<Fingerprint, Refusal> {
  const auto digest = net::kakusan::Hash::sha256(crypto, key.bytes());
  if (!digest) return unexpected(ProtocolRefusal::of(digest.error()));
  return Fingerprint{digest.value()};
}

}  // namespace libchiaking::kakusan
