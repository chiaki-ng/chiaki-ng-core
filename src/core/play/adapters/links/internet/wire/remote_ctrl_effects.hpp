/// @file
/// @brief Declares Effects, which carries out what an internet
/// control session's lifecycle asks for: heartbeats, PIN prompts, notices and
/// queued outbound frames.
#pragma once

#include "src/core/net/protocol/remote_play/stream/ctrl_features.hpp"
#include "src/core/play/adapters/links/internet/wire/remote_ctrl.hpp"

#include <cstdint>
#include <variant>
#include <vector>

namespace libchiaking::play::internet::remote_ctrl {

/// @brief What Effects::step found to do.
enum class EffectProgress : std::uint8_t {
  /// @brief Nothing more to do now.
  idle,
  /// @brief The console asks for a PIN; reported once.
  pin_required,
  /// @brief The session id fallback must be published.
  fallback_required,
  /// @brief The lifecycle failed.
  failed,
  /// @brief The control session failed.
  ctrl_failed,
};

/// @brief Why a step failed: the outbound send's error.
using EffectError = internet::remote_ctrl::OutboundStepError;

/// @brief Carries out the control lifecycle's effects on a session.
///
/// Static functions only; each sends on the channel and so blocks on it. Use
/// on the session's thread.
class Effects {
 public:
  /// @brief Takes the lifecycle's effects until one needs the caller.
  ///
  /// Heartbeat replies are sent and switch acks dropped on the way. A PIN
  /// request after the first only lets queued heartbeat replies through.
  ///
  /// @param owner The control session.
  /// @param channel The RUDP channel.
  /// @param crypto Seals the frames.
  /// @param target The firmware target.
  /// @param pin_notified Whether the PIN prompt was already reported.
  /// @return What was found; EffectProgress::pin_required when the
  ///     prompt is to be reported now; the heartbeat send's error.
  [[nodiscard]] static auto step(
      internet::remote_ctrl::Session& owner,
      net::transport::rudp::HandshakeChannel& channel,
      platform::CryptoProvider& crypto,
      net::protocol::remote_play::RegistrationTarget target, bool pin_notified)
      -> Expected<EffectProgress, EffectError>;

  /// @brief Passes the session's queued notices to the control features.
  ///
  /// @param owner The control session.
  /// @param features Takes the notices.
  /// @param events Receives the stream events the notices raise.
  /// @param couch The couch backend notices go to; may be null.
  static void drain_notices(
      internet::remote_ctrl::Session& owner,
      net::protocol::remote_play::CtrlFeatures& features,
      std::vector<net::protocol::remote_play::StreamEvent>& events,
      net::protocol::remote_play::CouchBackend* couch = nullptr);

  /// @brief Sends the features' queued frames once the session is active and
  /// its post-id exchange completed, repeating the post-id features first.
  ///
  /// @param owner The control session.
  /// @param channel The RUDP channel.
  /// @param crypto Seals the frames.
  /// @param target The firmware target.
  /// @param features Holds the queued frames; each sent frame is removed.
  /// @param enable_dualsense Whether DualSense features are offered.
  /// @param enable_keyboard Whether keyboard features are offered.
  /// @return Nothing, also when the session is not active yet; the send's
  ///     error, leaving the unsent frames queued.
  [[nodiscard]] static auto flush_outbound(
      internet::remote_ctrl::Session& owner,
      net::transport::rudp::HandshakeChannel& channel,
      platform::CryptoProvider& crypto,
      net::protocol::remote_play::RegistrationTarget target,
      net::protocol::remote_play::CtrlFeatures& features,
      bool enable_dualsense = false, bool enable_keyboard = false)
      -> Expected<void, internet::remote_ctrl::OutboundStepError>;
};

}  // namespace libchiaking::play::internet::remote_ctrl
