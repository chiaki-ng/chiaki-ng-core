/// @file
/// @brief Declares LinkReadings, what can be read off a link while it runs.
#pragma once

#include "libchiaking/platform/crypto/provider.hpp"
#include "src/core/net/protocol/remote_play/stream/diagnostics.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"

#include <cstdint>
#include <optional>

namespace libchiaking::play {

/// @brief What can be read off a link while it runs: its stream state, its
/// diagnostics and the Takion version it committed to.
///
/// Read by the turn's holder; the link driver publishes the state for other
/// threads.
class LinkReadings {
 public:
  /// @brief Allows destruction through the interface.
  virtual ~LinkReadings() = default;

  /// @brief Reads the stream's state.
  ///
  /// @return The state.
  virtual auto state() const noexcept
      -> net::protocol::remote_play::StreamState = 0;
  /// @brief Reads the stream's diagnostics.
  ///
  /// @return The diagnostics.
  virtual auto diagnostics() const noexcept
      -> net::protocol::remote_play::StreamDiagnostics = 0;
  /// @brief Reads the Takion version the stream committed to.
  ///
  /// @return The version; empty before it is known.
  virtual auto takion_version() const noexcept
      -> std::optional<std::uint32_t> = 0;
  /// @brief Names the ECDH curve the committed Takion version uses.
  ///
  /// @return secp521r1 or secp256k1; empty before the version is known or for
  ///     an unknown version.
  auto ecdh_curve() const noexcept -> std::optional<platform::EcdhCurve>;
};

}  // namespace libchiaking::play
