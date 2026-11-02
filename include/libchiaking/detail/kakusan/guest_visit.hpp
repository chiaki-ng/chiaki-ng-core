/// @file
/// @brief Declares kakusan::GuestVisit, the guest's side of a shared
/// session, and kakusan::VisitOutcome.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/kakusan/invitation.hpp"
#include "libchiaking/detail/kakusan/remembered_devices.hpp"
#include "libchiaking/detail/kakusan/values.hpp"
#include "libchiaking/detail/play/session_share.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace libchiaking::kakusan {

class Kakusan;
class Surroundings;
class Connections;
class PumpGate;

/// @brief How a GuestVisit ended, returned by GuestVisit::run.
/// @ingroup api_kakusan
class VisitOutcome {
 public:
  /// @brief The broad way the visit ended.
  enum class Status : std::uint8_t {
    /// @brief The visit ran and ended without a failure, for example
    /// because the guest left or the host stopped sharing.
    completed,
    /// @brief GuestVisit::cancel was called, or GuestVisit::run was called
    /// a second time.
    cancelled,
    /// @brief The visit failed; refusal() says why.
    visit_failed,
    /// @brief Receiving or presenting the stream failed; media_fault() says
    /// how.
    media_failed,
  };

  /// @brief The value of code() for Status::cancelled.
  static constexpr int cancelled_code = 2;
  /// @brief Added to the media::MediaFailure value to form code() for
  /// Status::media_failed.
  static constexpr int media_base = 4800;
  /// @brief Added to Refusal::code to form code() for Status::visit_failed.
  static constexpr int refusal_base = 5000;

  /// @brief The broad way the visit ended.
  /// @return The status; refusal() and media_fault() carry the detail for
  ///     Status::visit_failed and Status::media_failed.
  auto status() const noexcept -> Status { return status_; }
  /// @brief A single number for the outcome, for logs and exit codes.
  /// @return 0 for Status::completed, cancelled_code for Status::cancelled,
  ///     media_base plus the media failure kind for Status::media_failed, and
  ///     refusal_base plus Refusal::code for Status::visit_failed.
  auto code() const noexcept -> int;
  /// @brief Tells whether the visit ended without failure or cancellation.
  /// @return True only for Status::completed.
  auto succeeded() const noexcept -> bool {
    return status_ == Status::completed;
  }
  /// @brief Why the visit failed.
  /// @return The refusal for Status::visit_failed; empty otherwise.
  [[nodiscard]] auto refusal() const noexcept -> std::optional<Refusal> {
    return refusal_;
  }
  /// @brief What went wrong with the stream.
  /// @return The fault for Status::media_failed; empty otherwise.
  [[nodiscard]] auto media_fault() const noexcept
      -> const std::optional<media::MediaFault>& {
    return media_;
  }
  /// @brief A short English sentence describing a failure, for logs.
  /// @return Empty for Status::completed and Status::cancelled. The view is
  ///     valid while this VisitOutcome lives.
  auto message() const noexcept -> std::string_view { return message_; }

 private:
  friend class GuestVisit;

  VisitOutcome(Status status, std::optional<Refusal> refusal,
               std::optional<media::MediaFault> media);

  static auto describe(Status status, std::optional<Refusal> refusal,
                       const std::optional<media::MediaFault>& media)
      -> std::string;

  Status status_;
  std::optional<Refusal> refusal_;
  std::optional<media::MediaFault> media_;
  std::string message_;
};

/// @brief One guest's visit to a host's shared session, from accepted
/// invitation to departure.
///
/// Kakusan::join creates the visit from an Invitation. The guest then calls
/// run(), which blocks and drives everything: it produces the Reply to send
/// back to the host, connects, exchanges verification codes, waits for a seat,
/// and receives the host's stream into the media::MediaSink while send()
/// carries the guest's controller to the host.
///
/// The order of events a frontend sees is ReplyReady, then VisitChanged as
/// Progress advances, CodeToReadOut during Progress::verifying, StreamReady and
/// Connected once playing, and finally Disconnected.
///
/// Every event is delivered on the thread that called run(), from inside run().
/// The other members may be called from any thread while run() is in progress.
///
/// @warning Do not destroy the visit while run() is executing. Call leave() or
///     cancel() and wait for run() to return first.
/// @ingroup api_kakusan
class GuestVisit {
 public:
  /// @brief Internal protocol state of the visit; not part of the public
  /// interface.
  class Core;
  /// @brief Internal runtime state of the visit; not part of the public
  /// interface.
  class Visiting;

