/// @file
/// @brief Declares kakusan::RememberedDevices, the list of peer devices the
/// user has verified and named.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/kakusan/device_store.hpp"
#include "libchiaking/detail/kakusan/values.hpp"
#include "libchiaking/platform/runtime/threading.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::platform {
class CryptoProvider;
}

namespace libchiaking::kakusan {

/// @brief A peer device the user verified once and gave a nickname, so it can
/// be recognised next time.
struct RememberedDevice {
  /// @brief The device's public key; what makes it this device.
  IdentityKey key;
  /// @brief The fingerprint of key, for showing to the user.
  Fingerprint fingerprint;
  /// @brief The nickname the user chose, trimmed. No two remembered devices
  /// have nicknames that compare equal under
  /// RememberedDevices::same_nickname.
  std::string nickname;
  /// @brief When the device was first remembered.
  std::chrono::system_clock::time_point first_verified;
  /// @brief When the device last completed verification in a shared session.
  std::chrono::system_clock::time_point last_seen;

  /// @brief Compares two devices field by field.
  friend auto operator==(const RememberedDevice&, const RememberedDevice&)
      -> bool = default;
};

/// @brief Whether a device that has just identified itself is one the user
/// remembers.
struct Recognition {
  /// @brief The verdict.
  enum class Kind : std::uint8_t {
    /// @brief The device is not remembered.
    unknown,
    /// @brief Its key belongs to a remembered device.
    recognised,
    /// @brief The nickname it was expected under is held by a remembered
    /// device with a different key, which can mean an impostor or a
    /// reinstalled device.
    changed_key,
  };

  /// @brief The verdict.
  Kind kind{Kind::unknown};
  /// @brief The remembered device the verdict refers to; empty for
  /// Kind::unknown. For Kind::changed_key it is the device that holds the
  /// nickname.
  std::optional<RememberedDevice> device{};

  /// @brief Compares two recognitions field by field.
  friend auto operator==(const Recognition&, const Recognition&)
      -> bool = default;
};

/// @brief Whether remembering a device may take a nickname that another device
/// holds.
enum class Replace : std::uint8_t {
  /// @brief Refuses with RememberRefusal::Kind::nickname_taken.
  no,
  /// @brief Forgets the other device and gives the nickname to this one.
  yes,
};

/// @brief Why a device could not be remembered or renamed.
struct RememberRefusal {
  /// @brief The cause.
  enum class Kind : std::uint8_t {
    /// @brief The nickname is blank, has a control character, or is longer
    /// than RememberedDevices::longest_nickname.
    invalid_nickname,
    /// @brief Another remembered device already has this nickname; holder
    /// says which.
    nickname_taken,
    /// @brief No device with that key or guest id is known.
    unknown_device,
    /// @brief The device has not completed verification in the current
    /// session.
    not_verified,
    /// @brief The DeviceStore failed, or the fingerprint could not be
    /// computed. Nothing was changed.
    store_unavailable,
  };

  /// @brief The cause.
  Kind kind{Kind::store_unavailable};
  /// @brief The device that holds the nickname; present only for
  /// Kind::nickname_taken.
  std::optional<RememberedDevice> holder{};

  /// @brief Compares two refusals field by field.
  friend auto operator==(const RememberRefusal&, const RememberRefusal&)
      -> bool = default;
};

/// @brief The devices the user has chosen to remember, kept in memory and
/// written through to the DeviceStore.
///
/// Reached through Kakusan::remembered. A device is added from inside a
/// session, with SharedSession::remember on the host's side or
/// GuestVisit::remember_host on the guest's, because only a device that has
/// just been verified can be remembered. This class lists, recognises, renames
/// and forgets them. Every member may be called from any thread.
class RememberedDevices {
 public:
  /// @brief Longest nickname, counted in Unicode code points.
  static constexpr std::size_t longest_nickname = 40;

