/// @file
/// @brief Declares InternetPairer, which pairs a console through PSN without
/// being on its network.
#pragma once

#include "libchiaking/detail/consoles/device_uid.hpp"
#include "libchiaking/detail/consoles/firmware.hpp"
#include "libchiaking/detail/consoles/internet_pairing.hpp"
#include "libchiaking/detail/consoles/pairing.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/platform/runtime/stop_token.hpp"

#include <memory>

namespace libchiaking::platform {
class DiagnosticsProvider;
class PlatformFactory;
}  // namespace libchiaking::platform

namespace libchiaking::play {

/// @brief Pairs with a console over the internet, using the player's PSN
/// account.
///
/// Pairing at home needs the console's PIN and the local network. This pairer
/// needs neither: it reaches a console that is linked to the player's PSN
/// account, through PSN. It implements consoles::InternetPairing, the port the
/// consoles context pairs through, and the same object is given to Play.
///
/// Pairing and playing exclude each other. Only one pairing runs at a time, a
/// pairing is refused while a session started by Play is live, and Play refuses
/// to start a session while a pairing runs.
///
/// @see Play
/// @ingroup api_play
class InternetPairer final : public consoles::InternetPairing {
 public:
  /// @brief Implementation behind InternetPairer; not part of the public
  /// surface.
  class Core;

  /// @brief Creates a pairer on the frontend's platform services.
  ///
  /// @param platform Source of the network, crypto, HTTP and WebSocket
  ///     providers. Borrowed; must outlive the pairer.
  /// @param log Sink for diagnostics; when null, the one `platform` provides
  ///     is used, and nothing is logged if it has none.
  explicit InternetPairer(platform::PlatformFactory& platform,
                          platform::DiagnosticsProvider* log = nullptr);
  /// @brief Not copyable; a frontend has one pairer.
  InternetPairer(const InternetPairer&) = delete;
  /// @brief Not copy-assignable; a frontend has one pairer.
  auto operator=(const InternetPairer&) -> InternetPairer& = delete;
  /// @brief Destroys the pairer.
  ///
  /// @pre No pairing is in progress and no Play still refers to this pairer.
  ~InternetPairer() override;

  /// @brief Pairs one console of the player's PSN account.
  ///
  /// The call blocks until the pairing succeeds, fails or is cancelled, so it
  /// belongs on a worker thread. It only produces the pairing; saving it is
  /// left to the caller.
  ///
  /// @param as Player to pair for. Must have a PSN account. Borrowed for the
  ///     duration of the call.
  /// @param which Identifier PSN gives the console on the player's account.
  /// @param generation Generation of that console.
  /// @param watcher Receives stage changes and progress while the call runs;
  ///     may be null.
  /// @param stop Cancels the pairing.
  /// @return The paired console with its keys, ready to be saved.
  /// @retval consoles::PairingFailure::Reason::unanswered The player has no
  ///     PSN account, or PSN denied access;
  ///     consoles::PairingFailure::unanswered says which.
  /// @retval consoles::PairingFailure::Reason::busy Another pairing is
  ///     already running on this pairer.
  /// @retval consoles::PairingFailure::Reason::session_playing A session
  ///     started by Play is live.
  /// @retval consoles::PairingFailure::Reason::cancelled `stop` was
  ///     requested.
  /// @retval consoles::PairingFailure::Reason::malformed The request or the
  ///     console's reply could not be used.
  /// @note Other reasons, such as `unreachable`, `refused` and `timed_out`,
  ///     report failures on the way to the console;
  ///     consoles::PairingFailure::internet_stage says how far the pairing
  ///     got.
  [[nodiscard]] auto pair(users::PsUser& as, const consoles::DeviceUid& which,
                          consoles::Generation generation,
                          consoles::PairingWatcher* watcher,
                          const platform::StopToken& stop)
      -> Expected<consoles::PairedConsole, consoles::PairingFailure> override;

  /// @brief Returns the implementation, for the library's own wiring and
  /// tests.
  ///
  /// @return The implementation, for the library's own wiring and tests.
  auto core() noexcept -> Core& { return *core_; }

 private:
  std::unique_ptr<Core> core_;
};

}  // namespace libchiaking::play
