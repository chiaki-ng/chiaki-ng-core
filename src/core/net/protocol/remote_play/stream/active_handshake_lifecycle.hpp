/// @file
/// @brief The state machine of the active session's handshake: connection,
/// profile commit and BIG, BANG verification, crypto install, and the
/// stream info the console may send before the keys are in place.
#pragma once

#include "src/core/net/protocol/remote_play/stream/stream_info.hpp"

#include <boost/sml.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

namespace libchiaking::net::protocol::remote_play {

/// @brief The phase of the active session's handshake.
enum class ActiveHandshakeState : std::uint8_t {
  /// @brief Waiting for Takion to connect (INIT and cookie).
  awaiting_connection,
  /// @brief Waiting for the Takion version to be committed, then building BIG.
  awaiting_profile,
  /// @brief BIG sent; waiting for the console's BANG.
  awaiting_bang,
  /// @brief Checking the BANG's signed key.
  verifying_bang,
  /// @brief Waiting for Takion to install the stream ciphers.
  awaiting_crypto_install,
  /// @brief Waiting for the console's stream info.
  awaiting_stream_info,
  /// @brief Checking the stream info.
  validating_stream_info,
  /// @brief The stream info was acknowledged; media flows.
  ready,
  /// @brief The session closed.
  closed,
  /// @brief The handshake failed.
  failed,
};

}  // namespace libchiaking::net::protocol::remote_play

