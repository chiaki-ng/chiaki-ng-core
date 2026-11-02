/// @file
/// @brief Declares kakusan::DeviceStore, the persistence a frontend
/// provides for Kakusan, and the records it stores.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/kakusan/values.hpp"
#include "libchiaking/detail/secret_array.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace libchiaking::kakusan {

/// @brief The persisted form of this device's Kakusan identity.
///
/// The seed is the private half of the key pair. A store should keep it where
/// the platform protects secrets. The record is move-only because of the seed;
/// clone() copies it deliberately.
struct SavedIdentity {
  /// @brief Size in bytes of the secret seed.
  static constexpr std::size_t seed_size = 32;

  /// @brief The secret seed the key pair is derived from; zeroed when the
  /// record is destroyed or moved from.
  SecretArray<seed_size> seed;
  /// @brief The public key that goes with the seed.
  IdentityKey key{};
  /// @brief The fingerprint of key.
  Fingerprint fingerprint{};

  /// @brief Makes an independent copy of the record, seed included.
  /// @return The copy; its seed is wiped when it is destroyed, independently
  ///     of this record.
  auto clone() const noexcept -> SavedIdentity {
    return SavedIdentity{seed.clone(), key, fingerprint};
  }
};

/// @brief The persisted form of a remembered peer device.
/// @see RememberedDevice
struct SavedDevice {
  /// @brief The device's public key; the record's identity in the store.
  IdentityKey key;
  /// @brief The nickname the user gave the device, already normalised.
  std::string nickname;
  /// @brief When the device was first verified and remembered.
  std::chrono::system_clock::time_point first_verified;
  /// @brief When the device last completed verification in a shared session.
  std::chrono::system_clock::time_point last_seen;
  /// @brief The fingerprint of key. It is written for the store's benefit; on
  /// loading, the library derives the fingerprint from key again and ignores
  /// this field.
  Fingerprint fingerprint{};

  /// @brief Compares two records field by field.
  friend auto operator==(const SavedDevice&, const SavedDevice&)
      -> bool = default;
};

/// @brief Persistence for Kakusan, implemented by the frontend.
///
/// Holds one identity record for this device and one record per remembered peer
/// device. The library calls it when Kakusan is first built, when a device
/// is remembered, renamed or forgotten, and each time a remembered device is
/// verified again. Calls about remembered devices are made one at a time; they
/// can come from any thread that uses Kakusan, including the thread that
/// runs a session.
///
/// Every call is synchronous: the caller waits for it. Calls about remembered
/// devices are made while the RememberedDevices lock is held, so an
/// implementation must not call RememberedDevices or Kakusan::remembered
/// members, and a slow store delays every other user of the list.
///
/// @see ChiakiNg::Stores::devices
class DeviceStore {
 public:
  /// @brief The single failure a store can report.
  enum class Error : std::uint8_t {
    /// @brief The store could not be read or written.
    unavailable,
  };

  /// @brief Destroys the store through its interface.
  virtual ~DeviceStore() = default;

  /// @brief Loads this device's identity.
  /// @return The saved identity, or an empty optional when none has been
  ///     saved yet, in which case the library creates one and calls
  ///     save_identity().
  /// @retval Error::unavailable The store could not be read.
  [[nodiscard]] virtual auto load_identity()
      -> Expected<std::optional<SavedIdentity>, Error> = 0;
  /// @brief Saves this device's identity, replacing any earlier one.
  /// @param identity The record to keep. Clone it to retain it beyond the
  ///     call.
  /// @return Nothing on success.
  /// @retval Error::unavailable The store could not be written.
  [[nodiscard]] virtual auto save_identity(const SavedIdentity& identity)
      -> Expected<void, Error> = 0;
  /// @brief Loads every remembered device.
  /// @return All saved device records, in any order.
  /// @retval Error::unavailable The store could not be read.
  [[nodiscard]] virtual auto load_devices()
      -> Expected<std::vector<SavedDevice>, Error> = 0;
  /// @brief Saves a remembered device, adding it or replacing the record with
  /// the same key.
  /// @param device The record to keep.
  /// @return Nothing on success.
  /// @retval Error::unavailable The store could not be written.
  [[nodiscard]] virtual auto save(const SavedDevice& device)
      -> Expected<void, Error> = 0;
  /// @brief Removes the remembered device with a given key.
  /// @param key The public key of the device to remove.
  /// @return Nothing on success.
  /// @retval Error::unavailable The store could not be written.
  [[nodiscard]] virtual auto remove(const IdentityKey& key)
      -> Expected<void, Error> = 0;
};

}  // namespace libchiaking::kakusan
