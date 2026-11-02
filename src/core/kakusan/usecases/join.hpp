/// @file
/// @brief Declares usecases::Join, the use case of visiting a host as a guest:
/// the guest's commands, the run() loop and the delivery of events to the
/// frontend.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/kakusan/guest_visit.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "src/core/kakusan/adapters/guest_visit/guest_visit_core.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace libchiaking::kakusan {

class Surroundings;

namespace usecases {

/// @brief Carries out a guest visit.
///
/// Commands take the visit's lock, tell the protocol core, translate what it
/// produced into queued steps and wake the run() loop. run() is the visit's
/// thread: each turn it runs a pump turn through the PumpGate, delivers the
/// queued steps with no lock held (events to the observer, rumble to the
/// media sink, log lines), then sleeps on the pause until the core's next
/// timer, a command's wake, or the pacing cap. A cancel or a media fault turns
/// into a departure, which run() follows until the visit is gone or
/// Pacing::leave_deadline passes.
///
/// Owned by GuestVisit::Visiting. Commands may be called from any thread,
/// including the observer's; run() must be called once, on a thread of the
/// frontend's choosing. Neither copyable nor movable.
class Join {
 public:
  /// @brief Step: set the guest's rumble on the media sink.
  struct Shake {
    /// @brief Low-frequency motor strength, 0 to 65535.
    std::uint16_t low_frequency{};
    /// @brief High-frequency motor strength, 0 to 65535.
    std::uint16_t high_frequency{};
  };
  /// @brief Step: write a line to the log.
  struct Note {
    /// @brief The text, without the "Kakusan" prefix.
    std::string text;
    /// @brief How serious it is.
    platform::LogSeverity severity{platform::LogSeverity::info};
  };

  /// @brief Anything delivered after a pump turn.
  using Step = std::variant<GuestVisit::Event, Shake, Note>;

  /// @brief Binds the use case to its visiting.
  /// @param visiting The visiting that owns this object.
  /// @param surroundings The Kakusan context.
  /// @param observer Receives the events; must outlive the visiting.
  /// @param media Receives the stream and rumble; must outlive the visiting.
  /// @param pause What run() sleeps on.
  /// @param pacing Timing of the run() loop.
  Join(GuestVisit::Visiting& visiting, Surroundings& surroundings,
       GuestVisit::Observer& observer, media::MediaSink& media,
       GuestVisit::Pause& pause, GuestVisit::Pacing pacing) noexcept;
  /// @brief Not copyable.
  Join(const Join&) = delete;
  /// @brief Not copy-assignable.
  auto operator=(const Join&) -> Join& = delete;

  /// @brief Accepts an invitation and prepares the visit, as Kakusan::join
  /// does.
  ///
  /// Opens the transport source (resolving STUN servers, which blocks),
  /// builds the core, accepts the invitation, recognises the host's hint
  /// among the remembered devices, and assembles the visit. Each refusal is
  /// logged.
  ///
  /// @param surroundings The Kakusan context.
  /// @param invitation The invitation read with Kakusan::read.
  /// @param ports The observer, media sink and optional pause.
  /// @param pacing Timing of the run() loop.
  /// @return The visit, ready to run.
  /// @retval Refusal Any failure of building the core or accepting the
  ///     invitation (invalid, expired, no randomness, no link).
  [[nodiscard]] static auto open(std::shared_ptr<Surroundings> surroundings,
                                 const Invitation& invitation,
                                 GuestVisit::Ports ports,
                                 GuestVisit::Pacing pacing)
      -> Expected<std::unique_ptr<GuestVisit>, Refusal>;

  /// @brief Enters the host's code; see GuestVisit::verify.
  /// @param theirs The host's code.
  /// @return Nothing once taken.
  /// @retval Refusal::Kind::not_visiting The visit is no longer open.
  /// @retval Refusal Any failure of GuestVisit::Core::enter_code.
  [[nodiscard]] auto verify(const VerificationCode& theirs)
      -> Expected<void, Refusal>;
  /// @brief Takes the guest's controller state; see GuestVisit::send.
  /// @param state The state.
  /// @return Nothing once taken.
  /// @retval Refusal::Kind::not_visiting The visit is no longer open.
  /// @retval Refusal Any failure of GuestVisit::Core::set_controller.
  [[nodiscard]] auto send(const controllers::ControllerState& state)
      -> Expected<void, Refusal>;
  /// @brief Runs the visit until it ends; see GuestVisit::run.
  ///
  /// Blocks the calling thread for the whole visit. A second call, or a call
  /// after cancel() before the first turn, returns
  /// VisitOutcome::Status::cancelled at once.
  ///
  /// @return media_failed when the stream recorded a fault, cancelled after
  ///     cancel(), visit_failed with the refusal when the visit failed, and
  ///     completed otherwise.
  [[nodiscard]] auto run() -> VisitOutcome;
  /// @brief Begins leaving; see GuestVisit::leave. Calling it again while
  /// leaving succeeds and does nothing.
  /// @return Nothing once the departure has begun.
  /// @retval Refusal::Kind::not_visiting The visit is no longer open.
  [[nodiscard]] auto leave() -> Expected<void, Refusal>;
  /// @brief Asks run() to leave at its next turn and wakes it. Safe from any
  /// thread, without a lock.
  void cancel() noexcept;