namespace libchiaking::net::protocol::remote_play::active_handshake_lifecycle {
namespace sml = boost::sml;

/// @brief State tag: waiting for Takion to connect.
struct AwaitingConnection {};
/// @brief State tag: waiting for the version.
struct AwaitingProfile {};
/// @brief State tag: building BIG for the committed version.
struct CommittingProfile {};
/// @brief State tag: waiting for BANG.
struct AwaitingBang {};
/// @brief State tag: verifying BANG.
struct VerifyingBang {};
/// @brief State tag: waiting for the ciphers.
struct AwaitingCryptoInstall {};
/// @brief State tag: waiting for the stream info.
struct AwaitingStreamInfo {};
/// @brief State tag: checking the stream info.
struct ValidatingStreamInfo {};
/// @brief State tag: ready.
struct Ready {};
/// @brief State tag: closed.
struct Closed {};
/// @brief State tag: failed.
struct Failed {};

/// @brief Event: Takion connected.
struct TransportConnected {};
/// @brief Event: Takion committed a version.
struct ProfileArrived {};
/// @brief Event: the version is not one this session accepts.
struct VersionUnsupported {};
/// @brief Event: BIG could not be built.
struct BigUnbuildable {};
/// @brief Event: BIG was built and sent.
struct BigBuilt {};
/// @brief Event: BANG arrived.
struct BangArrived {};
/// @brief Event: BANG was refused; another may follow.
struct BangRejected {};
/// @brief Event: BANG could not be verified at all.
struct BangUnverifiable {};
/// @brief Event: BANG was verified.
struct BangVerified {};
/// @brief Event: the ciphers could not be prepared.
struct CryptoUnpreparable {};
/// @brief Event: Takion installed the ciphers.
struct CryptoInstalled {};
/// @brief Event: the stream info arrived.
struct StreamInfoArrived {
  /// @brief The stream info; held by moving from it when it arrives early.
  takion::StreamInfo& info;
};
/// @brief Event: the stream info was accepted.
struct StreamInfoAccepted {};
/// @brief Event: the stream info was refused; another may follow.
struct StreamInfoRejected {};
/// @brief Event: the stream info is unusable.
struct StreamInfoInvalid {};
/// @brief Event: the console did not take the acknowledgement; the stream
/// info will be resent.
struct StreamInfoUnacknowledged {};
/// @brief Event: the session closed.
struct SessionClosed {};
/// @brief Event: the session failed.
struct SessionFailed {};

/// @brief Effect: a version arrived before Takion connected.
struct ProfileTooEarly {};
/// @brief Effect: build and send BIG for the committed version.
struct BuildBig {};
/// @brief Effect: check the stream info that just arrived.
struct ForwardStreamInfo {};
/// @brief Effect: check the stream info held from before the ciphers.
struct ForwardHeldStreamInfo {};

/// @brief What the handshake must do after an event.
using Effect = std::variant<ProfileTooEarly, BuildBig, ForwardStreamInfo,
                            ForwardHeldStreamInfo>;

/// @brief The machine's data.
struct Context {
  /// @brief A stream info that arrived before the ciphers were installed.
  std::optional<takion::StreamInfo> held_stream_info;
  /// @brief The last event's effect.
  std::optional<Effect> effect;
  /// @brief Events the machine refused.
  std::size_t unexpected_events{};
};

/// @brief The transition table.
struct Fsm {
  /// @brief Builds the table.
  ///
  /// @return The boost::sml table.
  auto operator()() const noexcept {
    using namespace sml;
    const auto holds_stream_info = [](const Context& c) {
      return c.held_stream_info.has_value();
    };
    const auto refuse_early_profile = [](Context& c) {
      c.effect = ProfileTooEarly{};
    };
    const auto build_big = [](Context& c) { c.effect = BuildBig{}; };
    const auto hold_stream_info = [](const StreamInfoArrived& e, Context& c) {
      if (!c.held_stream_info) c.held_stream_info = std::move(e.info);
    };
    const auto forward_stream_info = [](Context& c) {
      c.effect = ForwardStreamInfo{};
    };
    const auto forward_held_stream_info = [](Context& c) {
      c.effect = ForwardHeldStreamInfo{};
    };
    const auto drop_held = [](Context& c) { c.held_stream_info.reset(); };

    return make_transition_table(
        *state<AwaitingConnection> + sml::event<TransportConnected> =
            state<AwaitingProfile>,
        state<AwaitingConnection> +
            sml::event<ProfileArrived> / refuse_early_profile,
        state<AwaitingProfile> + sml::event<ProfileArrived> / build_big =
            state<CommittingProfile>,
        state<CommittingProfile> + sml::event<VersionUnsupported> =
            state<Failed>,
        state<CommittingProfile> + sml::event<BigUnbuildable> = state<Failed>,
        state<CommittingProfile> + sml::event<BigBuilt> = state<AwaitingBang>,

        state<AwaitingBang> + sml::event<BangArrived> = state<VerifyingBang>,
        state<AwaitingBang> + sml::event<StreamInfoArrived> / hold_stream_info,
        state<VerifyingBang> + sml::event<StreamInfoArrived> / hold_stream_info,
        state<VerifyingBang> + sml::event<BangRejected> = state<AwaitingBang>,
        state<VerifyingBang> + sml::event<BangUnverifiable> = state<Failed>,
        state<VerifyingBang> + sml::event<CryptoUnpreparable> = state<Failed>,
        state<VerifyingBang> + sml::event<BangVerified> =
            state<AwaitingCryptoInstall>,

        state<AwaitingCryptoInstall> +
            sml::event<StreamInfoArrived> / hold_stream_info,
        state<AwaitingCryptoInstall> +
            sml::event<CryptoInstalled>[holds_stream_info] /
                forward_held_stream_info = state<ValidatingStreamInfo>,
        state<AwaitingCryptoInstall> + sml::event<CryptoInstalled> =
            state<AwaitingStreamInfo>,

        state<AwaitingStreamInfo> +
            sml::event<StreamInfoArrived> / forward_stream_info =
            state<ValidatingStreamInfo>,
        state<ValidatingStreamInfo> + sml::event<StreamInfoAccepted> =
            state<Ready>,
        state<ValidatingStreamInfo> + sml::event<StreamInfoRejected> =
            state<AwaitingStreamInfo>,
        state<ValidatingStreamInfo> + sml::event<StreamInfoInvalid> =
            state<Failed>,
        state<Ready> + sml::event<StreamInfoUnacknowledged> =
            state<AwaitingStreamInfo>,

        state<AwaitingConnection> + sml::event<SessionClosed> / drop_held =
            state<Closed>,
        state<AwaitingProfile> + sml::event<SessionClosed> / drop_held =
            state<Closed>,
        state<CommittingProfile> + sml::event<SessionClosed> / drop_held =
            state<Closed>,
        state<AwaitingBang> + sml::event<SessionClosed> / drop_held =
            state<Closed>,
        state<VerifyingBang> + sml::event<SessionClosed> / drop_held =
            state<Closed>,
        state<AwaitingCryptoInstall> + sml::event<SessionClosed> / drop_held =
            state<Closed>,
        state<AwaitingStreamInfo> + sml::event<SessionClosed> / drop_held =
            state<Closed>,
        state<ValidatingStreamInfo> + sml::event<SessionClosed> / drop_held =
            state<Closed>,
        state<Ready> + sml::event<SessionClosed> / drop_held = state<Closed>,
        state<Closed> + sml::event<SessionClosed> / drop_held,
        state<Failed> + sml::event<SessionClosed> / drop_held,

        state<AwaitingConnection> + sml::event<SessionFailed> / drop_held =
            state<Failed>,
        state<AwaitingProfile> + sml::event<SessionFailed> / drop_held =
            state<Failed>,
        state<CommittingProfile> + sml::event<SessionFailed> / drop_held =
            state<Failed>,
        state<AwaitingBang> + sml::event<SessionFailed> / drop_held =
            state<Failed>,
        state<VerifyingBang> + sml::event<SessionFailed> / drop_held =
            state<Failed>,
        state<AwaitingCryptoInstall> + sml::event<SessionFailed> / drop_held =
            state<Failed>,
        state<AwaitingStreamInfo> + sml::event<SessionFailed> / drop_held =
            state<Failed>,
        state<ValidatingStreamInfo> + sml::event<SessionFailed> / drop_held =
            state<Failed>,
        state<Ready> + sml::event<SessionFailed> / drop_held = state<Failed>,
        state<Failed> + sml::event<SessionFailed> / drop_held,
        state<Closed> + sml::event<SessionFailed> / drop_held);
  }
};

/// @brief The handshake's state machine with its context.
///
/// Not thread-safe.
class Lifecycle {
 public:
  /// @brief Makes a machine waiting for the connection.
  Lifecycle() : machine_(context_) {}
  Lifecycle(const Lifecycle&) = delete;
  Lifecycle& operator=(const Lifecycle&) = delete;

