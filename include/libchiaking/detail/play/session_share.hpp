/// @file
/// @brief Declares session sharing: forwarding a running session's stream to
/// guests, seating them, and receiving such a stream on the guest's side.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/media/media_sink.hpp"
#include "libchiaking/detail/play/controllers.hpp"
#include "libchiaking/detail/play/input_refusal.hpp"
#include "libchiaking/detail/play/outcome.hpp"
#include "libchiaking/detail/play/seats.hpp"
#include "libchiaking/detail/play/session.hpp"
#include "libchiaking/detail/play/statistics.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/detail/users/account_id.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace libchiaking::platform {
class PlatformFactory;
}

namespace libchiaking::play {

/// @brief The key and initialisation vector a guest needs to decrypt a shared
/// stream.
///
/// Both are held in memory that is wiped on destruction. The type moves but
/// does not copy implicitly; clone makes a deliberate copy.
class ReceiveKeys {
 public:
  /// @brief Size in bytes of the key and of the initialisation vector.
  static constexpr std::size_t size = 16;

  /// @brief Takes ownership of a key and its initialisation vector.
  ///
  /// @param key The stream's receive key.
  /// @param iv The matching initialisation vector.
  ReceiveKeys(SecretArray<size> key, SecretArray<size> iv) noexcept
      : key_(std::move(key)), iv_(std::move(iv)) {}

  /// @brief Makes a second copy of the secret, which is wiped separately.
  ///
  /// @return The copy.
  auto clone() const noexcept -> ReceiveKeys {
    return ReceiveKeys{key_.clone(), iv_.clone()};
  }

  /// @brief The key bytes; valid for as long as this object.
  ///
  /// @return The key bytes; valid for as long as this object.
  auto key() const noexcept -> std::span<const std::byte, size> {
    return std::span<const std::byte, size>{key_.data(), key_.size()};
  }
  /// @brief The initialisation vector bytes; valid for as long as this
  /// object.
  ///
  /// @return The initialisation vector bytes; valid for as long as this
  ///     object.
  auto iv() const noexcept -> std::span<const std::byte, size> {
    return std::span<const std::byte, size>{iv_.data(), iv_.size()};
  }

 private:
  SecretArray<size> key_;
  SecretArray<size> iv_;
};

/// @brief Describes a shared stream to the side that will receive it.
///
/// The host gets it in SharedAudience::stream_started and sends it to guests,
/// who pass it to ForwardedStream::open.
struct SharedStream {
  /// @brief Version of the stream's transport protocol on the host.
  std::uint32_t protocol_version{};
  /// @brief The kind of connection the host's session plays over.
  Where where{Where::home};
  /// @brief The stream's audio format and picture sizes.
  media::StreamDescription description{};
  /// @brief True when the granted picture is known and is not H.264.
  bool hevc{};
  /// @brief The granted frame rate, or the rate the host asked for when the
  /// grant is not known; may be 0.
  std::uint32_t frames_per_second{};
};

/// @brief One datagram of the host's stream, still encrypted, with what the
/// host knows about it.
///
/// ForwardedStream::receive needs only media, key_position and bytes. The
/// remaining fields describe the media unit the datagram belongs to, so that a
/// relay can prioritise or drop datagrams without decrypting them.
struct SharedDatagram {
  /// @brief Which stream the datagram belongs to.
  enum class Media : std::uint8_t {
    /// @brief The video stream.
    video,
    /// @brief The audio stream.
    audio,
  };

