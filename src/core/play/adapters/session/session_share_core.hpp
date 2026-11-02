/// @file
/// @brief Declares SessionShare::Core, which lets invited players watch and
/// play a live session by forwarding its encrypted stream, and the cores of
/// the Sharing and SharedSeat handles it gives out.
#pragma once

#include "libchiaking/detail/media/media_sink.hpp"
#include "libchiaking/detail/play/session.hpp"
#include "libchiaking/detail/play/session_share.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/net/protocol/takion/source_tap.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace libchiaking::play {

/// @brief The implementation behind the public SessionShare: one audience
/// that receives the session's encrypted datagrams, receive keys, stream
/// description and controller feedback, and guests seated in its spare
/// seats.
///
/// It sits between the link and the frontend twice: as the Takion source tap
/// it sees every datagram and the receive keys, and as the media sink it sees
/// the stream description and feedback before passing them on. Once a stream
/// is described with a Takion version that can be forwarded, the audience is
/// told the stream and keys, then every datagram. Only console sessions can
/// be shared, by one audience at a time. Shared by the session and the handles
/// through std::shared_ptr; the handles hold it weakly. Two mutexes guard the
/// operations and the state, taken in that order; deliveries run on the
/// link's threads and read the audience through an atomic pointer. Not
/// copyable.
class SessionShare::Core final
    : public std::enable_shared_from_this<SessionShare::Core>,
      private net::protocol::takion::SourceTap,
      private media::MediaSink {
 public:
  /// @brief What the share needs to know of its session. Plain data.
  struct Source {
    /// @brief Where the session runs.
    Where where{Where::home};
    /// @brief The frame rate asked for, used when the console grants none.
    std::uint32_t requested_fps{};
  };

  /// @brief Builds a share.
  ///
  /// @param threading Provider of the share's mutexes and condition
  ///     variable; must outlive the share.
  /// @param clock Monotonic clock the share's condition variable measures
  ///     on; must outlive the share.
  /// @param source Where the session runs and its asked frame rate.
  /// @return The share, owned through std::shared_ptr.
  static auto create(platform::ThreadingProvider& threading,
                     const platform::ClockProvider& clock, Source source)
      -> std::shared_ptr<Core>;

  /// @brief Releases the share.
  ~Core() override;
  /// @brief Not copyable; handles refer to this share.
  Core(const Core&) = delete;
  /// @brief Not copy-assignable; handles refer to this share.
  auto operator=(const Core&) -> Core& = delete;

  /// @brief Gives the public face.
  ///
  /// @return The face, owned by the share.
  auto face() noexcept -> SessionShare& { return face_; }
  /// @brief Gives the Takion source tap the link reports through.
  ///
  /// @return This share, seen as the tap.
  auto tap() noexcept -> net::protocol::takion::SourceTap& { return *this; }
  /// @brief Puts the share in front of the frontend's sink.
  ///
  /// @param next The frontend's sink; must outlive the share's use.
  /// @return This share, seen as the sink the media path delivers to.
  auto watch(media::MediaSink& next) noexcept -> media::MediaSink&;
  /// @brief Binds the session the share forwards for.
  ///
  /// @param session The session; must stay valid until
  /// SessionShare::Core::close.
  void bind(Session::Core& session);
  /// @brief Unbinds the session and ends sharing.
  void close();

  /// @brief Tells the audience the stream when it was not yet told, or that it
  /// cannot be shared, and lets it do its own work; called by the session
  /// loop.
  ///
  /// @param wait The longest the loop would sleep.
  /// @return The audience's own limit on the sleep; @p wait without one.
  auto service(std::chrono::milliseconds wait) -> std::chrono::milliseconds;
  /// @brief Ends sharing for good: drops the keys, description and guests and
  /// tells the audience the session ended.
  void session_over();
  /// @brief Tells the audience its guest was seated, when a guest holds the
  /// seat.
  ///
  /// @param seat The seat.
  void seat_joined(controllers::SeatNumber seat);
  /// @brief Frees a guest's seat the console refused and tells the audience.
  ///
  /// @param refusal The refusal.
  void seat_refused(const SeatRefusal& refusal);
  /// @brief Frees a guest's seat that emptied and tells the audience.
  ///
  /// @param seat The seat.
  void seat_left(controllers::SeatNumber seat);

  /// @brief Reports whether the session can be shared now.
  ///
  /// @return Nothing; NotShareable::cloud_session for the cloud;
  ///     NotShareable::session_ended once over or unbound;
  ///     NotShareable::protocol_version for a Takion version that cannot be
  ///     forwarded.
  [[nodiscard]] auto can_be_shared() const -> Expected<void, NotShareable>;
  /// @brief Starts sharing with an audience and wakes the session thread.
  ///
  /// @param audience Receives the stream; must outlive the returned handle.
  /// @return The handle that ends sharing; the reason it cannot be shared, or
  ///     NotShareable::already_shared while another audience is attached.
  [[nodiscard]] auto begin(SharedAudience& audience)
      -> Expected<Sharing, NotShareable>;
  /// @brief Seats a guest in a spare seat.
  ///
  /// Tries the preferred seat, or each spare seat from 1.
  ///
  /// @param request The guest, controller, account and preferred seat.
  /// @return The seat handle; SeatRefusal::no_free_seat when none could be
  ///     taken; a not-sent refusal when sharing has not begun or ended; the
  ///     seats' refusal otherwise.
  [[nodiscard]] auto seat_guest(const GuestSeat& request)
      -> Expected<SharedSeat, SeatRefusal>;
  /// @brief Asks the session for a keyframe, for a guest that lost the
  /// picture.
  void request_keyframe();
  /// @brief Wakes the session thread.
  void wake() noexcept;

  /// @brief Ends sharing with the audience of a ticket, frees its guests'
  /// seats, and waits until no delivery is still using the audience.
  ///
  /// @param ticket The audience's ticket; another ticket is ignored.
  void stop(std::uint64_t ticket);
  /// @brief Sends a guest's controller state.
  ///
  /// @param seat The guest's seat.
  /// @param ticket The guest's ticket.
  /// @param state The state.
  /// @return Nothing; InputRefusal::not_running once over or unbound;
  ///     InputRefusal::not_owner when the ticket does not hold the seat; as
  ///     Controllers::Core::forward.
  [[nodiscard]] auto send(controllers::SeatNumber seat, std::uint64_t ticket,
                          const controllers::ControllerState& state)
      -> Expected<void, InputRefusal>;
  /// @brief Frees a guest's seat; ignored for a stale ticket.
  ///
  /// @param seat The seat.
  /// @param ticket The guest's ticket.
  void release(controllers::SeatNumber seat, std::uint64_t ticket);

  /// @brief Converts a source datagram for the audience.
  ///
  /// @param wired The datagram.
  /// @return The shared datagram.
  static auto shared(
      const net::protocol::takion::SourceDatagram& wired) noexcept
      -> SharedDatagram;
  /// @brief Copies receive keys into wiping storage.
  ///
  /// @param wired The keys.
  /// @return The copy.
  static auto copied(
      const net::protocol::takion::SourceReceiveKeys& wired) noexcept
      -> ReceiveKeys;

 private:
  /// @brief Tracks which thread is delivering to the audience, so ending the
  /// share can wait for it.
  class Deliveries {
   public:
    /// @brief Makes a tracker with nobody delivering.
    ///
    /// @param threading Provider of the tracker's mutex and condition
    ///     variable; must outlive the tracker.
    /// @param clock Monotonic clock the condition variable measures on;
    ///     must outlive the tracker.
    Deliveries(platform::ThreadingProvider& threading,
               const platform::ClockProvider& clock)
        : mutex_(threading), finished_(threading, clock) {}

    /// @brief Marks the calling thread as delivering for its scope; nested
    /// turns on the same thread are allowed.
    class Turn {
     public:
      /// @brief Starts a turn.
      ///
      /// @param deliveries The tracker.
      explicit Turn(Deliveries& deliveries) noexcept;
      /// @brief Ends the turn when it was the outermost one.
      ~Turn();
      /// @brief Not copyable; a turn is scoped.
      Turn(const Turn&) = delete;
      /// @brief Not copy-assignable; a turn is scoped.
      auto operator=(const Turn&) -> Turn& = delete;

     private:
      /// @brief The tracker.
      Deliveries& deliveries_;
      /// @brief Whether this turn marked the thread.
      bool outermost_;
    };

    /// @brief Blocks until no other thread is delivering; returns at once on
    /// the delivering thread itself.
    void settle() const noexcept;

   private:
    /// @brief The delivering thread; names none when nobody delivers.
    std::atomic<platform::ThreadIdentity> thread_{};
    /// @brief How many threads wait in Deliveries::settle, so a turn only
    /// wakes them when someone waits.
    mutable std::atomic<std::size_t> settling_{};
    /// @brief Orders the end of a turn with the waits on
    /// Deliveries::finished_.
    mutable platform::Mutex mutex_;
    /// @brief Signalled when the outermost turn ends while someone waits.
    mutable platform::ConditionVariable finished_;
  };

  /// @brief The audience and its ticket.
  struct Registration {
    /// @brief The audience.
    SharedAudience& audience;
    /// @brief The ticket that identifies this sharing.
    std::uint64_t ticket{};
  };

  /// @brief A guest holding a seat.
  struct Guest {
    /// @brief The guest.
    GuestTag tag;
    /// @brief The ticket that identifies this seating.
    std::uint64_t ticket{};
  };

  /// @brief Builds a share for a session.
  ///
  /// @param threading Provider of the share's mutexes and condition
  ///     variable; must outlive the share.
  /// @param clock Monotonic clock the share's condition variable measures
  ///     on; must outlive the share.
  /// @param source Where the session runs and its asked frame rate.
  Core(platform::ThreadingProvider& threading,
       const platform::ClockProvider& clock, Source source);

  /// @brief Keeps the stream's Takion version and receive keys; a new
  /// generation starts.
  ///
  /// @param takion_version The Takion version.
  /// @param keys The receive keys.
  void on_source_keys(
      std::uint32_t takion_version,
      const net::protocol::takion::SourceReceiveKeys& keys) noexcept override;
  /// @brief Forwards a datagram to an audience that was told this generation.
  ///
  /// @param datagram The datagram.
  void on_source_datagram(
      const net::protocol::takion::SourceDatagram& datagram) noexcept override;
  /// @brief Passes the description on, keeps it, starts a new generation and
  /// tells the audience.
  ///
  /// @param description The stream description.
  void on_stream_description(
      const media::StreamDescription& description) override;
  /// @brief Passes a video unit on.
  ///
  /// @param unit The unit.
  /// @return The frontend's verdict.
  auto on_video(const media::VideoUnit& unit) -> media::VideoResult override;
  /// @brief Passes a video gap on.
  void on_video_gap() override;
  /// @brief Passes an audio unit on.
  ///
  /// @param unit The unit.
  /// @return The frontend's verdict.
  auto on_audio(const media::AudioUnit& unit) -> media::AudioResult override;
  /// @brief Passes feedback on, and to the audience when a guest is seated or
  /// it sets the haptic intensity.
  ///
  /// @param feedback The feedback.
  void on_feedback(const controllers::ControllerFeedback& feedback) override;

  /// @brief Decides whether the session can be shared; call with the
  /// operations mutex held.
  ///
  /// @return As SessionShare::Core::can_be_shared.
  auto verdict() const -> Expected<void, NotShareable>;
  /// @brief Reports whether an audience was told the current generation.
  ///
  /// @param listener The audience.
  /// @return `true` when told.
  auto told(const Registration& listener) const noexcept -> bool;
  /// @brief Tells an audience the stream, keys and haptic intensity.
  ///
  /// @param listener The audience.
  /// @return `true` when told; `false` without keys, version or description,
  ///     or with a version that cannot be forwarded.
  auto announce(const Registration& listener) -> bool;
  /// @brief Tells an audience once that the stream cannot be shared because of
  /// its Takion version.
  ///
  /// @param listener The audience.
  void refuse(const Registration& listener);
  /// @brief Reserves a seat for a guest.
  ///
  /// @param seat The seat.
  /// @param request The guest.
  /// @return The guest's ticket; empty once over, before sharing began, or when
  ///     a guest holds the seat.
  auto reserve(controllers::SeatNumber seat, const GuestSeat& request)
      -> std::optional<std::uint64_t>;
  /// @brief Forgets a reservation; ignored for a stale ticket.
  ///
  /// @param seat The seat.
  /// @param ticket The guest's ticket.
  void forget(controllers::SeatNumber seat, std::uint64_t ticket);

  /// @brief Where the session runs.
  const Where where_;
  /// @brief The frame rate asked for.
  const std::uint32_t requested_fps_;
  /// @brief The public face.
  SessionShare face_;
  /// @brief The frontend's sink.
  media::MediaSink* next_{};
  /// @brief Tracks deliveries to the audience.
  Deliveries deliveries_;
  /// @brief Serialises the public operations.
  mutable platform::Mutex operations_;
  /// @brief The bound session; null once closed.
  Session::Core* session_{};
  /// @brief Guards the state below.
  mutable platform::Mutex mutex_;
  /// @brief The audience; null when nobody shares.
  std::unique_ptr<Registration> registration_;
  /// @brief The stream's receive keys.
  std::optional<ReceiveKeys> keys_;
  /// @brief The stream's Takion version.
  std::optional<std::uint32_t> version_;
  /// @brief The stream's description.
  std::optional<media::StreamDescription> description_;
  /// @brief The guest in each seat.
  std::array<std::optional<Guest>, controllers::SeatNumber::count> guests_{};
  /// @brief The last ticket issued.
  std::uint64_t tickets_{};
  /// @brief The audience ticket already told it cannot share.
  std::uint64_t refused_ticket_{};
  /// @brief Set once the session is over.
  bool over_{};
  /// @brief The audience as deliveries see it.
  std::atomic<const Registration*> listener_{};
  /// @brief Counts new keys and descriptions.
  std::atomic<std::uint64_t> stream_generation_{};
  /// @brief The audience ticket last told the stream.
  std::atomic<std::uint64_t> told_ticket_{};
  /// @brief The generation last told.
  std::atomic<std::uint64_t> told_generation_{};
  /// @brief Seats guests hold.
  std::atomic<std::size_t> held_seats_{};
  /// @brief The last haptic intensity the console set.
  std::atomic<controllers::FeedbackIntensity> haptic_intensity_{
      controllers::FeedbackIntensity::strong};
};