 private:
  /// @brief What one turn of the loop decided.
  struct Turn {
    /// @brief Whether run() should stop.
    bool finished{true};
    /// @brief How long to sleep before the next turn.
    std::chrono::milliseconds wait{};
  };

  /// @brief Narrows a 16-bit motor strength to the media sink's 8 bits.
  /// @param value The strength, 0 to 65535.
  /// @return The strength, 0 to 255, rounded.
  static auto rumble_byte(std::uint16_t value) noexcept -> std::uint8_t;

  /// @brief Runs a pump turn through the gate and delivers its steps.
  void pump();
  /// @brief Translates every output of the core into queued steps and picks
  /// up the forwarded stream. Call with the lock held.
  void drain();
  /// @brief Queues ReplyReady.
  /// @param ready The core's output; its text is taken.
  void translate(GuestVisit::Core::ReplyReady& ready);
  /// @brief Records the host's key and queues HostIdentified with its
  /// recognition; takes the RememberedDevices lock.
  /// @param host The core's output.
  void translate(const GuestVisit::Core::HostIdentified& host);
  /// @brief Queues CodeToReadOut.
  /// @param code The core's output.
  void translate(const GuestVisit::Core::CodeReady& code);
  /// @brief Records the progress and queues VisitChanged, followed by
  /// Connected or Disconnected when due.
  /// @param state The core's output.
  void translate(const GuestVisit::Core::State& state);
  /// @brief Queues StreamReady.
  /// @param stream The core's output.
  void translate(const GuestVisit::Core::StreamReady& stream);
  /// @brief Queues StreamStopped.
  /// @param stopped The core's output.
  void translate(const GuestVisit::Core::StreamStopped& stopped);
  /// @brief Readies the guest's controller at once; a refusal is logged as a
  /// warning.
  /// @param requested The core's output.
  void translate(const GuestVisit::Core::ControllerRequested& requested);
  /// @brief Queues a Shake.
  /// @param rumble The core's output.
  void translate(const GuestVisit::Core::Rumble& rumble);
  /// @brief Queues a Note when the text is no longer than
  /// GuestVisit::diagnostic_limit.
  /// @param diagnostic The core's output; its text is taken.
  void translate(GuestVisit::Core::Diagnostic& diagnostic);
  /// @brief Hands an event to the observer; logs progress, and marks a
  /// remembered host as seen once verified (which blocks while the store
  /// saves).
  /// @param event The event.
  void deliver(GuestVisit::Event event);
  /// @brief Sends the rumble to the media sink as feedback for the first
  /// seat.
  /// @param shake The step.
  void deliver(const Shake& shake);
  /// @brief Writes the note to the log.
  /// @param note The step.
  void deliver(const Note& note);
  /// @brief Runs one turn of the loop.
  /// @param leave True to begin leaving first, when still open.
  /// @return Whether the visit is over (closed, or the departure timed out),
  ///     and otherwise how long to sleep: zero when work is waiting, else
  ///     until the core's next timer, capped by the pacing and the leave
  ///     deadline.
  auto service(bool leave) -> Turn;
  /// @brief Tells the core to leave and starts the leave deadline. Call with
  /// the lock held.
  void begin_leave();
  /// @brief Writes a log line.
  /// @param severity How serious it is.
  /// @param message The text.
  void log(platform::LogSeverity severity,
           std::string_view message) const noexcept;

  /// @brief The visiting that owns this object.
  GuestVisit::Visiting& visiting_;
  /// @brief The Kakusan context.
  Surroundings& surroundings_;
  /// @brief Receives the events.
  GuestVisit::Observer& observer_;
  /// @brief Receives the rumble as feedback.
  media::MediaSink& media_;
  /// @brief What run() sleeps on.
  GuestVisit::Pause& pause_;
  /// @brief Timing of the run() loop.
  GuestVisit::Pacing pacing_;
};

}  // namespace usecases
}  // namespace libchiaking::kakusan
