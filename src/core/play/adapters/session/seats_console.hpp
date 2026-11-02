/// @file
/// @brief Defines Seats::Core::Console, the console-side couch protocol that
/// admits extra controllers to a stream.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"
#include "src/core/net/protocol/remote_play/stream/controller_delivery.hpp"
#include "src/core/net/protocol/remote_play/stream/ctrl_features.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/play/adapters/session/couch_accounts.hpp"
#include "src/core/play/adapters/session/couch_seat.hpp"
#include "src/core/play/adapters/session/seats_core.hpp"
#include "src/core/play/adapters/session/stored_pads.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::play {

/// @brief The console-side half of the couch protocol for one stream: what
/// each pad's seat is on the console, the stored controller state of each
/// pad, and the control-channel frames that join and release them.
///
/// The stream's active stream runs it on its own thread; it is not
/// synchronised. Commands arrive through Console::execute and console
/// notices through a Binding; each step may queue control frames, change the
/// active session's pads and the controller delivery, and emit stream
/// events. A PS4 joins a pad by number, a PS5 by PSN account with an
/// optional passcode, a cloud server opens pads directly. A join the console
/// does not answer in Seats::Core::join_timeout_us is abandoned.
///
/// @tparam Active The active session driven; it must offer admit_pad,
///     announce_pad, announce_pads, disconnect_pad, release_pad, pad_led and
///     set_pad_controller_type.
template <class Active>
class Seats::Core::Console {
 public:
  /// @brief The controller delivery of the active session.
  using Delivery = net::protocol::remote_play::ControllerDelivery<Active>;

  /// @brief Routes couch notices from the control channel to the console,
  /// with the delivery, session and time of one pass.
  struct Binding final : net::protocol::remote_play::CouchBackend {
    /// @brief Binds one pass.
    ///
    /// @param owner The console half; must outlive the binding.
    /// @param delivery The controller delivery; null before streaming.
    /// @param active The active session; null before streaming.
    /// @param now_us The monotonic time of the pass, in microseconds.
    Binding(Console& owner, Delivery* delivery, Active* active,
            std::uint64_t now_us) noexcept
        : owner_(owner),
          delivery_(delivery),
          active_(active),
          now_us_(now_us) {}

    /// @brief Settles a couch notice from the console.
    ///
    /// @param notice The notice.
    /// @param events Receives the events it causes.
    /// @param ctrl The control channel frames are queued on.
    void couch_notice(
        const net::protocol::remote_play::CtrlNotice& notice,
        std::vector<net::protocol::remote_play::StreamEvent>& events,
        net::protocol::remote_play::CtrlFeatures& ctrl) override {
      owner_.on_notice(notice, events, ctrl, delivery_, active_, now_us_);
    }

    /// @brief Abandons joins the console did not answer in time.
    ///
    /// @param events Receives a join failure per abandoned seat.
    /// @param ctrl The control channel frames are queued on.
    void tick(std::vector<net::protocol::remote_play::StreamEvent>& events,
              net::protocol::remote_play::CtrlFeatures& ctrl) {
      owner_.tick(events, ctrl, delivery_, active_, now_us_);
    }

   private:
    /// @brief The console half.
    Console& owner_;
    /// @brief The controller delivery; null before streaming.
    Delivery* delivery_;
    /// @brief The active session; null before streaming.
    Active* active_;
    /// @brief The monotonic time of the pass, in microseconds.
    std::uint64_t now_us_;
  };

  /// @brief Builds the console half with every pad's stored state numbered.
  ///
  /// @param profile Which couch protocol the other end speaks.
  explicit Console(ConsoleKind profile) noexcept : profile_(profile) {}

  /// @brief Reports whether a command belongs to the couch protocol rather
  /// than the control channel's own features.
  ///
  /// @param command The command.
  /// @return `true` for a couch command.
  static auto is_couch_command(
      const net::protocol::remote_play::StreamCommand& command) noexcept
      -> bool {
    return !net::protocol::remote_play::CtrlFeatures::is_ctrl_command(command);
  }

  /// @brief Reads the protocol spoken.
  ///
  /// @return The profile.
  auto profile() const noexcept -> ConsoleKind { return profile_; }

  /// @brief Reads a pad's seat on the console.
  ///
  /// @param pad The pad number.
  /// @return Its state; ConsoleSeatState::empty for a pad out of range.
  auto seat_state(std::uint8_t pad) const noexcept -> ConsoleSeatState {
    return pad < seats_.size() ? seats_[pad].state() : ConsoleSeatState::empty;
  }

  /// @brief Reads how many pads the session was told it has.
  ///
  /// @return 1 to Pad::count; 1 until set.
  auto session_pad_count() const noexcept -> std::uint8_t {
    return session_pad_count_;
  }

  /// @brief Reads a pad's stored controller state.
  ///
  /// @param pad The pad number, below Pad::count.
  /// @return The last stored state.
  auto stored_state(std::uint8_t pad) const noexcept
      -> const net::protocol::remote_play::ControllerState& {
    return stored_.at(pad);
  }

  /// @brief Stores a pad's controller state, by its controller id; an id out
  /// of range is ignored.
  ///
  /// @param state The state.
  void store(
      const net::protocol::remote_play::ControllerState& state) noexcept {
    stored_.store(state);
  }

