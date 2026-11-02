/// @file
/// @brief Declares InternetLinks, the factory of links to a console over the
/// internet through PSN.
#pragma once

#include "src/core/play/adapters/links/link.hpp"

#include "src/core/consoles/paired_play.hpp"
#include "src/core/play/adapters/links/internet/internet_link_configuration.hpp"
#include "src/core/play/adapters/links/internet/internet_start.hpp"
#include "src/core/play/adapters/start/link_request.hpp"

#include <chrono>
#include <memory>

namespace libchiaking::play {

/// @brief Builds the configuration and the link of an internet start, for
/// play or for pairing alone. Stateless.
class InternetLinks {
 public:
  /// @brief Builds the configuration for reaching a console over PSN.
  ///
  /// IPv6 is always enabled here; the start offers it to a PS5 only.
  ///
  /// @param target The console as found on the account.
  /// @param as The user to play as.
  /// @param video The picture.
  /// @param preferences Controller, feedback, keyboard, keyframe and tuning.
  /// @param port_guessing How NAT ports are guessed.
  /// @param start_timeout How long the start may take; must be positive.
  /// @return The configuration; ConfigurationError::invalid_credential without
  ///     a PSN account; ConfigurationError::invalid_console for a target that
  ///     is neither PS4 10 nor PS5 1; ConfigurationError::rejected for a
  ///     timeout that is not positive, a refused controller or a failed
  ///     validation.
  [[nodiscard]] static auto configured(
      const consoles::PairedPlay::InternetTarget& target,
      std::shared_ptr<users::PsUser> as, VideoRequest video,
      ConsolePreferences preferences, PortGuessing port_guessing,
      std::chrono::seconds start_timeout)
      -> Expected<InternetLinkConfiguration, ConfigurationError>;

  /// @brief Builds a link that reaches the console over PSN and streams.
  ///
  /// @param configuration The checked configuration.
  /// @param platform The host's providers; must outlive the link.
  /// @param pairings Told when the start pairs the console; may be null.
  /// @return The link; StreamError::provider_unavailable without crypto, HTTP
  ///     or websocket providers.
  [[nodiscard]] static auto playing(
      InternetLinkConfiguration configuration,
      platform::PlatformFactory& platform,
      play::InternetStart::PairingListener* pairings)
      -> Expected<std::unique_ptr<Link>,
                  net::protocol::remote_play::StreamError>;

  /// @brief Builds a link that only pairs with the console over PSN.
  ///
  /// @param configuration The checked configuration.
  /// @param platform The host's providers; must outlive the link.
  /// @return The link; StreamError::provider_unavailable without crypto, HTTP
  ///     or websocket providers.
  [[nodiscard]] static auto pairing_only(
      InternetLinkConfiguration configuration,
      platform::PlatformFactory& platform)
      -> Expected<std::unique_ptr<Link>,
                  net::protocol::remote_play::StreamError>;
};

}  // namespace libchiaking::play
