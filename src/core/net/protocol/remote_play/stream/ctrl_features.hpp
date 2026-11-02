/// @file
/// @brief Declares CtrlFeatures, which turns control channel notices into
/// stream events and queues the control messages a running stream sends:
/// rest mode, home screen, on-screen keyboard and couch seat messages.
#pragma once

#include "libchiaking/platform/diagnostics/provider.hpp"
#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"
#include "src/core/net/protocol/remote_play/stream/limits.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::net::protocol::remote_play {

class CtrlFeatures;

/// @brief Where couch seat notices go: the play context's seat bookkeeping.
class CouchBackend {
 public:
  /// @brief Destroys the backend.
  virtual ~CouchBackend() = default;
  /// @brief Handles a couch seat notice.
  ///
  /// @param notice The notice: account unresolvable, passcode request, user or
  ///     pad join reply, or pad dropped.
  /// @param events Where stream events are appended.
  /// @param ctrl The control features, for queueing replies.
  virtual void couch_notice(const CtrlNotice& notice,
                            std::vector<StreamEvent>& events,
                            CtrlFeatures& ctrl) = 0;
};

/// @brief The control channel's part in a running stream.
///
/// Holds a bounded queue of outbound control messages that the link sends
/// when the channel can take them, and tracks the two protected-content
/// notices so the picture is reported hidden while either holds. Not
/// thread-safe.
class CtrlFeatures {
 public:
  /// @brief The most outbound messages queued.
  static constexpr std::size_t outbound_capacity = queue_capacity;

  /// @brief Makes the features.
  ///
  /// @param diagnostics Where to log, or null.
  explicit CtrlFeatures(
      platform::DiagnosticsProvider* diagnostics = nullptr) noexcept;

  /// @brief Whether a command is sent on the control channel: rest mode, home
  /// screen or a keyboard command.
  ///
  /// @param command The command.
  /// @return True for those.
  static auto is_ctrl_command(const StreamCommand& command) noexcept -> bool;

  /// @brief Queues the control message of a command.
  ///
  /// Keyboard text carries the next edit number, from 1.
  ///
  /// @param command The command.
  /// @return Nothing on success.
  /// @retval StreamError::invalid_state The queue is full, or the command is
  ///     not a control command.
  /// @retval StreamError::invalid_argument The keyboard text is too long.
  [[nodiscard]] auto queue_command(const StreamCommand& command)
      -> Expected<void, StreamError>;

  /// @brief Queues an encoded message.
  ///
  /// @param frame The message.
  /// @return Nothing, or StreamError::invalid_state when full.
  [[nodiscard]] auto queue_frame(CtrlOutboundFrame frame)
      -> Expected<void, StreamError>;

  /// @brief Whether a number of messages fit.
  ///
  /// @param frames How many.
  /// @return True when they fit.
  auto can_queue(std::size_t frames) const noexcept -> bool;

  /// @brief The next message to send.
  ///
  /// @return The message, or null when none waits.
  auto front_outbound() const noexcept -> const CtrlOutboundFrame*;

  /// @brief Removes the next message once sent.
  void pop_outbound() noexcept;

  /// @brief Empties the queue.
  void clear_outbound() noexcept;

  /// @brief Reports the control reply's `RP-Prohibit`.
  ///
  /// @param prohibited Whether the console prohibits the picture.
  /// @param events Where a DisplayProhibited event is appended when it does.
  void on_rp_prohibit(bool prohibited, std::vector<StreamEvent>& events);

  /// @brief Turns a control notice into stream events, passing couch notices
  /// on.
  ///
  /// @param notice The notice.
  /// @param events Where events are appended.
  /// @param couch Where couch notices go, or null to drop them.
  void on_notice(CtrlNotice notice, std::vector<StreamEvent>& events,
                 CouchBackend* couch = nullptr);

 private:
  /// @brief Appends a message when there is room.
  ///
  /// @param frame The message.
  /// @return Nothing, or StreamError::invalid_state when full.
  [[nodiscard]] auto queue(CtrlOutboundFrame frame)
      -> Expected<void, StreamError>;