  /// @brief Stores a state given before the stream started, once it passes
  /// validation.
  ///
  /// @param state The state.
  /// @return Nothing; StreamError::controller_rejected for an invalid state.
  [[nodiscard]] auto store_before_streaming(
      const net::protocol::remote_play::ControllerState& state)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    return stored_.store_valid(state);
  }

  /// @brief Submits pad 0's stored state as the stream begins.
  ///
  /// @param delivery The controller delivery.
  /// @param now_us The monotonic time, in microseconds.
  /// @return Nothing; StreamError::controller_rejected when the delivery
  ///     refused it.
  [[nodiscard]] auto begin_streaming(Delivery& delivery, std::uint64_t now_us)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (!delivery.submit(stored_.at(0), now_us))
      return unexpected(
          net::protocol::remote_play::StreamError::controller_rejected);
    return {};
  }

  /// @brief Runs a couch command.
  ///
  /// @param command The command.
  /// @param ctrl The control channel frames are queued on.
  /// @param delivery The controller delivery; null before streaming.
  /// @param active The active session; null before streaming.
  /// @param now_us The monotonic time, in microseconds.
  /// @return Nothing; StreamError::invalid_state for a command the profile
  ///     does not take or the seat's state forbids;
  ///     StreamError::invalid_argument for a bad pad or value; another error
  ///     when a frame could not be queued or the session refused the pad.
  [[nodiscard]] auto execute(
      const net::protocol::remote_play::StreamCommand& command,
      net::protocol::remote_play::CtrlFeatures& ctrl, Delivery* delivery,
      Active* active, std::uint64_t now_us)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    return std::visit(
        [&](const auto& value) {
          return run(value, ctrl, delivery, active, now_us);
        },
        command);
  }

  /// @brief Settles a couch notice from the console.
  ///
  /// @param notice The notice.
  /// @param events Receives the events it causes.
  /// @param ctrl The control channel frames are queued on.
  /// @param delivery The controller delivery; null before streaming.
  /// @param active The active session; null before streaming.
  /// @param now_us The monotonic time, in microseconds.
  void on_notice(const net::protocol::remote_play::CtrlNotice& notice,
                 std::vector<net::protocol::remote_play::StreamEvent>& events,
                 net::protocol::remote_play::CtrlFeatures& ctrl,
                 Delivery* delivery, Active* active, std::uint64_t now_us) {
    std::visit(
        [&](const auto& value) {
          settle(value, events, ctrl, delivery, active, now_us);
        },
        notice);
  }

  /// @brief Abandons joins the console did not answer in time, emitting a
  /// join failure with SeatRefusal::timed_out_status for each.
  ///
  /// @param events Receives the failures.
  /// @param ctrl The control channel frames are queued on.
  /// @param delivery The controller delivery; null before streaming.
  /// @param active The active session; null before streaming.
  /// @param now_us The monotonic time, in microseconds.
  void tick(std::vector<net::protocol::remote_play::StreamEvent>& events,
            net::protocol::remote_play::CtrlFeatures& ctrl, Delivery* delivery,
            Active* active, std::uint64_t now_us) {
    for (std::uint8_t pad = 1; pad < seats_.size(); ++pad) {
      if (!seats_[pad].timed_out(now_us)) continue;
      abandon(pad, ctrl, delivery, active, now_us);
      events.emplace_back(refused(pad, SeatRefusal::timed_out_status));
    }
  }

 private:
  /// @brief The join failure status sent when the session could not admit an
  /// accepted pad.
  static constexpr std::uint8_t not_accepted_status = 1;

  /// @brief Computes when a join sent now times out.
  ///
  /// @param now_us The monotonic time, in microseconds.
  /// @return The deadline, in microseconds.
  static constexpr auto join_deadline(std::uint64_t now_us) noexcept
      -> std::uint64_t {
    return now_us + join_timeout_us;
  }

  /// @brief Builds a join failure event.
  ///
  /// @param pad The pad number.
  /// @param status The console's status, or one of the library's own.
  /// @return The event.
  static auto refused(std::uint8_t pad, std::uint8_t status) noexcept
      -> net::protocol::remote_play::event::PadJoinFailed {
    return net::protocol::remote_play::event::PadJoinFailed{pad, status};
  }

  /// @brief Reports whether a pad number is an extra pad the couch protocol
  /// handles.
  ///
  /// @param pad The pad number.
  /// @return `true` from 1 below Pad::count.
  static auto ctrl_pad(std::uint8_t pad) noexcept -> bool {
    return pad != 0 && pad < net::protocol::remote_play::Pad::count;
  }

  /// @brief Reports whether the other end is a console.
  ///
  /// @return `false` for a cloud stream.
  auto remote_play() const noexcept -> bool {
    return profile_ != ConsoleKind::cloud;
  }

  /// @brief Reports whether the stream is streaming.
  ///
  /// @param delivery The controller delivery.
  /// @param active The active session.
  /// @return `true` when both are present.
  static auto streaming(Delivery* delivery, Active* active) noexcept -> bool {
    return delivery && active;
  }

  /// @brief Queues an encoded control frame.
  ///
  /// @param ctrl The control channel.
  /// @param frame The encoded frame, or the encoder's error.
  /// @return Nothing; StreamError::invalid_argument when encoding failed; the
  ///     channel's error when it could not queue.
  [[nodiscard]] static auto queue(
      net::protocol::remote_play::CtrlFeatures& ctrl,
      Expected<net::protocol::remote_play::CtrlOutboundFrame,
               net::protocol::remote_play::CtrlFeatureCodecError> frame)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (!frame)
      return unexpected(
          net::protocol::remote_play::StreamError::invalid_argument);
    return ctrl.queue_frame(std::move(frame.value()));
  }

  /// @brief Turns a success flag into a result.
  ///
  /// @param ok The flag.
  /// @return Nothing; StreamError::pipeline_failed when @p ok is `false`.
  [[nodiscard]] static auto stream_result(bool ok)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (!ok)
      return unexpected(
          net::protocol::remote_play::StreamError::pipeline_failed);
    return {};
  }

  /// @brief Builds the media unit that confirms a pad with its light colour.
  ///
  /// @param pad The pad number.
  /// @param led The light colour, red, green and blue.
  /// @return The unit.
  static auto pad_confirmed(std::uint8_t pad,
                            const std::array<std::uint8_t, 3>& led)
      -> net::protocol::remote_play::event::EncodedMedia {
    return net::protocol::remote_play::event::EncodedMedia{
        net::protocol::remote_play::EncodedMediaKind::pad_confirmed,
        {std::byte{pad}, std::byte{led[0]}, std::byte{led[1]},
         std::byte{led[2]}}};
  }

  /// @brief Empties a pad's seat.
  ///
  /// @param pad The pad number.
  void reset_seat(std::uint8_t pad) noexcept { seats_[pad] = CouchSeat{}; }

  /// @brief Stops a pad's input in the delivery and the session, when present.
  ///
  /// @param pad The pad number.
  /// @param delivery The controller delivery; may be null.
  /// @param active The active session; may be null.
  void drop_pad(std::uint8_t pad, Delivery* delivery, Active* active) {
    if (delivery) static_cast<void>(delivery->deactivate(pad));
    if (active) active->release_pad(pad);
  }

  /// @brief Gives up a pad: leaves cleanly while streaming, otherwise drops it
  /// and queues a leave; the seat empties either way.
  ///
  /// @param pad The pad number.
  /// @param ctrl The control channel.
  /// @param delivery The controller delivery; may be null.
  /// @param active The active session; may be null.
  /// @param now_us The monotonic time, in microseconds.
  void abandon(std::uint8_t pad, net::protocol::remote_play::CtrlFeatures& ctrl,
               Delivery* delivery, Active* active, std::uint64_t now_us) {
    if (!streaming(delivery, active) ||
        !leave(pad, ctrl, delivery, active, now_us)) {
      drop_pad(pad, delivery, active);
      static_cast<void>(queue(
          ctrl,
          net::protocol::remote_play::CtrlCodec::encode_couch_leave(pad)));
    }
    reset_seat(pad);
  }

  /// @brief Ignores a notice the couch protocol does not handle.
  ///
  /// @tparam Notice The notice's type.
  template <class Notice>
  void settle(const Notice&,
              std::vector<net::protocol::remote_play::StreamEvent>&,
              net::protocol::remote_play::CtrlFeatures&, Delivery*, Active*,
              std::uint64_t) {}

  /// @brief Settles a PS5's answer to a user join: a passcode demand, a
  /// refusal, or acceptance, which admits the pad and announces it with its
  /// light colour.
  ///
  /// @param value The answer.
  /// @param events Receives the events it causes.
  /// @param ctrl The control channel.
  /// @param delivery The controller delivery; may be null.
  /// @param active The active session; may be null.
  /// @param now_us The monotonic time, in microseconds.
  void settle(const net::protocol::remote_play::CtrlCouchUserJoinReply& value,
              std::vector<net::protocol::remote_play::StreamEvent>& events,
              net::protocol::remote_play::CtrlFeatures& ctrl,
              Delivery* delivery, Active* active, std::uint64_t now_us) {
    if (!ctrl_pad(value.pad) || profile_ != ConsoleKind::remote_play_ps5 ||
        !seats_[value.pad].awaiting_console())
      return;
    if (value.status ==
        net::protocol::remote_play::couch_user_join_status_passcode_required) {
      demand_passcode(value.pad, events, ctrl, delivery, active, now_us);
      return;
    }
    if (value.status != 0) {
      abandon(value.pad, ctrl, delivery, active, now_us);
      events.emplace_back(refused(value.pad, value.status));
      return;
    }
    if (!accept_pad(value.pad, delivery, active, now_us)) {
      abandon(value.pad, ctrl, delivery, active, now_us);
      events.emplace_back(refused(value.pad, not_accepted_status));
      return;
    }
    seats_[value.pad].admit();
    const auto led = active->pad_led(value.pad);
    events.emplace_back(pad_confirmed(value.pad, led));
    events.emplace_back(
        net::protocol::remote_play::event::PadAnnounced{value.pad, led});
  }

  /// @brief Settles a PS5's request for a passcode.
  ///
  /// @param value The request.
  /// @param events Receives the events it causes.
  /// @param ctrl The control channel.
  /// @param delivery The controller delivery; may be null.
  /// @param active The active session; may be null.
  /// @param now_us The monotonic time, in microseconds.
  void settle(const net::protocol::remote_play::CtrlCouchPasscodeRequest& value,
              std::vector<net::protocol::remote_play::StreamEvent>& events,
              net::protocol::remote_play::CtrlFeatures& ctrl,
              Delivery* delivery, Active* active, std::uint64_t now_us) {
    if (!ctrl_pad(value.pad) || profile_ != ConsoleKind::remote_play_ps5)
      return;
    demand_passcode(value.pad, events, ctrl, delivery, active, now_us);
  }

  /// @brief Settles a PS4's answer to a pad join: a refusal abandons the seat,
  /// acceptance announces the pad.
  ///
  /// @param value The answer.
  /// @param events Receives the events it causes.
  /// @param ctrl The control channel.
  /// @param delivery The controller delivery; may be null.
  /// @param active The active session; may be null.
  /// @param now_us The monotonic time, in microseconds.
  void settle(const net::protocol::remote_play::CtrlCouchPadJoinReply& value,
              std::vector<net::protocol::remote_play::StreamEvent>& events,
              net::protocol::remote_play::CtrlFeatures& ctrl,
              Delivery* delivery, Active* active, std::uint64_t now_us) {
    if (!ctrl_pad(value.pad) || profile_ != ConsoleKind::remote_play_ps4 ||
        seats_[value.pad].state() != ConsoleSeatState::join_requested)
      return;
    if (value.result != 0) {
      abandon(value.pad, ctrl, delivery, active, now_us);
      events.emplace_back(refused(value.pad, value.result));
      return;
    }
    events.emplace_back(net::protocol::remote_play::event::PadAnnounced{
        value.pad,
        active ? active->pad_led(value.pad) : std::array<std::uint8_t, 3>{}});
  }

  /// @brief Settles a console dropping a pad: the pad stops, the seat empties,
  /// a drop event is emitted when it was held, and a leave is queued.
  ///
  /// @param value The notice.
  /// @param events Receives the events it causes.
  /// @param ctrl The control channel.
  /// @param delivery The controller delivery; may be null.
  /// @param active The active session; may be null.
  void settle(const net::protocol::remote_play::CtrlCouchPadDropped& value,
              std::vector<net::protocol::remote_play::StreamEvent>& events,
              net::protocol::remote_play::CtrlFeatures& ctrl,
              Delivery* delivery, Active* active, std::uint64_t) {
    if (!ctrl_pad(value.pad) || !remote_play()) return;
    const bool held = seats_[value.pad].state() != ConsoleSeatState::empty;
    drop_pad(value.pad, delivery, active);
    reset_seat(value.pad);
    if (held)
      events.emplace_back(
          net::protocol::remote_play::event::PadDropped{value.pad});
    static_cast<void>(queue(
        ctrl,
        net::protocol::remote_play::CtrlCodec::encode_couch_leave(value.pad)));
  }

  /// @brief Settles a PS5 saying it cannot resolve a pad's account: the
  /// account is remembered as unresolvable and the seat abandoned.
  ///
  /// @param value The notice.
  /// @param events Receives the events it causes.
  /// @param ctrl The control channel.
  /// @param delivery The controller delivery; may be null.
  /// @param active The active session; may be null.
  /// @param now_us The monotonic time, in microseconds.
  void settle(
      const net::protocol::remote_play::CtrlCouchAccountUnresolvable& value,
      std::vector<net::protocol::remote_play::StreamEvent>& events,
      net::protocol::remote_play::CtrlFeatures& ctrl, Delivery* delivery,
      Active* active, std::uint64_t now_us) {
    if (!ctrl_pad(value.pad) || profile_ != ConsoleKind::remote_play_ps5)
      return;
    const auto& seat = seats_[value.pad];
    if (!seat.awaiting_console() || seat.account().empty()) return;
    accounts_.unresolvable(seat.account());
    abandon(value.pad, ctrl, delivery, active, now_us);
    events.emplace_back(
        net::protocol::remote_play::event::PadAccountUnresolvable{value.pad});
  }

  /// @brief Asks the player for a seat's passcode, or abandons the seat when
  /// its attempts are used.
  ///
  /// @param pad The pad number.
  /// @param events Receives the request, flagged as a retry after the first
  ///     attempt, or the failure.
  /// @param ctrl The control channel.
  /// @param delivery The controller delivery; may be null.
  /// @param active The active session; may be null.
  /// @param now_us The monotonic time, in microseconds.
  void demand_passcode(
      std::uint8_t pad,
      std::vector<net::protocol::remote_play::StreamEvent>& events,
      net::protocol::remote_play::CtrlFeatures& ctrl, Delivery* delivery,
      Active* active, std::uint64_t now_us) {
    auto& seat = seats_[pad];
    if (seat.state() != ConsoleSeatState::join_requested) return;
    if (seat.attempts_used()) {
      abandon(pad, ctrl, delivery, active, now_us);
      events.emplace_back(refused(pad, SeatRefusal::passcode_attempts_status));
      return;
    }
    seat.ask_passcode();
    events.emplace_back(net::protocol::remote_play::event::PadPasscodeRequest{
        pad, seat.retrying()});
  }

  /// @brief Admits an accepted pad to the session and activates its delivery.
  ///
  /// @param pad The pad number.
  /// @param delivery The controller delivery.
  /// @param active The active session.
  /// @param now_us The monotonic time, in microseconds.
  /// @return Nothing; StreamError::invalid_state before streaming;
  ///     StreamError::pipeline_failed when it could not be announced;
  ///     StreamError::controller_rejected when the delivery refused it.
  [[nodiscard]] auto accept_pad(std::uint8_t pad, Delivery* delivery,
                                Active* active, std::uint64_t now_us)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (!streaming(delivery, active))
      return unexpected(net::protocol::remote_play::StreamError::invalid_state);
    active->admit_pad(pad, false);
    if (!active->announce_pad(pad, true))
      return unexpected(
          net::protocol::remote_play::StreamError::pipeline_failed);
    if (!delivery->activate(pad, stored_.at(pad), now_us))
      return unexpected(
          net::protocol::remote_play::StreamError::controller_rejected);
    return {};
  }

  /// @brief Refuses rest mode; the control channel sends it, not the couch.
  ///
  /// @return StreamError::invalid_state.
  [[nodiscard]] auto run(const net::protocol::remote_play::command::GotoBed&,
                         net::protocol::remote_play::CtrlFeatures&, Delivery*,
                         Active*, std::uint64_t)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    return unexpected(net::protocol::remote_play::StreamError::invalid_state);
  }
  /// @brief Refuses going home; the control channel sends it, not the couch.
  ///
  /// @return StreamError::invalid_state.
  [[nodiscard]] auto run(const net::protocol::remote_play::command::GoHome&,
                         net::protocol::remote_play::CtrlFeatures&, Delivery*,
                         Active*, std::uint64_t)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    return unexpected(net::protocol::remote_play::StreamError::invalid_state);
  }
  /// @brief Refuses keyboard text; the control channel sends it, not the
  /// couch.
  ///
  /// @return StreamError::invalid_state.
  [[nodiscard]] auto run(
      const net::protocol::remote_play::command::KeyboardSetText&,
      net::protocol::remote_play::CtrlFeatures&, Delivery*, Active*,
      std::uint64_t)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    return unexpected(net::protocol::remote_play::StreamError::invalid_state);
  }
  /// @brief Refuses keyboard acceptance; the control channel sends it, not the
  /// couch.
  ///
  /// @return StreamError::invalid_state.
  [[nodiscard]] auto run(
      const net::protocol::remote_play::command::KeyboardAccept&,
      net::protocol::remote_play::CtrlFeatures&, Delivery*, Active*,
      std::uint64_t)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    return unexpected(net::protocol::remote_play::StreamError::invalid_state);
  }
  /// @brief Refuses keyboard dismissal; the control channel sends it, not the
  /// couch.
  ///
  /// @return StreamError::invalid_state.
  [[nodiscard]] auto run(
      const net::protocol::remote_play::command::KeyboardReject&,
      net::protocol::remote_play::CtrlFeatures&, Delivery*, Active*,
      std::uint64_t)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    return unexpected(net::protocol::remote_play::StreamError::invalid_state);
  }

  /// @brief Sets how many pads the session has.
  ///
  /// @param value The count.
  /// @return Nothing; StreamError::invalid_argument outside 1 to Pad::count.
  [[nodiscard]] auto run(
      const net::protocol::remote_play::command::SetPadCount& value,
      net::protocol::remote_play::CtrlFeatures&, Delivery*, Active*,
      std::uint64_t)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (value.pad_count < 1 ||
        value.pad_count > net::protocol::remote_play::Pad::count)
      return unexpected(
          net::protocol::remote_play::StreamError::invalid_argument);
    session_pad_count_ = value.pad_count;
    return {};
  }

  /// @brief Remembers the PSN account a pad will join as, base64-encoded.
  ///
  /// @param value The pad and account.
  /// @return Nothing; StreamError::invalid_argument for a bad pad.
  [[nodiscard]] auto run(
      const net::protocol::remote_play::command::CouchSetAccountId& value,
      net::protocol::remote_play::CtrlFeatures&, Delivery*, Active*,
      std::uint64_t)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (!ctrl_pad(value.pad))
      return unexpected(
          net::protocol::remote_play::StreamError::invalid_argument);
    accounts_.remember(value.pad, value.account_id);
    return {};
  }

  /// @brief Sends a PS5 user join for a pad.
  ///
  /// @param value The pad.
  /// @param ctrl The control channel.
  /// @param delivery The controller delivery.
  /// @param active The active session.
  /// @param now_us The monotonic time, in microseconds.
  /// @return As the join; StreamError::invalid_state off a PS5.
  [[nodiscard]] auto run(
      const net::protocol::remote_play::command::CouchUserJoin& value,
      net::protocol::remote_play::CtrlFeatures& ctrl, Delivery* delivery,
      Active* active, std::uint64_t now_us)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (profile_ != ConsoleKind::remote_play_ps5)
      return unexpected(net::protocol::remote_play::StreamError::invalid_state);
    return join(value.pad, 0, ctrl, delivery, active, now_us);
  }

  /// @brief Sends a PS4 pad join and marks the seat requested.
  ///
  /// @param value The pad and controller type.
  /// @param ctrl The control channel.
  /// @return Nothing; StreamError::invalid_state off a PS4;
  ///     StreamError::invalid_argument for a bad pad; the channel's error.
  [[nodiscard]] auto run(
      const net::protocol::remote_play::command::CouchPadJoin& value,
      net::protocol::remote_play::CtrlFeatures& ctrl, Delivery*, Active*,
      std::uint64_t)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (profile_ != ConsoleKind::remote_play_ps4)
      return unexpected(net::protocol::remote_play::StreamError::invalid_state);
    if (!ctrl_pad(value.pad))
      return unexpected(
          net::protocol::remote_play::StreamError::invalid_argument);
    auto queued = queue(
        ctrl, net::protocol::remote_play::CtrlCodec::encode_couch_pad_join(
                  value.pad, value.controller_type));
    if (queued && seats_[value.pad].state() == ConsoleSeatState::empty)
      seats_[value.pad].request(std::nullopt);
    return queued;
  }

  /// @brief Sends a leave for a pad and empties its seat.
  ///
  /// @param value The pad.
  /// @param ctrl The control channel.
  /// @param delivery The controller delivery; may be null.
  /// @param active The active session; may be null.
  /// @return Nothing; StreamError::invalid_state for a cloud stream;
  ///     StreamError::invalid_argument for a bad pad; the channel's error.
  [[nodiscard]] auto run(
      const net::protocol::remote_play::command::CouchLeave& value,
      net::protocol::remote_play::CtrlFeatures& ctrl, Delivery* delivery,
      Active* active, std::uint64_t)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (!remote_play())
      return unexpected(net::protocol::remote_play::StreamError::invalid_state);
    if (!ctrl_pad(value.pad))
      return unexpected(
          net::protocol::remote_play::StreamError::invalid_argument);
    auto queued = queue(
        ctrl,
        net::protocol::remote_play::CtrlCodec::encode_couch_leave(value.pad));
    if (queued) {
      drop_pad(value.pad, delivery, active);
      reset_seat(value.pad);
    }
    return queued;
  }

  /// @brief Sends a PS5 seat's passcode and restarts its join timer.
  ///
  /// @param value The pad and passcode.
  /// @param ctrl The control channel.
  /// @param now_us The monotonic time, in microseconds.
  /// @return Nothing; StreamError::invalid_state off a PS5 or when no passcode
  ///     was asked for; StreamError::invalid_argument for a malformed passcode
  ///     or pad; the channel's error.
  [[nodiscard]] auto run(
      const net::protocol::remote_play::command::CouchPasscode& value,
      net::protocol::remote_play::CtrlFeatures& ctrl, Delivery*, Active*,
      std::uint64_t now_us)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (profile_ != ConsoleKind::remote_play_ps5)
      return unexpected(net::protocol::remote_play::StreamError::invalid_state);
    auto frame = net::protocol::remote_play::CtrlCodec::encode_couch_passcode(
        value.pad, value.passcode.text());
    if (!frame)
      return unexpected(
          net::protocol::remote_play::StreamError::invalid_argument);
    auto& seat = seats_[value.pad];
    if (seat.state() != ConsoleSeatState::entering_passcode)
      return unexpected(net::protocol::remote_play::StreamError::invalid_state);
    auto queued = ctrl.queue_frame(std::move(frame.value()));
    if (!queued) return queued;
    seat.retry(join_deadline(now_us));
    return {};
  }

  /// @brief Pushes a pad's presence to the delivery.
  ///
  /// @param value The pad and presence.
  /// @param delivery The controller delivery.
  /// @param now_us The monotonic time, in microseconds.
  /// @return As Console::push_presence.
  [[nodiscard]] auto run(
      const net::protocol::remote_play::command::CouchPresence& value,
      net::protocol::remote_play::CtrlFeatures&, Delivery* delivery, Active*,
      std::uint64_t now_us)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    return push_presence(value.pad, value.present, delivery, now_us);
  }

  /// @brief Pushes a pad's presence to the delivery.
  ///
  /// @param value The pad and presence.
  /// @param delivery The controller delivery.
  /// @param now_us The monotonic time, in microseconds.
  /// @return As Console::push_presence.
  [[nodiscard]] auto run(
      const net::protocol::remote_play::command::CouchPushPresence& value,
      net::protocol::remote_play::CtrlFeatures&, Delivery* delivery, Active*,
      std::uint64_t now_us)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    return push_presence(value.pad, value.present, delivery, now_us);
  }

  /// @brief Announces the session's pad count to the active session.
  ///
  /// @param active The active session.
  /// @return Nothing; StreamError::invalid_state before streaming;
  ///     StreamError::pipeline_failed when the announcement failed.
  [[nodiscard]] auto run(
      const net::protocol::remote_play::command::CouchAnnouncePads&,
      net::protocol::remote_play::CtrlFeatures&, Delivery*, Active* active,
      std::uint64_t)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (!active)
      return unexpected(net::protocol::remote_play::StreamError::invalid_state);
    return stream_result(
        static_cast<bool>(active->announce_pads(session_pad_count_)));
  }

  /// @brief Opens a pad directly and marks its seat joined.
  ///
  /// @param value The pad and controller type.
  /// @param delivery The controller delivery.
  /// @param active The active session.
  /// @param now_us The monotonic time, in microseconds.
  /// @return As Console::open_pad.
  [[nodiscard]] auto run(
      const net::protocol::remote_play::command::CouchOpenPad& value,
      net::protocol::remote_play::CtrlFeatures&, Delivery* delivery,
      Active* active, std::uint64_t now_us)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    auto opened =
        open_pad(value.pad, value.controller_type, delivery, active, now_us);
    if (opened) seats_[value.pad].admit();
    return opened;
  }

  /// @brief Admits a PS5 pad the player accepted and marks its seat joined.
  ///
  /// @param value The pad.
  /// @param delivery The controller delivery.
  /// @param active The active session.
  /// @param now_us The monotonic time, in microseconds.
  /// @return Nothing; StreamError::invalid_state off a PS5;
  ///     StreamError::invalid_argument for a bad pad; as Console::accept_pad.
  [[nodiscard]] auto run(
      const net::protocol::remote_play::command::CouchAcceptPad& value,
      net::protocol::remote_play::CtrlFeatures&, Delivery* delivery,
      Active* active, std::uint64_t now_us)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (profile_ != ConsoleKind::remote_play_ps5)
      return unexpected(net::protocol::remote_play::StreamError::invalid_state);
    if (!ctrl_pad(value.pad))
      return unexpected(
          net::protocol::remote_play::StreamError::invalid_argument);
    auto accepted = accept_pad(value.pad, delivery, active, now_us);
    if (accepted) {
      seats_[value.pad].admit();
    }
    return accepted;
  }

  /// @brief Closes a pad and empties its seat.
  ///
  /// @param value The pad.
  /// @param delivery The controller delivery.
  /// @param active The active session.
  /// @return As Console::close_pad.
  [[nodiscard]] auto run(
      const net::protocol::remote_play::command::CouchClosePad& value,
      net::protocol::remote_play::CtrlFeatures&, Delivery* delivery,
      Active* active, std::uint64_t)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    auto closed = close_pad(value.pad, delivery, active);
    if (closed) reset_seat(value.pad);
    return closed;
  }

  /// @brief Joins a pad to a PS4 by number.
  ///
  /// @param value The pad and controller type.
  /// @param ctrl The control channel.
  /// @param delivery The controller delivery.
  /// @param active The active session.
  /// @param now_us The monotonic time, in microseconds.
  /// @return As the join; StreamError::invalid_state off a PS4.
  [[nodiscard]] auto run(
      const net::protocol::remote_play::command::CouchJoinPs4& value,
      net::protocol::remote_play::CtrlFeatures& ctrl, Delivery* delivery,
      Active* active, std::uint64_t now_us)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (profile_ != ConsoleKind::remote_play_ps4)
      return unexpected(net::protocol::remote_play::StreamError::invalid_state);
    return join(value.pad, value.controller_type, ctrl, delivery, active,
                now_us);
  }

  /// @brief Removes a pad from a console.
  ///
  /// @param value The pad.
  /// @param ctrl The control channel.
  /// @param delivery The controller delivery.
  /// @param active The active session.
  /// @param now_us The monotonic time, in microseconds.
  /// @return As the removal; StreamError::invalid_state for a cloud stream.
  [[nodiscard]] auto run(
      const net::protocol::remote_play::command::CouchLeavePs4& value,
      net::protocol::remote_play::CtrlFeatures& ctrl, Delivery* delivery,
      Active* active, std::uint64_t now_us)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (!remote_play())
      return unexpected(net::protocol::remote_play::StreamError::invalid_state);
    return remove(value.pad, ctrl, delivery, active, now_us);
  }

  /// @brief Joins a pad the way the profile needs.
  ///
  /// @param value The pad and controller type.
  /// @param ctrl The control channel.
  /// @param delivery The controller delivery.
  /// @param active The active session.
  /// @param now_us The monotonic time, in microseconds.
  /// @return As the join.
  [[nodiscard]] auto run(
      const net::protocol::remote_play::command::CouchJoin& value,
      net::protocol::remote_play::CtrlFeatures& ctrl, Delivery* delivery,
      Active* active, std::uint64_t now_us)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    return join(value.pad, value.controller_type, ctrl, delivery, active,
                now_us);
  }

  /// @brief Removes a pad the way the profile needs.
  ///
  /// @param value The pad.
  /// @param ctrl The control channel.
  /// @param delivery The controller delivery.
  /// @param active The active session.
  /// @param now_us The monotonic time, in microseconds.
  /// @return As the removal.
  [[nodiscard]] auto run(
      const net::protocol::remote_play::command::CouchRemove& value,
      net::protocol::remote_play::CtrlFeatures& ctrl, Delivery* delivery,
      Active* active, std::uint64_t now_us)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    return remove(value.pad, ctrl, delivery, active, now_us);
  }

  /// @brief Joins a pad: a PS4 admits it at once and asks the console, a PS5
  /// sends a user join and waits, a cloud server opens it.
  ///
  /// @param pad The pad number.
  /// @param controller_type The wire controller type; 0 to keep the default.
  /// @param ctrl The control channel.
  /// @param delivery The controller delivery.
  /// @param active The active session.
  /// @param now_us The monotonic time, in microseconds.
  /// @return Nothing; StreamError::invalid_argument for a bad pad;
  ///     StreamError::invalid_state for a held seat; the profile's error.
  [[nodiscard]] auto join(std::uint8_t pad, std::uint8_t controller_type,
                          net::protocol::remote_play::CtrlFeatures& ctrl,
                          Delivery* delivery, Active* active,
                          std::uint64_t now_us)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (!ctrl_pad(pad))
      return unexpected(
          net::protocol::remote_play::StreamError::invalid_argument);
    if (seats_[pad].state() != ConsoleSeatState::empty)
      return unexpected(net::protocol::remote_play::StreamError::invalid_state);
    switch (profile_) {
      case ConsoleKind::remote_play_ps4: {
        auto joined =
            join_ps4(pad, controller_type, ctrl, delivery, active, now_us);
        if (joined) seats_[pad].request(std::nullopt);
        return joined;
      }
      case ConsoleKind::remote_play_ps5:
        return user_join(pad, controller_type, ctrl, active, now_us);
      case ConsoleKind::cloud: {
        auto opened = open_pad(pad, controller_type, delivery, active, now_us);
        if (opened) seats_[pad].admit();
        return opened;
      }
    }
    return unexpected(net::protocol::remote_play::StreamError::invalid_state);
  }

  /// @brief Removes a pad; nothing to do for an empty seat.
  ///
  /// On a cloud stream the pad is closed; on a console the seat is abandoned,
  /// which needs room on the control channel.
  ///
  /// @param pad The pad number.
  /// @param ctrl The control channel.
  /// @param delivery The controller delivery.
  /// @param active The active session.
  /// @param now_us The monotonic time, in microseconds.
  /// @return Nothing; StreamError::invalid_argument for a bad pad;
  ///     StreamError::invalid_state when the channel is full; the close's
  ///     error.
  [[nodiscard]] auto remove(std::uint8_t pad,
                            net::protocol::remote_play::CtrlFeatures& ctrl,
                            Delivery* delivery, Active* active,
                            std::uint64_t now_us)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (!ctrl_pad(pad))
      return unexpected(
          net::protocol::remote_play::StreamError::invalid_argument);
    if (seats_[pad].state() == ConsoleSeatState::empty) return {};
    if (!remote_play()) {
      if (!streaming(delivery, active)) {
        reset_seat(pad);
        return {};
      }
      auto closed = close_pad(pad, delivery, active);
      reset_seat(pad);
      return closed;
    }
    if (!ctrl.can_queue(1))
      return unexpected(net::protocol::remote_play::StreamError::invalid_state);
    abandon(pad, ctrl, delivery, active, now_us);
    return {};
  }

  /// @brief Sends a PS5 user join as the pad's remembered account and starts
  /// the join timer.
  ///
  /// @param pad The pad number.
  /// @param controller_type The wire controller type; 0 to keep the default.
  /// @param ctrl The control channel.
  /// @param active The active session; may be null.
  /// @param now_us The monotonic time, in microseconds.
  /// @return Nothing; StreamError::invalid_argument without an account, for an
  ///     account the console could not resolve, or one already seated; the
  ///     channel's error.
  [[nodiscard]] auto user_join(std::uint8_t pad, std::uint8_t controller_type,
                               net::protocol::remote_play::CtrlFeatures& ctrl,
                               Active* active, std::uint64_t now_us)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (!accounts_.joinable(pad))
      return unexpected(
          net::protocol::remote_play::StreamError::invalid_argument);
    const auto& account = accounts_.account(pad);
    for (std::uint8_t other = 1; other < seats_.size(); ++other)
      if (other != pad && seats_[other].state() != ConsoleSeatState::empty &&
          seats_[other].account() == account)
        return unexpected(
            net::protocol::remote_play::StreamError::invalid_argument);
    auto queued = queue(
        ctrl, net::protocol::remote_play::CtrlCodec::encode_couch_user_join(
                  pad, account));
    if (!queued) return queued;
    if (controller_type && active)
      active->set_pad_controller_type(pad, controller_type);
    seats_[pad].request_as(account, join_deadline(now_us));
    return {};
  }

  /// @brief Pushes a pad's presence to the delivery.
  ///
  /// @param pad The pad number.
  /// @param present Whether the pad is present.
  /// @param delivery The controller delivery.
  /// @param now_us The monotonic time, in microseconds.
  /// @return Nothing; StreamError::invalid_argument for a bad pad;
  ///     StreamError::invalid_state without a delivery;
  ///     StreamError::pipeline_failed when it refused.
  [[nodiscard]] auto push_presence(std::uint8_t pad, bool present,
                                   Delivery* delivery, std::uint64_t now_us)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (!ctrl_pad(pad))
      return unexpected(
          net::protocol::remote_play::StreamError::invalid_argument);
    if (!delivery)
      return unexpected(net::protocol::remote_play::StreamError::invalid_state);
    if (!delivery->push_presence(pad, present, now_us))
      return unexpected(
          net::protocol::remote_play::StreamError::pipeline_failed);
    return {};
  }

  /// @brief Admits a pad to a PS4 stream, marks it present and sends the pad
  /// join.
  ///
  /// @param pad The pad number.
  /// @param controller_type The wire controller type; 0 to keep the default.
  /// @param ctrl The control channel.
  /// @param delivery The controller delivery.
  /// @param active The active session.
  /// @param now_us The monotonic time, in microseconds.
  /// @return Nothing; StreamError::invalid_state before streaming or with a
  ///     full channel; StreamError::pipeline_failed or
  ///     StreamError::controller_rejected when the pad could not be admitted;
  ///     the channel's error.
  [[nodiscard]] auto join_ps4(std::uint8_t pad, std::uint8_t controller_type,
                              net::protocol::remote_play::CtrlFeatures& ctrl,
                              Delivery* delivery, Active* active,
                              std::uint64_t now_us)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (!streaming(delivery, active) || !ctrl.can_queue(1))
      return unexpected(net::protocol::remote_play::StreamError::invalid_state);
    if (controller_type) active->set_pad_controller_type(pad, controller_type);
    active->admit_pad(pad, true);
    if (!active->announce_pad(pad, true))
      return unexpected(
          net::protocol::remote_play::StreamError::pipeline_failed);
    if (!delivery->activate(pad, stored_.at(pad), now_us))
      return unexpected(
          net::protocol::remote_play::StreamError::controller_rejected);
    if (auto present = push_presence(pad, true, delivery, now_us); !present)
      return present;
    return queue(
        ctrl,
        net::protocol::remote_play::CtrlCodec::encode_couch_pad_join(pad, 0));
  }

  /// @brief Leaves a pad cleanly: disconnects it, marks it absent, sends a
  /// leave and drops it.
  ///
  /// @param pad The pad number.
  /// @param ctrl The control channel.
  /// @param delivery The controller delivery.
  /// @param active The active session; must not be null.
  /// @param now_us The monotonic time, in microseconds.
  /// @return `true` when every step worked; `false` at the first that failed.
  [[nodiscard]] auto leave(std::uint8_t pad,
                           net::protocol::remote_play::CtrlFeatures& ctrl,
                           Delivery* delivery, Active* active,
                           std::uint64_t now_us) -> bool {
    if (!active->disconnect_pad(pad)) return false;
    if (!push_presence(pad, false, delivery, now_us)) return false;
    if (!queue(ctrl,
               net::protocol::remote_play::CtrlCodec::encode_couch_leave(pad)))
      return false;
    drop_pad(pad, delivery, active);
    return true;
  }

  /// @brief Opens a pad without asking the other end.
  ///
  /// @param pad The pad number.
  /// @param controller_type The wire controller type; 0 to keep the default.
  /// @param delivery The controller delivery.
  /// @param active The active session.
  /// @param now_us The monotonic time, in microseconds.
  /// @return Nothing; StreamError::invalid_argument for a bad pad;
  ///     StreamError::invalid_state before streaming;
  ///     StreamError::pipeline_failed or StreamError::controller_rejected when
  ///     it could not be admitted.
  [[nodiscard]] auto open_pad(std::uint8_t pad, std::uint8_t controller_type,
                              Delivery* delivery, Active* active,
                              std::uint64_t now_us)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (!ctrl_pad(pad))
      return unexpected(
          net::protocol::remote_play::StreamError::invalid_argument);
    if (!streaming(delivery, active))
      return unexpected(net::protocol::remote_play::StreamError::invalid_state);
    if (controller_type) active->set_pad_controller_type(pad, controller_type);
    if (!active->announce_pad(pad, true))
      return unexpected(
          net::protocol::remote_play::StreamError::pipeline_failed);
    active->admit_pad(pad, true);
    if (!delivery->activate(pad, stored_.at(pad), now_us))
      return unexpected(
          net::protocol::remote_play::StreamError::controller_rejected);
    return {};
  }

  /// @brief Closes a pad: drops it and disconnects it from the session.
  ///
  /// @param pad The pad number.
  /// @param delivery The controller delivery.
  /// @param active The active session.
  /// @return Nothing; StreamError::invalid_argument for a bad pad;
  ///     StreamError::invalid_state before streaming;
  ///     StreamError::pipeline_failed when it could not be disconnected.
  [[nodiscard]] auto close_pad(std::uint8_t pad, Delivery* delivery,
                               Active* active)
      -> Expected<void, net::protocol::remote_play::StreamError> {
    if (!ctrl_pad(pad))
      return unexpected(
          net::protocol::remote_play::StreamError::invalid_argument);
    if (!streaming(delivery, active))
      return unexpected(net::protocol::remote_play::StreamError::invalid_state);
    drop_pad(pad, delivery, active);
    return stream_result(static_cast<bool>(active->disconnect_pad(pad)));
  }

  /// @brief Which couch protocol the other end speaks.
  ConsoleKind profile_;
  /// @brief How many pads the session has; 1 until set.
  std::uint8_t session_pad_count_{1};
  /// @brief The PSN account each pad will join as, and the accounts the
  /// console could not resolve.
  CouchAccounts accounts_;
  /// @brief The last controller state stored per pad.
  StoredPads stored_;
  /// @brief Each pad's seat on the console.
  std::array<CouchSeat, net::protocol::remote_play::Pad::count> seats_{};
};

}  // namespace libchiaking::play