  /// @brief Which stream the datagram belongs to.
  Media media{Media::video};
  /// @brief Position in the key stream at which the datagram decrypts;
  /// forward it unchanged.
  std::uint64_t key_position{};
  /// @brief The datagram as received. In SharedAudience::encrypted_datagram
  /// the span is valid only during the call.
  std::span<const std::byte> bytes{};
  /// @brief Size in bytes of the encrypted media payload inside the datagram.
  std::size_t sealed_size{};
  /// @brief Index of this unit within its frame.
  std::uint16_t unit_index{};
  /// @brief Number of units in the frame.
  std::uint16_t unit_total{};
  /// @brief How many of the frame's units carry error-correction data.
  std::uint16_t unit_redundant{};
  /// @brief Size in bytes of one unit of an audio frame; 0 for video.
  std::uint16_t unit_size{};
  /// @brief The stream's codec identifier for the unit.
  std::uint8_t codec{};
  /// @brief For video, the index of the picture size the unit belongs to; 0
  /// for audio.
  std::uint8_t stream_index{};
  /// @brief True when an audio datagram carries controller haptics rather
  /// than sound.
  bool auxiliary{};
};

/// @brief Why a session cannot be shared.
enum class NotShareable : std::uint8_t {
  /// @brief Cloud sessions cannot be shared.
  cloud_session,
  /// @brief The stream uses a protocol version that cannot be forwarded.
  protocol_version,
  /// @brief The session has ended or is being destroyed.
  session_ended,
  /// @brief The session already has an audience; stop that share first.
  already_shared,
};

/// @brief Receives a shared session's stream and guest events on the host, for
/// relaying to guests.
///
/// The host frontend implements this interface and passes it to
/// SessionShare::begin. The object must stay alive until the returned Sharing
/// has been stopped or destroyed, or session_ended has been called.
///
/// Calls other than encrypted_datagram are made by the session while it runs,
/// on its own thread. Calls should return quickly.
///
/// @see SessionShare
/// @see ForwardedStream
class SharedAudience {
 public:
  /// @brief Destroys the audience.
  virtual ~SharedAudience() = default;

  /// @brief Called when the stream is ready to be forwarded, and again
  /// whenever its keys or description change.
  ///
  /// No datagram is delivered before the first call. Send both arguments to
  /// each guest, who opens a ForwardedStream with them.
  ///
  /// @param stream What the stream consists of.
  /// @param keys The stream's receive keys; the audience owns this copy.
  virtual void stream_started(const SharedStream& stream, ReceiveKeys keys) = 0;
  /// @brief Called for every video and audio datagram the host receives.
  ///
  /// It is called on the path that receives the stream, which may be a
  /// different thread from the other callbacks, so it must not block and must
  /// not throw. Copy the bytes before returning.
  ///
  /// @param datagram The encrypted datagram and what is known about it.
  virtual void encrypted_datagram(const SharedDatagram& datagram) noexcept = 0;
  /// @brief Called with controller feedback from the console while at least
  /// one guest holds a seat, and with every HapticIntensity change.
  ///
  /// The audience routes it to the guest whose seat it names. The current
  /// HapticIntensity also follows each stream_started call, so an audience
  /// that joins after the console set it still learns the setting.
  ///
  /// @param feedback The feedback; a HapticAudio fact refers to bytes valid
  ///     only during the call.
  virtual void feedback(const controllers::ControllerFeedback& feedback) = 0;
  /// @brief Called when the console has let a guest's seat join.
  ///
  /// @param seat The guest's seat.
  virtual void guest_seated(controllers::SeatNumber seat) = 0;
  /// @brief Called when a guest's seat request failed; the seat is free again
  /// and the guest's SharedSeat no longer sends input.
  ///
  /// @param refusal Which seat and why.
  virtual void guest_seat_refused(const SeatRefusal& refusal) = 0;
  /// @brief Called when a guest lost a seat it held, because the console
  /// dropped it or the stream was lost.
  ///
  /// @param seat The seat, which is free again.
  virtual void guest_unseated(controllers::SeatNumber seat) = 0;
  /// @brief Called when a share that had begun turns out not to be possible.
  ///
  /// It is reported once per share, when the stream's protocol version
  /// becomes known and cannot be forwarded.
  ///
  /// @param reason Why; NotShareable::protocol_version.
  virtual void became_unshareable(NotShareable reason) = 0;
  /// @brief Called on each turn of the session's loop so the audience can do
  /// its periodic work.
  ///
  /// @param wait The longest the session intends to wait before its next
  ///     turn.
  /// @return The longest the audience is willing to wait before being called
  ///     again; the session waits no longer than the smaller of the two. Call
  ///     SessionShare::wake to be called sooner.
  virtual auto service(std::chrono::milliseconds wait)
      -> std::chrono::milliseconds = 0;
  /// @brief Called once when the session is over; no call follows and the
  /// share has ended.
  virtual void session_ended() = 0;
};

/// @brief Keeps a share alive; stopping or destroying it ends the share.
///
/// Returned by SessionShare::begin. Ending the share releases every seat held
/// by a guest. It is safe to keep past the end of the session, when stopping
/// does nothing.
class Sharing {
 public:
  /// @brief Implementation behind the handle.
  class Core;

