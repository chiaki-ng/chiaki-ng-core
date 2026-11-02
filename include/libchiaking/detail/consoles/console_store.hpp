/// @file
/// @brief Declares ConsoleStore, the persistence port for consoles and
/// pairings, and the rows it holds.
#pragma once

#include "libchiaking/detail/consoles/console_name.hpp"
#include "libchiaking/detail/consoles/device_uid.hpp"
#include "libchiaking/detail/consoles/firmware.hpp"
#include "libchiaking/detail/consoles/hardware_id.hpp"
#include "libchiaking/detail/consoles/host.hpp"
#include "libchiaking/detail/consoles/pairing.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/users/account_id.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace libchiaking::consoles {

/// @brief The facts about a console that are persisted.
struct SavedConsole {
  /// @brief The console's MAC address; the row's key.
  HardwareId hardware_id;
  /// @brief The firmware family last learned, which also fixes the
  /// generation.
  Firmware firmware;
  /// @brief The display name and its source.
  ConsoleName name;
  /// @brief The address the console was last reached at, if any.
  std::optional<Host> last_address{};

  /// @brief Compares every field.
  friend auto operator==(const SavedConsole&, const SavedConsole&) noexcept
      -> bool = default;
};

/// @brief One account's pairing with one console, as persisted.
///
/// The row holds the pairing keys, which are secrets. PairingKeys is move-only
/// and wipes its bytes on destruction, so the row is move-only too; use clone()
/// when a copy is really needed. An implementation of ConsoleStore is
/// responsible for protecting them at rest.
struct SavedPairing {
  /// @brief The account the pairing belongs to.
  AccountId account;
  /// @brief The console the pairing is with.
  HardwareId console;
  /// @brief The secret keys the console issued for this account.
  PairingKeys keys;
  /// @brief PSN's identifier for the console on this account, once it is
  /// known. Needed to reach a PS5 over the internet.
  std::optional<DeviceUid> device_uid{};

  /// @brief Makes a deliberate copy of the row, keys included.
  ///
  /// @return An independent row with its own copy of the keys.
  auto clone() const -> SavedPairing {
    return SavedPairing{account, console, keys.clone(), device_uid};
  }
};

/// @brief Port through which KnownConsoles persists consoles and pairings.
///
/// The application implements this over its own storage. The library calls it
/// from whichever thread performs the operation that needs it, including
/// several threads at once, so an implementation must be thread safe.
///
/// @see KnownConsoles
/// @see testing::InMemoryConsoleStore
class ConsoleStore {
 public:
  /// @brief Why a store call failed.
  enum class Error : std::uint8_t {
    /// @brief The storage could not be read or written.
    unavailable,
  };

  /// @brief Everything the store holds.
  struct Saved {
    /// @brief Every saved console.
    std::vector<SavedConsole> consoles;
    /// @brief Every saved pairing. Rows whose console is not among
    /// `consoles` are rejected by KnownConsoles::remember_saved.
    std::vector<SavedPairing> pairings;
  };

  /// @brief Allows deletion through the interface.
  virtual ~ConsoleStore() = default;

  /// @brief Loads every saved console and pairing.
  ///
  /// Called by KnownConsoles::remember_saved.
  ///
  /// @return The saved rows, or Error::unavailable when storage cannot be
  ///     read.
  [[nodiscard]] virtual auto load_saved_consoles()
      -> Expected<Saved, Error> = 0;
  /// @brief Writes a console's row, replacing any row with the same hardware
  /// id.
  ///
  /// @param console The facts to persist.
  /// @return Nothing on success, or Error::unavailable when the write failed.
  [[nodiscard]] virtual auto save(const SavedConsole& console)
      -> Expected<void, Error> = 0;
  /// @brief Writes a pairing, replacing any row for the same account and
  /// console.
  ///
  /// @param pairing The pairing to persist. Its keys are secret.
  /// @return Nothing on success, or Error::unavailable when the write failed.
  [[nodiscard]] virtual auto save(const SavedPairing& pairing)
      -> Expected<void, Error> = 0;
  /// @brief Deletes one account's pairing with a console.
  ///
  /// Called by KnownConsoles::unpair. The console's row and other accounts'
  /// pairings with it stay.
  ///
  /// @param account The account whose pairing goes.
  /// @param console The console it was with.
  /// @return Nothing on success, or Error::unavailable when the delete
  ///     failed.
  [[nodiscard]] virtual auto remove_pairing(AccountId account,
                                            HardwareId console)
      -> Expected<void, Error> = 0;
  /// @brief Deletes a console's row and every pairing with that console.
  ///
  /// Called by KnownConsoles::forget.
  ///
  /// @param console The console to delete.
  /// @return Nothing on success, or Error::unavailable when the delete
  ///     failed.
  [[nodiscard]] virtual auto remove_console(HardwareId console)
      -> Expected<void, Error> = 0;
};

}  // namespace libchiaking::consoles
