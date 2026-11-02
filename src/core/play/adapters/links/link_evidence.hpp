/// @file
/// @brief Declares LinkEvidence, the translation between a link's stream
/// errors and quit reasons and the public reasons a session ended for.
#pragma once

#include "libchiaking/detail/play/outcome.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/play/adapters/session/rest_request.hpp"

#include <optional>
#include <span>
#include <utility>

namespace libchiaking::play {

/// @brief Maps between the stream's own errors and quits and the public
/// EndReason, ConsoleQuit and RestOutcome. Stateless; tables are constant.
class LinkEvidence {
 public:
  /// @brief Names the public cause of a stream error.
  ///
  /// @param error The stream error.
  /// @return The matching cause; EndReason::Cause::invalid_state for one not
  ///     in the table.
  static auto cause(net::protocol::remote_play::StreamError error) noexcept
      -> EndReason::Cause;
  /// @brief Names the stream error behind a public cause.
  ///
  /// @param cause The cause.
  /// @return The first matching error; StreamError::invalid_state for one not
  ///     in the table.
  static auto error(EndReason::Cause cause) noexcept
      -> net::protocol::remote_play::StreamError;
  /// @brief Turns the other end's quit into the public quit.
  ///
  /// @param quit The reason and text.
  /// @return The public quit with the text; empty for none or an unmapped
  ///     reason.
  static auto quit(const net::protocol::remote_play::QuitDetail& quit)
      -> std::optional<ConsoleQuit>;
  /// @brief Names the stream quit reason behind a public quit reason.
  ///
  /// @param reason The public reason.
  /// @return The stream's reason; QuitReason::none for one not in the table.
  static auto quit_reason(ConsoleQuit::Reason reason) noexcept
      -> net::protocol::remote_play::QuitReason;
  /// @brief Turns a rest request's outcome into the public one.
  ///
  /// @param outcome The request's outcome.
  /// @return The matching public outcome.
  static auto rest(RestRequest::Outcome outcome) noexcept -> RestOutcome;

 private:
  /// @brief A stream error and its public cause.
  using Causes =
      std::pair<net::protocol::remote_play::StreamError, EndReason::Cause>;
  /// @brief A stream quit reason and its public reason.
  using Quits =
      std::pair<net::protocol::remote_play::QuitReason, ConsoleQuit::Reason>;

  /// @brief The error table; session_init_refused appears twice, so a pairing
  /// refusal maps back to it too.
  ///
  /// @return The table.
  static auto causes() noexcept -> std::span<const Causes>;
  /// @brief The quit table.
  ///
  /// @return The table.
  static auto quits() noexcept -> std::span<const Quits>;
};

}  // namespace libchiaking::play