  /// @brief Wraps a share implementation.
  ///
  /// Frontends do not construct this; SessionShare::begin returns it.
  ///
  /// @param core The implementation; the handle takes ownership.
  explicit Sharing(std::unique_ptr<Core> core) noexcept;
  /// @brief Moves the share; the source no longer controls it.
  ///
  /// @param other The share to take over.
  Sharing(Sharing&& other) noexcept;
  /// @brief Takes over another share, ending the one held before.
  ///
  /// @param other The share to take over.
  /// @return This object.
  auto operator=(Sharing&& other) noexcept -> Sharing&;
  /// @brief Ends the share if it is still running.
  ~Sharing();

  /// @brief Ends the share.
  ///
  /// When this returns, no call to the SharedAudience is in progress on
  /// another thread and none will follow, so the audience may be destroyed.
  /// Calling it again does nothing.
  void stop() noexcept;

 private:
  std::unique_ptr<Core> core_;
};

/// @brief A seat held by a remote guest; destroying it gives the seat up.
///
/// Returned by SessionShare::seat_guest once the join has been queued. The
/// guest's input goes through send. Destroying the object asks the console to
/// remove the seat's controller.
class SharedSeat {
 public:
  /// @brief Implementation behind the seat.
  class Core;

  /// @brief Wraps a guest seat implementation.
  ///
  /// Frontends do not construct this; SessionShare::seat_guest returns it.
  ///
  /// @param core The implementation; the seat takes ownership.
  explicit SharedSeat(std::unique_ptr<Core> core) noexcept;
  /// @brief Moves the seat; the source no longer holds it and must not be
  /// used.
  ///
  /// @param other The seat to take over.
  SharedSeat(SharedSeat&& other) noexcept;
  /// @brief Takes over another seat, giving up the one held before.
  ///
  /// @param other The seat to take over.
  /// @return This object.
  auto operator=(SharedSeat&& other) noexcept -> SharedSeat&;
  /// @brief Gives the seat up if it is still held.
  ~SharedSeat();

  /// @brief The seat the guest was given.
  ///
  /// @return The seat the guest was given.
  auto number() const noexcept -> controllers::SeatNumber;
  /// @brief The tag of the guest holding the seat.
  ///
  /// @return The tag of the guest holding the seat.
  auto guest() const noexcept -> GuestTag;
  /// @brief Sends the guest's controller state to the console.
  ///
  /// Unlike Controllers::send the state is not queued; it is passed to the
  /// connection on the calling thread.
  ///
  /// @param state The guest controller's complete current state.
  /// @return Nothing on success. A failure to deliver is written to the
  ///     session journal.
  /// @retval InputRefusal::invalid The state sets button bits outside
  ///     ControllerState::button_mask.
  /// @retval InputRefusal::not_running The session has ended or is not
  ///     accepting requests.
  /// @retval InputRefusal::not_owner This object no longer holds the seat,
  ///     because the join was refused, the seat was dropped, or the share was
  ///     stopped.
  [[nodiscard]] auto send(const controllers::ControllerState& state)
      -> Expected<void, InputRefusal>;

 private:
  std::unique_ptr<Core> core_;
};

/// @brief Shares a running session with guests on other devices.
///
/// The host forwards the stream it receives, still encrypted, to an audience
/// that relays it to guests; each guest decrypts and plays it with a
/// ForwardedStream. Guests can also be given seats, so that their controllers
/// play alongside the host's.
///
/// Only home and internet sessions can be shared, and one audience at a time.
///
/// Obtained from Session::share and valid for as long as that Session. All
/// members may be called from any thread.
///
/// @see SharedAudience
/// @see ForwardedStream
class SessionShare {
 public:
  /// @brief Implementation behind the control; owned by the session.
  class Core;

  /// @brief A request to seat one guest.
  struct GuestSeat {
    /// @brief The frontend's tag for the guest.
    GuestTag guest;
    /// @brief The controller model to announce for the seat.
    controllers::ControllerKind controller{
        controllers::ControllerKind::console_default};
    /// @brief Account to sign the seat in as; required by a PS5 console, as
    /// for Seats::add_player.
    std::optional<AccountId> as{};
    /// @brief The seat the guest wants. When absent, the lowest free added
    /// seat is used.
    std::optional<controllers::SeatNumber> preferred{};
  };

  /// @brief Binds the control to the session's implementation.
  ///
  /// Frontends do not construct this; they use Session::share.
  ///
  /// @param core Implementation owned by the session. It must outlive this
  ///     object.
  explicit SessionShare(Core& core) noexcept : core_(&core) {}
  /// @brief Not copyable; a session has one sharing control.
  SessionShare(const SessionShare&) = delete;
  /// @brief Not copy-assignable; a session has one sharing control.
  auto operator=(const SessionShare&) -> SessionShare& = delete;

