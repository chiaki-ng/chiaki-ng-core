/// @file
/// @brief Declares InternetPairing, the port KnownConsoles uses to pair with a
/// console through PSN.
#pragma once

#include "libchiaking/detail/consoles/device_uid.hpp"
#include "libchiaking/detail/consoles/firmware.hpp"
#include "libchiaking/detail/consoles/pairing.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/platform/runtime/stop_token.hpp"

namespace libchiaking::consoles {

/// @brief Port for pairing with a console over the internet.
///
/// Pairing over the internet needs the same PSN signalling and connection
/// set-up as playing over the internet, which lives in the play context. That
/// context implements this interface (play::InternetPairer) and the application
/// passes it to the KnownConsoles constructor. Without one,
/// KnownConsoles::pair_over_the_internet fails with
/// PairingFailure::Reason::internet_pairing_missing.
///
/// @see KnownConsoles::pair_over_the_internet
class InternetPairing {
 public:
  /// @brief Allows deletion through the interface.
  virtual ~InternetPairing() = default;

  /// @brief Reaches a console through PSN and obtains pairing keys from it.
  ///
  /// The call blocks until the pairing finishes. The implementation does not
  /// save anything; KnownConsoles saves the result.
  ///
  /// @param as The signed-in user to pair as.
  /// @param which PSN's identifier for the console.
  /// @param generation Whether the console is a PS4 or a PS5.
  /// @param watcher Told the stages and steps reached. May be null.
  /// @param stop Cancels the attempt.
  /// @return The paired console and its secret keys, or the reason pairing
  ///     failed. PairingFailure::internet_stage says how far the attempt got.
  [[nodiscard]] virtual auto pair(users::PsUser& as, const DeviceUid& which,
                                  Generation generation,
                                  PairingWatcher* watcher,
                                  const platform::StopToken& stop)
      -> Expected<PairedConsole, PairingFailure> = 0;
};

}  // namespace libchiaking::consoles