  /// @brief The reply to the host's invitation is ready to be sent back.
  ///
  /// The frontend must get Reply::text to the host by the same means the
  /// invitation arrived; the host passes it to SharedSession::accept_reply.
  struct ReplyReady {
    /// @brief The reply to deliver to the host.
    Reply reply;
  };
  /// @brief The host has proven which device it is, over the connection the
  /// two devices just made.
  ///
  /// Arrives before the visit reaches Progress::verifying, so the frontend
  /// can show who the codes are about to be exchanged with.
  struct HostIdentified {
    /// @brief The host device's public key.
    IdentityKey key{};
    /// @brief The fingerprint of key, for showing to the user.
    Fingerprint fingerprint{};
    /// @brief The host's seats as the host reports them.
    SeatSummary seats;
    /// @brief Whether the host is a device this one remembers.
    Recognition recognition{};
  };
  /// @brief This device's verification code is ready to be read out to the
  /// host.
  ///
  /// The guest tells the host this code and enters the host's code with
  /// GuestVisit::verify.
  struct CodeToReadOut {
    /// @brief The four digits to read out.
    VerificationCode code;
  };
  /// @brief The visit moved to a new stage, or ended.
  struct VisitChanged {
    /// @brief The stage the visit is now in.
    Progress progress{Progress::waiting};
    /// @brief Why the visit failed, when it ended in failure.
    std::optional<Refusal> refusal{};
    /// @brief Why the visit ended, when an ending was announced.
    std::optional<EndReason> ended{};
    /// @brief True when the ending was started by the host's side, false
    /// when by this device.
    bool by_them{};
    /// @brief The seat the host granted, once there is one.
    std::optional<SeatGrant> grant{};
  };
  /// @brief The host's stream is set up and media is about to reach the
  /// media::MediaSink.
  struct StreamReady {
    /// @brief The frame rate of the host's video.
    std::uint32_t frames_per_second{};
  };
  /// @brief The host's stream stopped reaching this guest.
  struct StreamStopped {};
  /// @brief The visit reached Progress::playing for the first time.
  struct Connected {};
  /// @brief The visit reached Progress::gone; no more events follow and run()
  /// is about to return.
  struct Disconnected {};

  /// @brief Everything a GuestVisit can report to its observer.
  using Event =
      std::variant<ReplyReady, HostIdentified, CodeToReadOut, VisitChanged,
                   StreamReady, StreamStopped, Connected, Disconnected>;

  /// @brief Receives the events of one visit; implemented by the frontend.
  class Observer {
   public:
    /// @brief Destroys the observer through its interface.
    virtual ~Observer() = default;
    /// @brief Receives one event.
    ///
    /// Called on the thread that is executing GuestVisit::run, with no
    /// GuestVisit lock held, so the handler may call verify(), send(),
    /// leave() or cancel().
    ///
    /// @param event The event; std::visit over it to handle each kind.
    virtual void on_event(Event event) = 0;
  };

  /// @brief Lets a frontend supply the way GuestVisit::run sleeps between
  /// turns.
  ///
  /// Only needed to tie the loop into a frontend's own scheduling. When
  /// Ports::pause is null the visit uses an internal condition variable.
  class Pause {
   public:
    /// @brief Destroys the pause through its interface.
    virtual ~Pause() = default;
    /// @brief Blocks the run() thread until wake() is called or the limit
    /// passes.
    /// @param limit The longest time to block.
    virtual void wait(std::chrono::milliseconds limit) = 0;
    /// @brief Ends a wait() in progress, or makes the next one return at
    /// once.
    ///
    /// Called from any thread, including from inside GuestVisit members.
    virtual void wake() noexcept = 0;
  };

  /// @brief What the frontend plugs into a visit. All of it must outlive the
  /// GuestVisit.
  struct Ports {
    /// @brief Receives the visit's events.
    Observer& observer;
    /// @brief Receives the host's video and audio, and rumble for the
    /// guest's controller as controllers::ControllerFeedback. The rumble is
    /// already scaled by the haptic strength set on the host's console.
    media::MediaSink& media;
    /// @brief Optional replacement for the internal sleep; null to use the
    /// built-in one.
    Pause* pause{};
  };

