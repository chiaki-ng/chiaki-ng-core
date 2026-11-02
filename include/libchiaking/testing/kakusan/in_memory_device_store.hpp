/// @file
/// @brief Declares InMemoryDeviceStore, a Kakusan device store kept in
/// memory.
#pragma once

#include "libchiaking/detail/kakusan/device_store.hpp"
#include "libchiaking/platform/runtime/threading.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

namespace libchiaking::kakusan::testing {

/// @brief Stands in for DeviceStore without a keychain or database.
///
/// It holds this device's Kakusan identity and the list of devices it has
/// met, in memory for the life of the object. Hand one to Kakusan (or one
/// for each side to KakusanPair), run the code under test, then read back
/// identity(), devices() and the save counters. become_unavailable() makes
/// every DeviceStore operation fail, which plays a locked or missing store.
///
/// All members lock internally, so a test may read it while the library is
/// running.
class InMemoryDeviceStore final : public DeviceStore {
 public:
  /// @brief Creates an empty store.
  ///
  /// @param threading Makes the lock that guards the store; must outlive it.
  explicit InMemoryDeviceStore(platform::ThreadingProvider& threading)
      : mutex_(threading) {}

  /// @brief Returns this device's saved identity, if one was saved.
  ///
  /// @return An independent copy of the identity, or an empty optional when
  ///     none has been saved yet. An empty optional is a success, not an
  ///     error.
  /// @retval Error::unavailable The store was made unavailable.
  [[nodiscard]] auto load_identity()
      -> Expected<std::optional<SavedIdentity>, Error> override {
    const platform::Lock lock{mutex_};
    if (unavailable_) return unexpected(Error::unavailable);
    if (!identity_) return std::optional<SavedIdentity>{};
    return std::optional<SavedIdentity>{identity_->clone()};
  }

  /// @brief Saves this device's identity, replacing any earlier one.
  ///
  /// @param identity The identity to keep; a copy is stored.
  /// @return Nothing on success, after adding one to identity_saves().
  /// @retval Error::unavailable The store was made unavailable. Nothing
  ///     changes.
  [[nodiscard]] auto save_identity(const SavedIdentity& identity)
      -> Expected<void, Error> override {
    const platform::Lock lock{mutex_};
    if (unavailable_) return unexpected(Error::unavailable);
    identity_.emplace(identity.clone());
    ++identity_saves_;
    return {};
  }

  /// @brief Returns every saved device.
  ///
  /// @return Copies of the saved devices, in the order they were first saved.
  /// @retval Error::unavailable The store was made unavailable.
  [[nodiscard]] auto load_devices()
      -> Expected<std::vector<SavedDevice>, Error> override {
    const platform::Lock lock{mutex_};
    if (unavailable_) return unexpected(Error::unavailable);
    return devices_;
  }

  /// @brief Saves a device, updating in place one with the same identity key.
  ///
  /// @param device The device to keep; a copy is stored.
  /// @return Nothing on success, after adding one to device_saves().
  /// @retval Error::unavailable The store was made unavailable. Nothing
  ///     changes.
  [[nodiscard]] auto save(const SavedDevice& device)
      -> Expected<void, Error> override {
    const platform::Lock lock{mutex_};
    if (unavailable_) return unexpected(Error::unavailable);
    const auto found =
        std::ranges::find(devices_, device.key, &SavedDevice::key);
    if (found == devices_.end())
      devices_.push_back(device);
    else
      *found = device;
    ++device_saves_;
    return {};
  }

  /// @brief Forgets the device with the given identity key.
  ///
  /// @param key The identity key of the device to forget.
  /// @return Nothing on success, including when no such device was held.
  ///     Removals are not counted.
  /// @retval Error::unavailable The store was made unavailable. Nothing
  ///     changes.
  [[nodiscard]] auto remove(const IdentityKey& key)
      -> Expected<void, Error> override {
    const platform::Lock lock{mutex_};
    if (unavailable_) return unexpected(Error::unavailable);
    std::erase_if(devices_, [&key](const SavedDevice& device) {
      return device.key == key;
    });
    return {};
  }

  /// @brief Scripts every DeviceStore operation to fail or to work again.
  ///
  /// @param unavailable True to fail loads, saves and removals with
  ///     Error::unavailable; false to restore normal behaviour. What is
  ///     already held is kept either way.
  void become_unavailable(bool unavailable = true) {
    const platform::Lock lock{mutex_};
    unavailable_ = unavailable;
  }

  /// @brief Reads back the saved identity, for assertions.
  ///
  /// @return A copy of the identity, or nothing when none was saved. Not
  ///     affected by become_unavailable().
  [[nodiscard]] auto identity() const -> std::optional<SavedIdentity> {
    const platform::Lock lock{mutex_};
    if (!identity_) return std::nullopt;
    return identity_->clone();
  }

  /// @brief Reads back the saved devices, for assertions.
  ///
  /// @return A snapshot of the devices held. Not affected by
  ///     become_unavailable().
  auto devices() const -> std::vector<SavedDevice> {
    const platform::Lock lock{mutex_};
    return devices_;
  }

  /// @brief Reports how many times save_identity() has succeeded.
  /// @return The count since construction; failed saves are not counted.
  auto identity_saves() const -> std::size_t {
    const platform::Lock lock{mutex_};
    return identity_saves_;
  }

  /// @brief Reports how many times a device has been saved successfully.
  /// @return The count of successful save() calls since construction,
  ///     updates of an existing device included.
  auto device_saves() const -> std::size_t {
    const platform::Lock lock{mutex_};
    return device_saves_;
  }

 private:
  mutable platform::Mutex mutex_;
  std::optional<SavedIdentity> identity_;
  std::vector<SavedDevice> devices_;
  std::size_t identity_saves_{};
  std::size_t device_saves_{};
  bool unavailable_{};
};

}  // namespace libchiaking::kakusan::testing