  /// @brief Reports whether a stream of the given protocol version can be
  /// forwarded.
  ///
  /// @param protocol_version The value of SharedStream::protocol_version or
  ///     Statistics::protocol_version.
  /// @return True when streams of that version can be shared.
  static auto shares_protocol_version(std::uint32_t protocol_version) noexcept
      -> bool;

  /// @brief Checks whether this session can be shared now.
  ///
  /// While the stream's protocol version is not yet known the answer is yes;
  /// a share begun then may still be refused through
  /// SharedAudience::became_unshareable.
  ///
  /// @return Nothing when the session can be shared.
  /// @retval NotShareable::cloud_session The session is a cloud session.
  /// @retval NotShareable::session_ended The session has ended.
  /// @retval NotShareable::protocol_version The stream's protocol version
  ///     cannot be forwarded.
  [[nodiscard]] auto can_be_shared() const -> Expected<void, NotShareable>;
  /// @brief Starts forwarding the session's stream to an audience.
  ///
  /// The audience hears SharedAudience::stream_started as soon as the
  /// stream's keys and description are known, then every datagram.
  ///
  /// @param audience Receiver of the stream and guest events. It must outlive
  ///     the returned Sharing.
  /// @return The handle that keeps the share alive.
  /// @retval NotShareable::cloud_session The session is a cloud session.
  /// @retval NotShareable::session_ended The session has ended.
  /// @retval NotShareable::protocol_version The stream's protocol version
  ///     cannot be forwarded.
  /// @retval NotShareable::already_shared Another share is running.
  [[nodiscard]] auto begin(SharedAudience& audience)
      -> Expected<Sharing, NotShareable>;
  /// @brief Asks for a seat for a guest.
  ///
  /// On success the join is queued, as by Seats::add_player, and the seat
  /// reads as SeatState::joining with the guest as holder. The console's
  /// answer arrives through SharedAudience::guest_seated or
  /// SharedAudience::guest_seat_refused, and through SessionEvents like any
  /// other seat. The host's own Controllers::send is refused for the seat
  /// while the guest holds it.
  ///
  /// @param request Who the guest is and which seat is wanted.
  /// @return The guest's hold on the seat.
  /// @retval SeatRefusal::Kind::no_free_seat The wanted seat is taken, or no
  ///     added seat is free.
  /// @retval SeatRefusal::Kind::not_sent No share is running, or the session
  ///     is not accepting requests; the cause is
  ///     EndReason::Cause::invalid_state.
  [[nodiscard]] auto seat_guest(const GuestSeat& request)
      -> Expected<SharedSeat, SeatRefusal>;
  /// @brief Asks the console for a keyframe on behalf of a guest.
  ///
  /// Call it when a guest reports ForwardedStream::wants_keyframe. Does
  /// nothing once the session is being destroyed.
  void request_keyframe();
  /// @brief Wakes the session's loop so that SharedAudience::service is
  /// called soon.
  void wake() noexcept;

 private:
  Core* core_;
};

/// @brief Plays a shared stream on a guest's device.
///
/// The guest's frontend opens one with the SharedStream and ReceiveKeys relayed
/// from the host, feeds it every relayed datagram with receive, and calls
/// service regularly. Decrypted video, audio and feedback come out through the
/// MediaSink on the calling thread, as they would from a Session.
///
/// The object is not safe for concurrent use; call it from one thread at a
/// time.
///
/// @see SessionShare
/// @see SharedAudience
class ForwardedStream {
 public:
  /// @brief Implementation behind the stream.
  class Core;

  /// @brief Why a forwarded stream could not be opened or could not continue.
  enum class Refusal : std::uint8_t {
    /// @brief The stream description is unusable: it has no picture sizes,
    /// or they failed validation.
    invalid_stream,
    /// @brief The stream's protocol version cannot be forwarded.
    unsupported_version,
    /// @brief The platform offers no cryptography provider, or it could not
    /// set up decryption.
    crypto_unavailable,
    /// @brief A datagram is not a well-formed stream packet.
    malformed_datagram,
    /// @brief A datagram failed its integrity check; the keys or key
    /// position do not match.
    authentication_failed,
    /// @brief The media could not be processed or the sink reported a
    /// failure; fault() has the detail when the sink did.
    media_failed,
  };