  /// @brief Timing of the run() loop. The defaults suit a live stream.
  struct Pacing {
    /// @brief Longest sleep between turns before the host has admitted this
    /// guest.
    std::chrono::milliseconds idle_wait{8};
    /// @brief Longest sleep between turns once the guest is admitted, when
    /// media has to keep flowing.
    std::chrono::milliseconds admitted_wait{2};
    /// @brief How long run() keeps going after leave() or cancel() to let
    /// the host acknowledge the departure before giving up.
    std::chrono::milliseconds leave_deadline{1500};
  };

  /// @brief Longest diagnostic note, in characters, that a visit writes to
  /// the log; longer notes are dropped.
  static constexpr std::size_t diagnostic_limit = 512;

  /// @brief Wraps the internal state of a visit. Frontends obtain a visit
  /// from Kakusan::join instead.
  /// @param visiting The assembled visit.
  explicit GuestVisit(std::unique_ptr<Visiting> visiting) noexcept;
  /// @brief Releases the visit and its connection to the host.
  /// @pre run() is not executing.
  ~GuestVisit();
  /// @brief Not copyable; a visit is one connection with one owner.
  GuestVisit(const GuestVisit&) = delete;
  /// @brief Not copy-assignable; a visit is one connection with one owner.
  auto operator=(const GuestVisit&) -> GuestVisit& = delete;

  /// @brief What the invitation says about the host and the session.
  ///
  /// Takes no lock; safe from any thread.
  ///
  /// @return The summary read when the visit was created, including whether
  ///     the host is a remembered device. It does not change during the
  ///     visit.
  auto host() const noexcept -> const InvitationSummary&;
  /// @brief The host device's public key, once the host has proven it.
  ///
  /// Takes the visit's lock briefly; safe from any thread, including from
  /// inside Observer::on_event.
  ///
  /// @return The key reported in HostIdentified, or nothing before that.
  auto host_key() const -> std::optional<IdentityKey>;
  /// @brief Enters the verification code the host read out.
  ///
  /// The two sides then prove to each other that the codes match. The result
  /// arrives as VisitChanged: Progress::waiting_for_seat on success, or
  /// Progress::gone with a refusal. Only one code can be entered per visit.
  ///
  /// Only queues: it takes the visit's lock, records the code and wakes
  /// run(), which does the checking. May be called from any thread,
  /// including from inside Observer::on_event.
  ///
  /// @param theirs The host's code.
  /// @return Nothing once the code is accepted for checking.
  /// @retval Refusal::Kind::not_visiting The visit is leaving or has ended.
  /// @retval Refusal::Kind::internal The visit is not at Progress::verifying,
  ///     or a code was already entered.
  /// @retval Refusal::Kind::verification_failed The code is not four ASCII
  ///     digits, which only a code built from unchecked characters can be.
  [[nodiscard]] auto verify(const VerificationCode& theirs)
      -> Expected<void, Refusal>;
  /// @brief Remembers the host's device under a nickname so later invitations
  /// from it are recognised.
  ///
  /// Blocks while the DeviceStore saves the record, and while it removes
  /// the device that held the nickname when replace is Replace::yes. Takes
  /// the visit's lock, then the RememberedDevices lock. May be called from
  /// any thread, including from inside Observer::on_event.
  ///
  /// @param nickname The name to file the host under; see
  ///     RememberedDevices::normalised for what is accepted.
  /// @param replace Replace::yes to take the nickname from another device
  ///     that holds it, which forgets that device.
  /// @return The remembered device as saved.
  /// @retval RememberRefusal::Kind::not_verified The host has not been
  ///     verified in this visit yet, or the visit has reached
  ///     Progress::gone.
  /// @retval RememberRefusal::Kind::invalid_nickname The nickname is not
  ///     acceptable.
  /// @retval RememberRefusal::Kind::nickname_taken Another device holds the
  ///     nickname and replace is Replace::no; RememberRefusal::holder names
  ///     it.
  /// @retval RememberRefusal::Kind::store_unavailable The DeviceStore failed;
  ///     a device that held the nickname is still remembered.
  [[nodiscard]] auto remember_host(std::string_view nickname,
                                   Replace replace = Replace::no)
      -> Expected<RememberedDevice, RememberRefusal>;
  /// @brief Sends the guest's current controller state towards the host's
  /// console.
  ///
  /// The state is only queued; the run() loop transmits it. The host drops
  /// presses of buttons its SharingRules do not allow. Takes the visit's
  /// lock and wakes run(); may be called from any thread, including from
  /// inside Observer::on_event.
  ///
  /// @param state The whole controller state as it is now.
  /// @return Nothing once the state is queued.
  /// @retval Refusal::Kind::not_visiting The visit is leaving or has ended.
  /// @retval Refusal::Kind::internal The connection to the host is already
  ///     leaving or gone, before run() has noticed.
  /// @retval Refusal::Kind::protocol The state holds values the protocol
  ///     cannot carry.
  [[nodiscard]] auto send(const controllers::ControllerState& state)
      -> Expected<void, Refusal>;
  /// @brief Runs the visit to its end on the calling thread.
  ///
  /// Blocks, sleeping between turns as Pacing directs, delivering every event
  /// from this thread. It returns when the visit reaches Progress::gone, or
  /// when Pacing::leave_deadline passes after leave() or cancel(). A media
  /// fault makes the visit leave on its own.
  ///
  /// @return How the visit ended. When several apply, a media fault is
  ///     reported in preference to cancellation, and cancellation in
  ///     preference to a refusal.
  /// @note A visit runs once. A second call returns Status::cancelled at once
  ///     without doing anything, so calling it from inside Observer::on_event
  ///     does nothing useful. A call after cancel() also returns
  ///     Status::cancelled without connecting.
  [[nodiscard]] auto run() -> VisitOutcome;
  /// @brief Tells the host this guest is leaving and winds the visit down.
  ///
  /// Only starts the departure; run() returns once the host acknowledges it
  /// or Pacing::leave_deadline passes, and VisitChanged then Disconnected
  /// report the end. Calling it again while leaving succeeds and changes
  /// nothing. Takes the visit's lock and wakes run(); may be called from any
  /// thread, including from inside Observer::on_event.
  ///
  /// @return Nothing once leaving has begun.
  /// @retval Refusal::Kind::not_visiting The visit has already ended.
  [[nodiscard]] auto leave() -> Expected<void, Refusal>;
  /// @brief Abandons the visit from any thread.
  ///
  /// Wakes run(), which then leaves as leave() does and returns
  /// Status::cancelled. Safe to call before run(), during it, or after it.
  /// Takes no visit lock, so it is also safe from inside Observer::on_event.
  void cancel() noexcept;

