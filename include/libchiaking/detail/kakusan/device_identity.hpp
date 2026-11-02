/// @file
/// @brief Declares kakusan::DeviceIdentity, the key pair that identifies
/// this device to Kakusan peers.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/kakusan/device_store.hpp"
#include "libchiaking/detail/kakusan/values.hpp"
#include "libchiaking/detail/secret_array.hpp"

#include <cstddef>
#include <span>

namespace libchiaking::platform {
class CryptoProvider;
}

namespace libchiaking::platform {
class SecureRandomProvider;
}

namespace libchiaking::kakusan {

/// @brief This device's long-lived Kakusan identity: an Ed25519 key pair
/// and the fingerprint of its public half.
///
/// The public key and fingerprint are what the other side of a shared session
/// sees and can remember. The private seed stays inside the object in wiping
/// storage and is never exposed. A frontend normally does not build one:
/// Kakusan loads it from the DeviceStore, or creates and saves it the first
/// time, and offers it through Kakusan::this_device.
///
/// The type is move-only; clone() and saved() are the deliberate ways to copy
/// the secret seed.
class DeviceIdentity {
 public:
  /// @brief Makes a brand new identity from fresh random bytes.
  /// @param crypto Provider used to derive the public key and the
  ///     fingerprint.
  /// @param random Source of the secret seed.
  /// @return The new identity. It is not saved anywhere; pass saved() to
  ///     DeviceStore::save_identity to keep it.
  /// @retval Refusal::Kind::internal The random source failed, or the crypto
  ///     provider could not derive the key or the fingerprint.
  [[nodiscard]] static auto create(platform::CryptoProvider& crypto,
                                   platform::SecureRandomProvider& random)
      -> Expected<DeviceIdentity, Refusal>;
  /// @brief Rebuilds an identity from a seed saved earlier.
  ///
  /// Only the seed is used. The public key and fingerprint are derived from
  /// it again, so stale values in the saved record are ignored.
  ///
  /// @param crypto Provider used to derive the public key and the
  ///     fingerprint.
  /// @param saved The record a DeviceStore returned.
  /// @return The identity for that seed.
  /// @retval Refusal::Kind::internal The crypto provider could not derive the
  ///     key or the fingerprint.
  [[nodiscard]] static auto restore(platform::CryptoProvider& crypto,
                                    const SavedIdentity& saved)
      -> Expected<DeviceIdentity, Refusal>;
  /// @brief Computes the fingerprint of any device's public key.
  /// @param crypto Provider used for the SHA-256 digest.
  /// @param key The public key to fingerprint.
  /// @return The SHA-256 digest of the key.
  /// @retval Refusal::Kind::internal The crypto provider could not compute
  ///     the digest.
  [[nodiscard]] static auto fingerprint_of(platform::CryptoProvider& crypto,
                                           const IdentityKey& key)
      -> Expected<Fingerprint, Refusal>;

  /// @brief Not copyable; use clone() so each copy of the secret seed is
  /// explicit.
  DeviceIdentity(const DeviceIdentity&) = delete;
  /// @brief Not copy-assignable; use clone() so each copy of the secret seed
  /// is explicit.
  auto operator=(const DeviceIdentity&) -> DeviceIdentity& = delete;
  /// @brief Moves the identity; the source's seed is zeroed.
  DeviceIdentity(DeviceIdentity&&) noexcept = default;
  /// @brief Replaces this identity with another; the old seed and the
  /// source's seed are zeroed.
  auto operator=(DeviceIdentity&&) noexcept -> DeviceIdentity& = default;

  /// @brief The public key peers know this device by.
  /// @return The Ed25519 public key, valid while this identity lives.
  auto key() const noexcept -> const IdentityKey& { return key_; }
  /// @brief The fingerprint of key(), the value two people compare to confirm
  /// a device.
  /// @return The SHA-256 digest of key(), valid while this identity lives.
  auto fingerprint() const noexcept -> const Fingerprint& {
    return fingerprint_;
  }
  /// @brief The record to hand to DeviceStore::save_identity.
  /// @return A copy of the identity including the secret seed, in wiping
  ///     storage.
  auto saved() const noexcept -> SavedIdentity {
    return SavedIdentity{seed_.clone(), key_, fingerprint_};
  }
  /// @brief Makes a second identity object for the same key pair.
  /// @return A copy including the secret seed, wiped on its own schedule.
  auto clone() const noexcept -> DeviceIdentity {
    return DeviceIdentity{seed_.clone(), key_, fingerprint_};
  }

 private:
  friend class SharedSession;
  friend class GuestVisit;

  DeviceIdentity(SecretArray<SavedIdentity::seed_size> seed,
                 const IdentityKey& key,
                 const Fingerprint& fingerprint) noexcept
      : seed_(std::move(seed)), key_(key), fingerprint_(fingerprint) {}

  auto seed() const noexcept -> std::span<const std::byte> {
    return std::span<const std::byte>{seed_.data(), seed_.size()};
  }

  SecretArray<SavedIdentity::seed_size> seed_;
  IdentityKey key_;
  Fingerprint fingerprint_;
};

}  // namespace libchiaking::kakusan