  /// @brief Reports the opened keyboard.
  ///
  /// @param value The notice.
  /// @param events Where events go.
  void handle(CtrlKeyboardOpen& value, std::vector<StreamEvent>& events);

  /// @brief Reports the keyboard's new text.
  ///
  /// @param value The notice.
  /// @param events Where events go.
  void handle(CtrlKeyboardTextChange& value, std::vector<StreamEvent>& events);

  /// @brief Reports the keyboard closed by the console.
  ///
  /// @param events Where events go.
  void handle(CtrlKeyboardRemoteClose&, std::vector<StreamEvent>& events);

  /// @brief Tracks the first protected-content notice; reports the picture
  /// shown again once neither notice holds.
  ///
  /// @param value The notice.
  /// @param events Where events go.
  void handle(CtrlDisplayA& value, std::vector<StreamEvent>& events);

  /// @brief Tracks the second protected-content notice; reports the picture
  /// hidden when it follows the first.
  ///
  /// @param value The notice.
  /// @param events Where events go.
  void handle(CtrlDisplayB& value, std::vector<StreamEvent>& events);

  /// @brief Logs and forwards an unresolvable couch account.
  ///
  /// @param value The notice.
  /// @param events Where events go.
  void handle(CtrlCouchAccountUnresolvable& value,
              std::vector<StreamEvent>& events);

  /// @brief Logs and forwards a couch user join reply.
  ///
  /// @param value The notice.
  /// @param events Where events go.
  void handle(CtrlCouchUserJoinReply& value, std::vector<StreamEvent>& events);

  /// @brief Logs and forwards a couch passcode request.
  ///
  /// @param value The notice.
  /// @param events Where events go.
  void handle(CtrlCouchPasscodeRequest& value,
              std::vector<StreamEvent>& events);

  /// @brief Logs and forwards a couch pad join reply.
  ///
  /// @param value The notice.
  /// @param events Where events go.
  void handle(CtrlCouchPadJoinReply& value, std::vector<StreamEvent>& events);

  /// @brief Logs and forwards a dropped couch seat.
  ///
  /// @param value The notice.
  /// @param events Where events go.
  void handle(CtrlCouchPadDropped& value, std::vector<StreamEvent>& events);

  /// @brief Logs a couch leave reply; nothing is forwarded.
  ///
  /// @param value The notice.
  void handle(CtrlCouchLeaveReply& value, std::vector<StreamEvent>&);

  /// @brief Passes a couch notice to the backend, when one was given.
  ///
  /// @tparam Notice A couch notice type.
  /// @param notice The notice.
  /// @param events Where events go.
  template <class Notice>
  void forward(Notice notice, std::vector<StreamEvent>& events) {
    if (couch_)
      couch_->couch_notice(CtrlNotice{std::move(notice)}, events, *this);
  }

  /// @brief Logs a malformed notice.
  ///
  /// @param value The notice.
  void handle(CtrlMalformedNotice& value, std::vector<StreamEvent>&);

  /// @brief Writes a log line.
  ///
  /// @param severity The severity.
  /// @param message The line.
  void log(platform::LogSeverity severity, std::string_view message) noexcept;

  /// @brief Writes a formatted log line.
  ///
  /// @tparam Args The argument types.
  /// @param severity The severity.
  /// @param format The std::format format, checked at compile time.
  /// @param args The arguments.
  template <class... Args>
  void logf(platform::LogSeverity severity,
            std::format_string<const Args&...> format, const Args&... args) {
    if (diagnostics_) log(severity, std::format(format, args...));
  }

  /// @brief Where to log.
  platform::DiagnosticsProvider* diagnostics_{};
  /// @brief Messages waiting to be sent.
  std::deque<CtrlOutboundFrame> outbound_;
  /// @brief The couch backend during on_notice().
  CouchBackend* couch_{};
  /// @brief The last keyboard edit number sent.
  std::uint32_t keyboard_counter_{};
  /// @brief Whether the first protected-content notice holds.
  bool cant_display_a_{};
  /// @brief Whether the second protected-content notice holds.
  bool cant_display_b_{};
};

}  // namespace libchiaking::net::protocol::remote_play