  /// @brief Opens a receiver for a shared stream and announces the stream to
  /// the sink.
  ///
  /// MediaSink::on_stream_description is called before this returns.
  ///
  /// @param platform Supplies cryptography, the clock and diagnostics. It
  ///     must outlive the stream.
  /// @param stream The description relayed from
  ///     SharedAudience::stream_started.
  /// @param keys The receive keys relayed with it.
  /// @param media Receiver of the decoded stream. It must outlive the stream.
  /// @return The open stream.
  /// @retval Refusal::crypto_unavailable The platform has no cryptography
  ///     provider, or decryption could not be set up.
  /// @retval Refusal::unsupported_version The stream's protocol version
  ///     cannot be forwarded.
  /// @retval Refusal::invalid_stream The description has no picture sizes or
  ///     they failed validation.
  [[nodiscard]] static auto open(platform::PlatformFactory& platform,
                                 SharedStream stream, ReceiveKeys keys,
                                 media::MediaSink& media)
      -> Expected<std::unique_ptr<ForwardedStream>, Refusal>;

  /// @brief Wraps a stream implementation.
  ///
  /// Frontends do not construct this; they call open.
  ///
  /// @param core The implementation; the stream takes ownership.
  explicit ForwardedStream(std::unique_ptr<Core> core) noexcept;
  /// @brief Not copyable; a forwarded stream has one owner.
  ForwardedStream(const ForwardedStream&) = delete;
  /// @brief Not copy-assignable; a forwarded stream has one owner.
  auto operator=(const ForwardedStream&) -> ForwardedStream& = delete;
  /// @brief Closes the stream.
  ~ForwardedStream();

  /// @brief Takes one relayed datagram and delivers any media it completes to
  /// the sink.
  ///
  /// @param datagram The datagram as the host's audience saw it; media,
  ///     key_position and bytes are used.
  /// @return Nothing on success.
  /// @retval Refusal::malformed_datagram The datagram is not a well-formed
  ///     stream packet.
  /// @retval Refusal::authentication_failed The datagram failed its integrity
  ///     check.
  /// @retval Refusal::media_failed The media could not be processed, or the
  ///     sink has reported a failure that ends playback.
  [[nodiscard]] auto receive(const SharedDatagram& datagram)
      -> Expected<void, Refusal>;
  /// @brief Runs the stream's timers and delivers any media that became
  /// ready.
  ///
  /// Call it at least as often as it asks.
  ///
  /// @param wait The longest the caller would like to wait before calling
  ///     again.
  /// @return How long the caller may wait before the next call; never more
  ///     than wait, and zero when work is already due.
  /// @retval Refusal::media_failed The media could not be processed, or the
  ///     sink has reported a failure that ends playback.
  [[nodiscard]] auto service(std::chrono::milliseconds wait)
      -> Expected<std::chrono::milliseconds, Refusal>;
  /// @brief Reports whether the guest needs a keyframe, and clears the
  /// request.
  ///
  /// It becomes true after stream loss or repeated decoder rejections. Relay
  /// it to the host, which calls SessionShare::request_keyframe.
  ///
  /// @return True when a keyframe should be asked for now.
  auto wants_keyframe() noexcept -> bool;

  /// @brief Tells the stream that the frontend has shown a video frame; see
  /// Session::video_presented.
  ///
  /// @return The loss accumulated since the previous call.
  auto video_presented() noexcept -> media::PresentationLoss;
  /// @brief Tells the stream that the decoder rejected a frame after
  /// MediaSink::on_video returned; see Session::video_rejected.
  ///
  /// @param frame The rejected frame's VideoFrame::frame_index.
  /// @param cause The decoder's own error code.
  void video_rejected(std::uint16_t frame, int cause);
  /// @brief Tells the stream that video output has failed; later calls to
  /// receive and service return Refusal::media_failed.
  ///
  /// @param cause The frontend's own error code; kept in the fault.
  void video_output_failed(int cause);
  /// @brief The media failure that stopped playback, if there is one.
  ///
  /// @return The media failure that stopped playback, if there is one.
  auto fault() const -> std::optional<media::MediaFault>;
  /// @brief Returns a snapshot of the stream's video statistics.
  ///
  /// @return A snapshot of the stream's video statistics.
  auto statistics() const -> media::VideoStatistics;

 private:
  std::unique_ptr<Core> core_;
};

}  // namespace libchiaking::play