/// @brief The implementation behind the public Sharing handle: ends its
/// sharing when stopped or destroyed.
class Sharing::Core {
 public:
  /// @brief Builds the handle.
  ///
  /// @param share The share, held weakly.
  /// @param ticket The audience's ticket.
  Core(std::weak_ptr<SessionShare::Core> share, std::uint64_t ticket) noexcept;
  /// @brief Stops sharing.
  ~Core();
  /// @brief Not copyable; one handle per sharing.
  Core(const Core&) = delete;
  /// @brief Not copy-assignable; one handle per sharing.
  auto operator=(const Core&) -> Core& = delete;

  /// @brief Ends sharing once; blocks while deliveries to the audience finish.
  void stop() noexcept;

 private:
  /// @brief The share; empty once stopped or gone.
  std::weak_ptr<SessionShare::Core> share_;
  /// @brief The audience's ticket.
  std::uint64_t ticket_;
};

/// @brief The implementation behind the public SharedSeat handle: a guest's
/// seat, freed when destroyed.
class SharedSeat::Core {
 public:
  /// @brief Builds the handle.
  ///
  /// @param share The share, held weakly.
  /// @param seat The seat.
  /// @param guest The guest.
  /// @param ticket The guest's ticket.
  Core(std::weak_ptr<SessionShare::Core> share, controllers::SeatNumber seat,
       GuestTag guest, std::uint64_t ticket) noexcept;
  /// @brief Frees the seat.
  ~Core();
  /// @brief Not copyable; one handle per seat.
  Core(const Core&) = delete;
  /// @brief Not copy-assignable; one handle per seat.
  auto operator=(const Core&) -> Core& = delete;

  /// @brief Reads the seat.
  ///
  /// @return The seat.
  auto number() const noexcept -> controllers::SeatNumber { return seat_; }
  /// @brief Reads the guest.
  ///
  /// @return The guest.
  auto guest() const noexcept -> GuestTag { return guest_; }
  /// @brief Sends the guest's controller state.
  ///
  /// @param state The state.
  /// @return As SessionShare::Core::send; InputRefusal::not_running once the
  ///     share is gone.
  [[nodiscard]] auto send(const controllers::ControllerState& state)
      -> Expected<void, InputRefusal>;

 private:
  /// @brief The share.
  std::weak_ptr<SessionShare::Core> share_;
  /// @brief The seat.
  controllers::SeatNumber seat_;
  /// @brief The guest.
  GuestTag guest_;
  /// @brief The guest's ticket.
  std::uint64_t ticket_;
};

}  // namespace libchiaking::play
