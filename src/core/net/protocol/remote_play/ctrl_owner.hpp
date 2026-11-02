/// @file
/// @brief Declares CtrlOwner, which runs the home control channel: it pumps
/// the TCP frame reader, feeds the control lifecycle, and sends the replies
/// the lifecycle's effects call for.
#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <libchiaking/platform/runtime/threading.hpp>
#include <memory>
#include <optional>
#include <span>

#include "ctrl_exchange.hpp"
#include "ctrl_lifecycle.hpp"

namespace libchiaking::net::protocol::remote_play {

/// @brief The home control channel of one session: a CtrlFrameReader driven
/// by the control lifecycle.
///
/// The owner starts an attempt, pumps inbound frames, takes effects and acts
/// on them with the run_* calls, and finally stops and runs the cleanup. A
/// send that fails for crypto or counter reasons is dropped and counted rather
/// than failing the session. Not thread-safe; every call must come from the
/// thread that made it (checked in debug builds).
class CtrlOwner {
 public:
  /// @brief Takes over a reader that has already begun on the control socket.
  ///
  /// @param reader The reader; may be null, in which case nothing starts.
  explicit CtrlOwner(std::unique_ptr<CtrlFrameReader> reader) noexcept;
  /// @brief Checks, in debug builds, that it dies on its owning thread.
  ~CtrlOwner();
  CtrlOwner(const CtrlOwner&) = delete;
  CtrlOwner& operator=(const CtrlOwner&) = delete;