  /// @brief Feeds an event, clearing the previous effect.
  ///
  /// @tparam Event One of the event types above.
  /// @param event The event.
  /// @return True when taken; false (and counted) otherwise.
  template <class Event>
  [[nodiscard]] bool process(const Event& event) {
    context_.effect.reset();
    if (machine_.process_event(event)) return true;
    ++context_.unexpected_events;
    return false;
  }

  /// @brief Takes the last event's effect.
  ///
  /// @return The effect, or nothing.
  auto take_effect() noexcept -> std::optional<Effect> {
    return std::exchange(context_.effect, std::nullopt);
  }

  /// @brief Takes the stream info held from before the ciphers.
  ///
  /// @return The stream info, or nothing.
  auto take_held_stream_info() noexcept -> std::optional<takion::StreamInfo> {
    return std::exchange(context_.held_stream_info, std::nullopt);
  }

  /// @brief The phase.
  ///
  /// @return The state.
  ActiveHandshakeState state() const noexcept {
    if (machine_.is(sml::state<AwaitingConnection>))
      return ActiveHandshakeState::awaiting_connection;
    if (machine_.is(sml::state<AwaitingProfile>) ||
        machine_.is(sml::state<CommittingProfile>))
      return ActiveHandshakeState::awaiting_profile;
    if (machine_.is(sml::state<AwaitingBang>))
      return ActiveHandshakeState::awaiting_bang;
    if (machine_.is(sml::state<VerifyingBang>))
      return ActiveHandshakeState::verifying_bang;
    if (machine_.is(sml::state<AwaitingCryptoInstall>))
      return ActiveHandshakeState::awaiting_crypto_install;
    if (machine_.is(sml::state<AwaitingStreamInfo>))
      return ActiveHandshakeState::awaiting_stream_info;
    if (machine_.is(sml::state<ValidatingStreamInfo>))
      return ActiveHandshakeState::validating_stream_info;
    if (machine_.is(sml::state<Ready>)) return ActiveHandshakeState::ready;
    if (machine_.is(sml::state<Closed>)) return ActiveHandshakeState::closed;
    return ActiveHandshakeState::failed;
  }

 private:
  /// @brief The context.
  Context context_;
  /// @brief The machine.
  sml::sm<Fsm> machine_;
};

}  // namespace
   // libchiaking::net::protocol::remote_play::active_handshake_lifecycle