  /// @brief Loads the remembered devices from a store.
  /// @param crypto Provider used to compute each device's fingerprint. It
  ///     must outlive the result.
  /// @param threading Makes the lock that guards the list. It must outlive
  ///     the result.
  /// @param store The store to read now and write later. It must outlive the
  ///     result.
  /// @return The loaded list.
  /// @retval DeviceStore::Error::unavailable The store could not be read, or
  ///     a fingerprint could not be computed.
  [[nodiscard]] static auto load(platform::CryptoProvider& crypto,
                                 platform::ThreadingProvider& threading,
                                 DeviceStore& store)
      -> Expected<std::unique_ptr<RememberedDevices>, DeviceStore::Error>;
  /// @brief Checks a nickname and returns the form that would be stored.
  ///
  /// Leading and trailing ASCII white space is removed. The text is expected
  /// to be UTF-8.
  ///
  /// @param nickname What the user typed.
  /// @return The trimmed nickname, or an empty optional when it is blank,
  ///     contains a control character, or is longer than longest_nickname.
  [[nodiscard]] static auto normalised(std::string_view nickname)
      -> std::optional<std::string>;
  /// @brief Tells whether two nicknames count as the same name.
  ///
  /// Compares without regard to ASCII letter case, Latin-1 accents or
  /// combining marks, so "Zoe", "zoe" and "Zoë" are one name.
  ///
  /// @param left One nickname, in UTF-8.
  /// @param right The other nickname, in UTF-8.
  /// @return True when they would clash.
  static auto same_nickname(std::string_view left, std::string_view right)
      -> bool;

  /// @brief Not copyable; there is one list per Kakusan.
  RememberedDevices(const RememberedDevices&) = delete;
  /// @brief Not copy-assignable; there is one list per Kakusan.
  auto operator=(const RememberedDevices&) -> RememberedDevices& = delete;

  /// @brief Every remembered device.
  /// @return A copy of the list as it is now.
  auto all() const -> std::vector<RememberedDevice>;
  /// @brief Looks a device up by its key, and optionally by the nickname it
  /// claims.
  /// @param key The public key the device presented.
  /// @param expected_nickname A nickname the device is expected to hold. When
  ///     given and some remembered device holds it, the verdict is about that
  ///     device: Recognition::Kind::recognised if the keys match,
  ///     Recognition::Kind::changed_key if not. When absent, or nobody holds
  ///     the nickname, the lookup is by key alone.
  /// @return The verdict and the remembered device it refers to.
  auto recognise(const IdentityKey& key,
                 std::optional<std::string_view> expected_nickname = {}) const
      -> Recognition;
  /// @brief Looks a device up by the start of its key, as an invitation or
  /// reply states it.
  /// @param hint The first eight bytes of the key the text claims.
  /// @return Recognition::Kind::recognised with the remembered device whose
  ///     key starts with the hint, otherwise Recognition::Kind::unknown. The
  ///     verdict is provisional: the device has not yet proven it holds the
  ///     key.
  auto recognise(const IdentityHint& hint) const -> Recognition;
  /// @brief Gives a remembered device a new nickname.
  /// @param key The device to rename.
  /// @param nickname The new nickname; see normalised().
  /// @return The device as saved.
  /// @retval RememberRefusal::Kind::invalid_nickname The nickname is not
  ///     acceptable.
  /// @retval RememberRefusal::Kind::nickname_taken Another device holds the
  ///     nickname; RememberRefusal::holder names it.
  /// @retval RememberRefusal::Kind::unknown_device No remembered device has
  ///     this key.
  /// @retval RememberRefusal::Kind::store_unavailable The DeviceStore failed;
  ///     nothing changed.
  [[nodiscard]] auto rename(const IdentityKey& key, std::string_view nickname)
      -> Expected<RememberedDevice, RememberRefusal>;
  /// @brief Stops remembering a device.
  ///
  /// Removes it from the store first and from memory only if that worked. A
  /// session already in progress with the device is not affected.
  ///
  /// @param key The device to forget.
  /// @return Nothing on success.
  /// @retval DeviceStore::Error::unavailable The DeviceStore failed; the
  ///     device is still remembered.
  [[nodiscard]] auto forget(const IdentityKey& key)
      -> Expected<void, DeviceStore::Error>;

 private:
  friend class SharedSession;
  friend class GuestVisit;

  RememberedDevices(platform::CryptoProvider& crypto,
                    platform::ThreadingProvider& threading, DeviceStore& store,
                    std::vector<RememberedDevice> devices);

  [[nodiscard]] auto remember(const IdentityKey& key, std::string_view nickname,
                              Replace replace,
                              std::chrono::system_clock::time_point now)
      -> Expected<RememberedDevice, RememberRefusal>;
  void seen(const IdentityKey& key, std::chrono::system_clock::time_point now);

  static auto folded(std::string_view nickname) -> std::u32string;
  static auto saved(const RememberedDevice& device) -> SavedDevice;
  auto named(std::string_view nickname) const -> const RememberedDevice*;
  auto held(const IdentityKey& key) -> RememberedDevice*;

  platform::CryptoProvider& crypto_;
  DeviceStore& store_;
  mutable platform::Mutex mutex_;
  std::vector<RememberedDevice> devices_;
};

}  // namespace libchiaking::kakusan