  /// @brief Starts a control attempt.
  ///
  /// @param attempt_id A nonzero attempt id.
  /// @return False when there is no reader or the lifecycle refuses.
  [[nodiscard]] bool start(std::uint64_t attempt_id);
  /// @brief Feeds a decoded message to the lifecycle.
  ///
  /// @param attempt_id The attempt it arrived on.
  /// @param message The message.
  /// @return False when the lifecycle refuses it.
  [[nodiscard]] bool on_message(std::uint64_t attempt_id,
                                CtrlMessage&& message);
  /// @brief Reads at most one frame and feeds it to the lifecycle.
  ///
  /// Never blocks. A reader failure also feeds a transport Failure.
  ///
  /// @return True when a frame was handled, false when none was whole.
  /// @retval CtrlReaderError::invalid_state Not waiting for the id or active,
  ///     or the lifecycle refused the message.
  /// @retval CtrlReaderError Any reader failure.
  [[nodiscard]] auto pump_once() -> Expected<bool, CtrlReaderError>;
  /// @brief The type of the last frame read, for diagnostics.
  ///
  /// @return The type, or nothing before the first frame.
  auto last_frame_type() const noexcept -> std::optional<std::uint16_t>;
  /// @brief The display state bytes of the last frame read, for diagnostics.
  ///
  /// @return The first two payload bytes of a displaya or displayb frame, or
  ///     nothing when the last frame was another type.
  [[nodiscard]] auto last_display_state() const noexcept
      -> std::optional<std::array<std::uint8_t, 2>> {
    return last_display_state_;
  }
  /// @brief Asks the lifecycle to stop the attempt.
  ///
  /// @param attempt_id The attempt.
  /// @return False when it is not the running attempt or cannot stop now.
  [[nodiscard]] bool stop(std::uint64_t attempt_id);
  /// @brief Carries out a Cleanup effect: cancels the reader and reports
  /// Closed.
  ///
  /// @param attempt_id The attempt.
  /// @return The reader's close result.
  /// @retval CtrlReaderError::invalid_state Not closing, or another attempt.
  [[nodiscard]] auto run_cleanup(std::uint64_t attempt_id)
      -> Expected<void, CtrlReaderError>;
  /// @brief Carries out a SendHeartbeatReply effect.
  ///
  /// @param attempt_id The attempt.
  /// @return As pump_outbound().
  /// @retval CtrlReaderError::invalid_state Another attempt, or not waiting
  ///     for the id or active.
  [[nodiscard]] auto run_heartbeat_reply(std::uint64_t attempt_id)
      -> Expected<bool, CtrlReaderError>;
  /// @brief Sends an arbitrary control message on the active channel.
  ///
  /// @param attempt_id The attempt.
  /// @param type The message type.
  /// @param payload The plaintext payload.
  /// @return As pump_outbound().
  /// @retval CtrlReaderError::invalid_state Another attempt, or not active.
  /// @retval CtrlReaderError::too_large The payload does not fit.
  [[nodiscard]] auto run_control_frame(std::uint64_t attempt_id,
                                       std::uint16_t type,
                                       std::span<const std::byte> payload)
      -> Expected<bool, CtrlReaderError>;
  /// @brief Carries out a RequestPin effect with the user's passcode.
  ///
  /// @param attempt_id The attempt.
  /// @param pin The passcode as ASCII digits.
  /// @return As pump_outbound().
  /// @retval CtrlReaderError::invalid_state Another attempt, the wrong phase,
  ///     or no passcode was asked for.
  /// @retval CtrlReaderError::too_large The passcode exceeds 504 bytes.
  [[nodiscard]] auto run_pin_reply(std::uint64_t attempt_id,
                                   std::span<const std::byte> pin)
      -> Expected<bool, CtrlReaderError>;
  /// @brief Writes queued frames as far as the socket takes them.
  ///
  /// @return True when nothing remains queued (also after a dropped send),
  ///     false when bytes remain.
  /// @retval CtrlReaderError::invalid_state Nothing was queued.
  [[nodiscard]] auto pump_outbound() -> Expected<bool, CtrlReaderError>;
  /// @brief Takes the oldest effect.
  ///
  /// @return The effect, or nothing.
  [[nodiscard]] auto take_effect()
      -> std::optional<ctrl_lifecycle_detail::Effect>;
  /// @brief The lifecycle's phase.
  ///
  /// @return The state.
  auto state() const noexcept -> ctrl_lifecycle_detail::State;
  /// @brief Whether queued bytes remain to send.
  ///
  /// @return True while sending.
  auto outbound_pending() const noexcept -> bool { return outbound_pending_; }
  /// @brief How many sends were dropped.
  ///
  /// @return The count.
  auto dropped_sends() const noexcept -> std::uint64_t {
    return dropped_sends_;
  }
  /// @brief What the owner polls.
  ///
  /// @return The reader's socket, or null.
  auto pollable() const noexcept -> const platform::Pollable* {
    return reader_ ? reader_->pollable() : nullptr;
  }

 private:
  /// @brief Asserts, in debug builds, that the caller is the owning thread.
  void assert_owner() const noexcept;
  /// @brief Queues a message and starts sending it.
  ///
  /// @param type The message type.
  /// @param payload The plaintext payload.
  /// @return As pump_outbound(); true when the message was dropped.
  [[nodiscard]] auto queue_and_pump(std::uint16_t type,
                                    std::span<const std::byte> payload)
      -> Expected<bool, CtrlReaderError>;

  /// @brief The frame reader.
  std::unique_ptr<CtrlFrameReader> reader_;
  /// @brief The lifecycle's data.
  ctrl_lifecycle_detail::Context context_;
  /// @brief The lifecycle.
  ctrl_lifecycle_detail::Lifecycle lifecycle_;
  /// @brief The attempt started.
  std::uint64_t attempt_id_{};
  /// @brief Whether queued bytes remain.
  bool outbound_pending_{false};
  /// @brief Sends dropped so far.
  std::uint64_t dropped_sends_{};
  /// @brief The type of the last frame read.
  std::optional<std::uint16_t> last_frame_type_;
  /// @brief The display state bytes of the last frame read.
  std::optional<std::array<std::uint8_t, 2>> last_display_state_;
  /// @brief The thread that made the owner.
  [[maybe_unused]] const platform::ThreadIdentity owner_{
      platform::ThreadIdentity::current()};
};

}  // namespace libchiaking::net::protocol::remote_play