  /// @brief Reports that the frontend has presented a video frame, as
  /// play::Session::video_presented does for an ordinary session.
  ///
  /// Takes no visit lock; may be called from the frontend's render thread.
  ///
  /// @return Frames lost since the last call and whether the stream has
  ///     recovered; empty when the visit was cancelled or no stream is being
  ///     received, so the frame should not be shown.
  auto video_presented() noexcept -> std::optional<media::PresentationLoss>;
  /// @brief Reports that the frontend's decoder rejected a frame, and wakes
  /// run() to react.
  /// @param frame The index of the rejected frame.
  /// @param cause The decoder's own error code, kept for diagnostics.
  void video_rejected(std::uint16_t frame, int cause);
  /// @brief Reports that the frontend can no longer output video, and wakes
  /// run(). A media fault recorded this way ends the visit with
  /// VisitOutcome::Status::media_failed.
  /// @param cause The frontend's own error code, kept for diagnostics.
  void video_output_failed(int cause);
  /// @brief Video statistics for the stream being received.
  ///
  /// Takes a stream lock briefly; safe from any thread.
  ///
  /// @return The live figures, or the last ones recorded if the stream has
  ///     stopped.
  auto video_statistics() const -> media::VideoStatistics;

 private:
  std::unique_ptr<Visiting> visiting_;
};

/// @brief Short name for GuestVisit::Event.
using GuestVisitEvent = GuestVisit::Event;
/// @brief Short name for GuestVisit::Observer.
using GuestVisitObserver = GuestVisit::Observer;

}  // namespace libchiaking::kakusan
