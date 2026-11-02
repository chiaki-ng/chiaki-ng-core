/// @file
/// @brief Declares ActiveSession, the running Takion stream of a session:
/// its handshake, media pipeline, controller input and feedback, and the
/// queues the play context drains.
///
/// ActiveSession owns the Takion client on the calibrated route. Each pump
/// drains Takion's events: it drives the BIG/BANG handshake (each of its
/// three windows may take handshake_stage_timeout_us), validates and
/// acknowledges the stream info, puts video units back in order, assembles
/// frames, tracks frame dependencies and reports lost frames (asking for a
/// keyframe when configured), and every 200 ms sends a congestion report and
/// every second a heartbeat.
#pragma once
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/protocol/remote_play/stream/diagnostics.hpp"
#include "src/core/utils/diagnostics/trace.hpp"

#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"
#include "src/core/net/protocol/remote_play/ecdh_wire.hpp"
#include "src/core/net/protocol/remote_play/stream/active_handshake.hpp"
#include "src/core/net/protocol/remote_play/stream/active_route_lifecycle.hpp"
#include "src/core/net/protocol/remote_play/stream/active_takion.hpp"
#include "src/core/net/protocol/remote_play/stream/audio_negotiation.hpp"
#include "src/core/net/protocol/remote_play/stream/cloud_stream_media.hpp"
#include "src/core/net/protocol/remote_play/stream/controller.hpp"
#include "src/core/net/protocol/remote_play/stream/limits.hpp"
#include "src/core/net/protocol/remote_play/stream/microphone_sender.hpp"
#include "src/core/net/protocol/remote_play/stream/video_bitstream.hpp"
#include "src/core/net/protocol/remote_play/stream/video_reorder_queue.hpp"
#include "src/core/utils/containers/bounded_queue.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::net::protocol::remote_play {

/// @brief The Takion event sink of an active session: an unbounded queue
/// the session drains on each pump.
///
/// Takion calls on_event() from the same thread that pumps it.
class ActiveEventInbox final : public takion::EventSink {
 public:
  /// @brief Queues an event.
  ///
  /// @param event The event.
  void on_event(takion::Event event) override;

  /// @brief Takes the oldest event.
  ///
  /// @return The event, or nothing.
  [[nodiscard]] auto take_next() -> std::optional<takion::Event>;

  /// @brief How many events wait.
  ///
  /// @return The count.
  auto size() const noexcept -> std::size_t { return events_.size(); }

 private:
  /// @brief The events.
  containers::BoundedQueue<takion::Event> events_{
      std::numeric_limits<std::size_t>::max()};
};

/// @brief A media unit leaving the session: audio, video or haptics.
using ActiveMediaEvent = std::variant<
    takion::client_event::EncodedAudio, takion::client_event::EncodedVideo,
    takion::client_event::Haptics, takion::client_event::ControllerSpeaker>;

/// @brief Controller feedback the session learns from the console's pad
/// info.
namespace feedback_event {
/// @brief The console reset the motion sensors.
struct MotionReset {};
/// @brief The console's motion sensor settings changed.
struct MotionSettings {
  /// @brief The two setting bytes.
  std::array<std::uint8_t, 2> values{};
};
/// @brief The console's haptics intensity changed.
struct HapticIntensity {
  /// @brief The intensity, 0 to 3.
  std::uint8_t value{};
};
/// @brief The console's trigger effect intensity changed.
struct TriggerIntensity {
  /// @brief The intensity, 0 to 3.
  std::uint8_t value{};
};
/// @brief The host pad's light bar colour changed.
struct LedColor {
  /// @brief Red, green, blue.
  std::array<std::uint8_t, 3> rgb{};
};
/// @brief The host pad's player index changed.
struct PlayerIndex {
  /// @brief The index.
  std::uint8_t value{};
};
/// @brief The console confirmed a seat's pad, with its light bar colour.
struct PadConfirmed {
  /// @brief The seat.
  std::uint8_t index{};
  /// @brief Red, green, blue.
  std::array<std::uint8_t, 3> led{};
};
/// @brief A video frame was lost to FEC.
struct VideoFecFailure {
  /// @brief The last frame of the loss.
  std::uint16_t frame_index{};
  /// @brief Whether a keyframe request was outstanding.
  bool idr_request_sent{};
};
}  // namespace feedback_event

/// @brief Controller feedback for the caller.
using ActiveFeedbackEvent = std::variant<
    takion::client_event::Rumble, takion::client_event::TriggerEffects,
    feedback_event::MotionReset, feedback_event::MotionSettings,
    feedback_event::HapticIntensity, feedback_event::TriggerIntensity,
    feedback_event::LedColor, feedback_event::PlayerIndex,
    feedback_event::PadConfirmed, feedback_event::VideoFecFailure>;

/// @brief A bounded queue of controller feedback that sheds its oldest
/// entries when full.
///
/// Not thread-safe.
class ActiveFeedbackQueue {
 public:
  /// @brief The most entries held.
  static constexpr std::size_t capacity = queue_capacity;
  /// @brief Queues feedback, shedding the oldest when full.
  ///
  /// @param event The feedback.
  void push(ActiveFeedbackEvent event);
  /// @brief Takes the oldest feedback.
  ///
  /// @return The feedback, or nothing.
  [[nodiscard]] auto take() -> std::optional<ActiveFeedbackEvent>;
  /// @brief Empties the queue.
  void clear() noexcept;

 private:
  /// @brief The entries.
  containers::BoundedQueue<ActiveFeedbackEvent> events_{
      capacity,
      containers::BoundedQueue<ActiveFeedbackEvent>::WhenFull::shed_oldest};
};

/// @brief The media the session has ready for the caller, in arrival
/// order across five lanes (audio, video, haptics, controller speaker and
/// party voice).
///
/// Video overflowing its lane drops every queued frame except the codec
/// header and reports them lost. Audio overflowing drops the oldest frames
/// and puts one empty frame in their place so the decoder conceals the gap;
/// party voice does the same in its own lane, so neither stream's overflow
/// touches the other.
/// Haptics and controller speaker audio overflowing drop their oldest. Not
/// thread-safe.
class ActiveMediaQueue {
 public:
  /// @brief The most video frames held, codec headers aside.
  static constexpr std::size_t video_capacity = queue_capacity;
  /// @brief The most audio frames held, game audio and party voice each.
  static constexpr std::size_t audio_capacity = ring_capacity;
  /// @brief The most haptics frames held.
  static constexpr std::size_t haptics_capacity = queue_capacity;
  /// @brief The most controller speaker frames held.
  static constexpr std::size_t speaker_capacity = queue_capacity;
  /// @brief The most entries held.
  static constexpr std::size_t capacity =
      video_capacity + 2 * audio_capacity + haptics_capacity + speaker_capacity;
  /// @brief The most bytes per lane.
  static constexpr std::size_t max_bytes = media_queue_max_bytes;
  /// @brief Queues a media unit.
  ///
  /// @param event The unit.
  /// @return The video frames that had to be dropped to make room.
  auto push(ActiveMediaEvent event) -> std::vector<VideoFrameLoss>;
  /// @brief Whether a video frame would overflow its lane.
  ///
  /// @param incoming The frame's size in bytes.
  /// @return True when it would.
  auto video_overflows(std::size_t incoming) const noexcept -> bool;
  /// @brief Drops every queued video frame except the latest codec header.
  ///
  /// @return The range dropped, or nothing when none were.
  auto drop_video() -> std::optional<VideoFrameLoss>;
  /// @brief Takes the oldest unit across the lanes.
  ///
  /// @return The unit, or nothing.
  [[nodiscard]] auto take() -> std::optional<ActiveMediaEvent>;
  /// @brief Empties every lane.
  void clear() noexcept;
  /// @brief Units dropped.
  ///
  /// @return The count.
  auto dropped() const noexcept -> std::uint64_t { return dropped_; }

 private:
  /// @brief A queued unit with its arrival order.
  struct Entry {
    /// @brief Arrival order.
    std::uint64_t sequence{};
    /// @brief The unit.
    ActiveMediaEvent event;
  };
  /// @brief One lane.
  struct Lane {
    /// @brief Its units.
    std::deque<Entry> entries;
    /// @brief Their bytes.
    std::size_t bytes{};
  };
  /// @brief A unit's payload size.
  ///
  /// @param event The unit.
  /// @return Bytes.
  static auto media_size(const ActiveMediaEvent& event) noexcept -> std::size_t;
  /// @brief Whether a unit is a video codec header.
  ///
  /// @param event The unit.
  /// @return True for a header.
  static auto profile_header(const ActiveMediaEvent& event) noexcept -> bool;
  /// @brief Whether a unit is an empty audio frame marking a gap.
  ///
  /// @param event The unit.
  /// @return True for a marker.
  static auto concealment_marker(const ActiveMediaEvent& event) noexcept
      -> bool;
  /// @brief Whether a unit is an audio codec header.
  ///
  /// @param event The unit.
  /// @return True for a header.
  static auto audio_profile_header(const ActiveMediaEvent& event) noexcept
      -> bool;
  /// @brief Removes a lane's oldest unit.
  ///
  /// @param lane The lane.
  void pop_front(Lane& lane) noexcept;
  /// @brief Queues audio, evicting old frames and marking the gap.
  ///
  /// @param entry The frame.
  /// @param incoming Its size in bytes.
  /// @param voice Whether it is party voice, which has its own lane.
  void push_audio(Entry entry, std::size_t incoming, bool voice);
  /// @brief The lane party voice is queued in, after one lane per media
  /// type.
  static constexpr std::size_t voice_lane =
      std::variant_size_v<ActiveMediaEvent>;
  /// @brief Audio, video, haptics, controller speaker and party voice.
  std::array<Lane, voice_lane + 1> lanes_{};
  /// @brief Video codec headers queued.
  std::size_t video_headers_{};
  /// @brief The next arrival order.
  std::uint64_t next_sequence_{};
  /// @brief Units dropped.
  std::uint64_t dropped_{};
};

/// @brief Why the active session failed or refused a call.
enum class ActiveSessionError : std::uint8_t {
  /// @brief The Takion client could not start.
  start_failed,
  /// @brief Takion failed while pumping.
  pump_failed,
  /// @brief Takion refused a command other than for a full mailbox.
  post_failed,
  /// @brief The handshake failed.
  handshake_failed,
  /// @brief BANG could not be authenticated.
  bang_authentication_failed,
  /// @brief The stream is not ready, or has stopped.
  not_ready,
  /// @brief A controller state or record is out of range.
  invalid_controller_state,
  /// @brief A handshake window took longer than handshake_stage_timeout_us.
  handshake_timeout,
  /// @brief The console sent a disconnect.
  peer_disconnected,
  /// @brief Nothing arrived for the peer silence timeout.
  peer_silent,
  /// @brief The Takion version or features cannot carry the request.
  unsupported_request,
};

/// @brief Why and where the active session ended.
struct ActiveSessionFailure {
  /// @brief The handshake phase it failed in.
  ActiveHandshakeState stage;
  /// @brief Why.
  ActiveSessionError cause;
  /// @brief Takion's own error, when it had one.
  std::optional<takion::ClientError> transport;
  /// @brief The console's disconnect reason, when it sent one.
  std::optional<std::string> peer_reason;
};

/// @brief How an active session behaves, fixed when it starts.
///
/// Plain data.
struct ActiveControlProfile {
  /// @brief The controller presented to the console.
  takion::ControllerConnectionType controller_type{
      takion::ControllerConnectionType::dualsense};
  /// @brief How audio is assembled.
  AudioAssemblyPolicy audio_assembly{AudioAssemblyPolicy::console_units};
  /// @brief The bytes of each datagram Takion may not use.
  ActiveDatagramBudget datagram_budget{};
  /// @brief Whether the video is H.265.
  bool hevc{};
  /// @brief Whether a frame lost to FEC asks the console for a keyframe.
  bool enable_idr_on_fec_failure{};
  /// @brief How long the stream may receive nothing, in microseconds; 0 for
  /// no limit.
  std::uint64_t peer_silence_timeout_us{};
  /// @brief Whether a HEARTBEAT is also sent every second unasked. A session
  /// with a PS5 at home or over PSN answers the console's own heartbeat only:
  /// the console times each echo as a round trip for its rate control, so an
  /// unasked heartbeat makes that time meaningless.
  /// Every received HEARTBEAT is echoed either way.
  bool periodic_heartbeat{true};
  /// @brief Takion versions to negotiate; empty for the fixed version.
  std::vector<std::uint32_t> takion_offer{};
  /// @brief How many controllers take part.
  std::uint8_t pad_count{1};
  /// @brief Whether received audio is dropped.
  bool drop_audio{};
  /// @brief Whether received video is dropped.
  bool drop_video{};
  /// @brief Whether controller speaker audio is passed on.
  bool controller_speaker{};
  /// @brief The largest loss share reported.
  double packet_loss_max{MediaPacketStats::default_packet_loss_max};
  /// @brief A tap that observes the stream's packets, or null.
  takion::SourceTap* source_tap{};
  /// @brief Whether BANG must repeat the session key.
  BangSessionKeyPolicy bang_session_key{BangSessionKeyPolicy::required};

  /// @brief Copies the caller's tuning into a profile: audio and video
  /// dropping, controller speaker audio, the loss ceiling and the source tap.
  ///
  /// @param profile The profile, changed in place.
  /// @param tuning The tuning.
  static void apply_session_tuning(ActiveControlProfile& profile,
                                   const SessionTuning& tuning) noexcept;
};

/// @brief Where the media pipeline sends its requests to the console.
class ActiveMediaSink {
 public:
  /// @brief Destroys the sink.
  virtual ~ActiveMediaSink() = default;
  /// @brief Tells the console a range of frames is corrupt.
  ///
  /// @param first The first frame.
  /// @param last The last frame.
  /// @return Nothing, or why it could not be sent.
  [[nodiscard]] virtual auto post_corrupt_frame(std::uint16_t first,
                                                std::uint16_t last)
      -> Expected<void, ActiveSessionError> = 0;
  /// @brief Asks the console for a keyframe after a loss.
  ///
  /// @return True when sent, false when Takion's mailbox was full.
  /// @retval ActiveSessionError Why it failed otherwise.
  [[nodiscard]] virtual auto post_idr_after_loss()
      -> Expected<bool, ActiveSessionError> = 0;
  /// @brief Reports a frame lost to FEC.
  ///
  /// @param frame_index The last frame of the loss.
  /// @param idr_request_sent Whether a keyframe request is outstanding.
  virtual void on_video_fec_failure(std::uint16_t frame_index,
                                    bool idr_request_sent) = 0;
};

/// @brief How the media pipeline works.
///
/// Plain data.
struct ActiveMediaOptions {
  /// @brief How audio is assembled.
  AudioAssemblyPolicy audio_assembly{AudioAssemblyPolicy::console_units};
  /// @brief Whether Takion encodes unit counts only.
  bool count_only{};
  /// @brief Whether the video is H.265.
  bool hevc{};
  /// @brief Whether a frame lost to FEC asks for a keyframe.
  bool enable_idr_on_fec_failure{};
  /// @brief Whether PS cloud controller speaker lanes are passed on instead
  /// of dropped.
  bool controller_speaker{};
};

/// @brief From received AV units to queued media: assembly, frame
/// dependencies, profile switches and loss reports.
///
/// Also the Takion client's video gate, so units of frames the assembler has
/// moved past are not decrypted. Not thread-safe.
class ActiveMediaPipeline final : public takion::VideoUnitGate {
 public:
  /// @brief Makes a pipeline.
  ///
  /// @param sink Where requests to the console go; must outlive it.
  /// @param options How it works.
  /// @param diagnostics The counters it updates; must outlive it.
  ActiveMediaPipeline(ActiveMediaSink& sink, ActiveMediaOptions options,
                      StreamDiagnostics& diagnostics) noexcept;
  /// @brief Takes the stream info: picture sizes, audio channels and profiles.
  ///
  /// Only the first stream info is taken.
  ///
  /// @param info The checked stream info.
  /// @param features The negotiated Takion features.
  void on_stream_info(const ValidatedStreamInfo& info,
                      takion::FeatureSet features = {});
  /// @brief Takes one AV unit.
  ///
  /// A video unit of a new picture size first queues that size's codec header.
  ///
  /// @param values The unit; its data moves into the assembler.
  /// @param now_us Monotonic microseconds now.
  /// @return Nothing, or why reporting a loss failed.
  [[nodiscard]] auto consume(takion::AvPacketValues values,
                             std::uint64_t now_us)
      -> Expected<void, ActiveSessionError>;
  /// @brief Tells whether the assembler refuses units of a video frame as
  /// stale.
  ///
  /// @param frame_index The frame.
  /// @return True when the frame is behind the assembler.
  auto video_frame_stale(std::uint16_t frame_index) const noexcept
      -> bool override;
  /// @brief Queues a unit for the caller, reporting video it had to drop.
  ///
  /// @param event The unit.
  /// @return Nothing, or why reporting failed.
  [[nodiscard]] auto queue_media(ActiveMediaEvent event)
      -> Expected<void, ActiveSessionError>;
  /// @brief Queues an audio frame, preceded by a codec header when the
  /// profile changed.
  ///
  /// @param audio The frame.
  /// @return Nothing, or why queueing failed.
  [[nodiscard]] auto queue_audio(takion::client_event::EncodedAudio audio)
      -> Expected<void, ActiveSessionError>;
  /// @brief Queues a party voice chat frame, preceded by its lane's codec
  /// header when that changed; dropped when the stream info did not describe
  /// the lane.
  ///
  /// @param audio The frame, with EncodedAudio::voice set.
  /// @return Nothing, or why queueing failed.
  [[nodiscard]] auto queue_voice(takion::client_event::EncodedAudio audio)
      -> Expected<void, ActiveSessionError>;
  /// @brief Queues a video frame once its dependencies allow.
  ///
  /// @param video The frame.
  /// @param assembled Whether this pipeline assembled it.
  /// @return Nothing, or why queueing failed.
  [[nodiscard]] auto queue_video(takion::client_event::EncodedVideo video,
                                 bool assembled)
      -> Expected<void, ActiveSessionError>;
  /// @brief Reports frames the caller could not decode.
  ///
  /// Frames in the range that were delivered stop counting as delivered;
  /// the console is told every range back to the last good frame.
  ///
  /// @param first The first bad frame.
  /// @param last The last bad frame.
  /// @return Nothing, or why reporting failed.
  [[nodiscard]] auto report_loss(std::uint16_t first, std::uint16_t last)
      -> Expected<void, ActiveSessionError>;
  /// @brief Takes the oldest queued unit.
  ///
  /// @return The unit, or nothing.
  [[nodiscard]] auto take() -> std::optional<ActiveMediaEvent> {
    return events_.take();
  }
  /// @brief Takes the congestion counts.
  ///
  /// @param packet_loss_max The largest loss share to report.
  /// @return The report values.
  auto take_congestion(double packet_loss_max =
                           MediaPacketStats::default_packet_loss_max) noexcept
      -> takion::CongestionValues {
    return assembly_.take_congestion(packet_loss_max);
  }
  /// @brief Empties the queues and assembly.
  void clear() noexcept;
  /// @brief Units dropped from the queues.
  ///
  /// @return The count.
  auto dropped() const noexcept -> std::uint64_t { return events_.dropped(); }
  /// @brief Units the assemblers or dependencies refused.
  ///
  /// @return The count.
  auto rejected_units() const noexcept -> std::uint64_t {
    return rejected_units_;
  }
  /// @brief Whether frames are refused until a keyframe.
  ///
  /// @return True while waiting.
  auto waiting_for_idr() const noexcept -> bool {
    return dependencies_.waiting_for_idr();
  }
  /// @brief Frames lost since the last delivered one.
  ///
  /// @return The count.
  auto frames_lost() const noexcept -> std::int32_t { return frames_lost_; }
  /// @brief Changes whether Takion encodes unit counts only.
  ///
  /// @param count_only The new setting.
  void set_count_only(bool count_only) noexcept;
  /// @brief Discards every packet of the next video frame to start, as if
  /// the network had lost it, so loss recovery can be exercised.
  void drop_next_video_frame() noexcept { drop_requested_ = true; }

 private:
  /// @brief Applies frame dependencies to a video frame.
  ///
  /// @param video The frame; may be rewritten.
  /// @return True when it may be queued.
  [[nodiscard]] auto accept_video(takion::client_event::EncodedVideo& video)
      -> bool;
  /// @brief Queues the codec header of a picture size when the stream
  /// switches to it.
  ///
  /// @param index The size's index.
  /// @param frame_index The first frame in it.
  /// @param codec The codec byte.
  /// @return Nothing, or why queueing failed.
  [[nodiscard]] auto switch_profile(std::uint8_t index,
                                    std::uint16_t frame_index,
                                    std::uint8_t codec)
      -> Expected<void, ActiveSessionError>;
  /// @brief Whether a loss range is short enough to report.
  ///
  /// @param first The first frame.
  /// @param last The last frame.
  /// @return True when it spans less than half the index range.
  static auto reportable(std::uint16_t first, std::uint16_t last) noexcept
      -> bool;
  /// @brief Forgets every frame and asks for a keyframe, for a loss too long
  /// to report.
  ///
  /// @return Nothing, or why the request failed.
  [[nodiscard]] auto recover_unreportable_loss()
      -> Expected<void, ActiveSessionError>;
  /// @brief Asks for a keyframe when configured to.
  ///
  /// @return Nothing, or why the request failed.
  [[nodiscard]] auto request_idr() -> Expected<void, ActiveSessionError>;
  /// @brief Decides whether a video packet belongs to a frame being discarded
  /// on request, starting the discard at the first packet of a new frame.
  ///
  /// @param frame The packet's frame index.
  /// @return True when the packet is to be discarded.
  auto discard_requested(std::uint16_t frame) noexcept -> bool;
  /// @brief Asks for a keyframe unless one is outstanding.
  ///
  /// @return Nothing, or why the request failed.
  [[nodiscard]] auto send_idr() -> Expected<void, ActiveSessionError>;
  /// @brief Reports a lost range and forgets those frames.
  ///
  /// @param first The first frame.
  /// @param last The last frame.
  /// @return Nothing, or why reporting failed.
  [[nodiscard]] auto report_lost_range(std::uint16_t first, std::uint16_t last)
      -> Expected<void, ActiveSessionError>;
  /// @brief Reports a range lost to FEC, counts it and asks for a keyframe
  /// when configured.
  ///
  /// @param first The first frame.
  /// @param last The last frame.
  /// @return Nothing, or why reporting failed.
  [[nodiscard]] auto report_lost_frames(std::uint16_t first, std::uint16_t last)
      -> Expected<void, ActiveSessionError>;
  /// @brief Adds to the lost frame count, saturating.
  ///
  /// @param frames Frames lost.
  void count_lost(std::uint32_t frames) noexcept;
  /// @brief Records a delivered frame.
  ///
  /// @param frame The frame.
  void record_completion(std::uint16_t frame);
  /// @brief The picture's block count for a unit's size and codec.
  ///
  /// @param values The unit.
  /// @return 16-pixel blocks for H.264, 64-pixel for H.265; 0 for an unknown
  ///     size.
  auto expected_video_blocks(
      const takion::AvPacketValues& values) const noexcept -> std::uint32_t;

  /// @brief A delivered frame and the frame delivered before it.
  struct Completion {
    /// @brief The frame.
    std::uint16_t frame{};
    /// @brief The one before it.
    std::uint16_t previous{};
  };
  /// @brief How many delivered frames are remembered.
  static constexpr std::size_t completion_capacity = ring_capacity;

  /// @brief Where requests go.
  ActiveMediaSink& sink_;
  /// @brief How it works.
  ActiveMediaOptions options_;
  /// @brief The counters.
  StreamDiagnostics& diagnostics_;
  /// @brief The assemblers.
  CloudStreamMediaAssembly assembly_;
  /// @brief Frame dependencies.
  VideoDependencies dependencies_;
  /// @brief Media for the caller.
  ActiveMediaQueue events_;
  /// @brief Audio profile tracking.
  AudioProfileSelector audio_profiles_;
  /// @brief The party voice chat format last delivered.
  std::optional<AudioProfileChange> voice_format_;
  /// @brief Each picture size's codec header.
  std::vector<std::vector<std::byte>> profiles_;
  /// @brief Each picture size's width and height.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> profile_dimensions_;
  /// @brief Frames delivered recently.
  std::deque<Completion> completions_;
  /// @brief The picture size in use.
  std::optional<std::uint8_t> video_profile_;
  /// @brief Units refused.
  std::uint64_t rejected_units_{};
  /// @brief Frames lost since the last delivered one.
  std::int32_t frames_lost_{};
  /// @brief Whether the next video frame to start is to be discarded.
  bool drop_requested_{};
  /// @brief The video frame being discarded on request.
  std::optional<std::uint16_t> dropping_frame_;
  /// @brief When the last unit arrived, in monotonic microseconds.
  std::uint64_t now_us_{};
  /// @brief When the keyframe request being waited on was sent, in monotonic
  /// microseconds.
  std::uint64_t idr_sent_us_{};
  /// @brief Video frames refused in a row for a missing reference.
  std::uint32_t refusals_{};
};

/// @brief A received message the library does not understand, kept so a
/// cloud session can log it.
struct UnrecognisedMessage {
  /// @brief Where the message arrived.
  enum class Kind : std::uint8_t {
    /// @brief An unsupported control message.
    control,
    /// @brief A generic data transfer.
    generic_data,
    /// @brief A cloud server message; its text is in the metadata.
    server_message,
    /// @brief A deep link; its fields are described in the metadata.
    deep_link,
    /// @brief A cloud server's answer about the microphone; the type is the
    /// control message type and the metadata describes the answer.
    microphone,
    /// @brief How one of the client's generic data transfers went; the
    /// metadata says, and the type is the transfer's number.
    generic_data_outcome,
    /// @brief A received packet Takion rejected; the type is the
    /// takion::ClientError value, the metadata the rejecting source line and
    /// the bytes up to 64 of the packet.
    rejection
  };
  /// @brief How many rejections a session reports.
  static constexpr std::size_t reported_rejections = 20;
  /// @brief How many messages are kept; older ones are dropped.
  static constexpr std::size_t capacity = 16;

  /// @brief Where it arrived.
  Kind kind{Kind::control};
  /// @brief The control message type or the transfer's number.
  std::uint32_t type{};
  /// @brief The transfer's metadata; empty for control messages.
  std::string metadata;
  /// @brief The raw bytes.
  std::vector<std::byte> bytes;
};

/// @brief The unrecognised messages a session keeps until the caller takes
/// them, at most UnrecognisedMessage::capacity; the oldest is dropped beyond
/// that.
///
/// Not thread-safe.
class UnrecognisedMessages {
 public:
  /// @brief Keeps a message, dropping the oldest beyond the capacity.
  ///
  /// @param message The message.
  void keep(UnrecognisedMessage message) {
    if (messages_.size() == UnrecognisedMessage::capacity)
      messages_.pop_front();
    messages_.push_back(std::move(message));
  }
  /// @brief Takes the oldest message.
  ///
  /// @return The message, or nothing when none is waiting.
  [[nodiscard]] auto take() -> std::optional<UnrecognisedMessage> {
    if (messages_.empty()) return std::nullopt;
    auto message = std::move(messages_.front());
    messages_.pop_front();
    return message;
  }

 private:
  /// @brief The messages, oldest first.
  std::deque<UnrecognisedMessage> messages_;
};

/// @brief Times the three handshake windows, each allowed
/// handshake_stage_timeout_us, and remembers the refusal to report when one
/// times out after the console's answer was refused.
///
/// A window restarts when the handshake moves into another window; a refusal
/// is forgotten once the handshake leaves its window. Not thread-safe.
class ActiveHandshakeTimer {
 public:
  /// @brief Starts timing the connect window.
  ///
  /// @param started_us When it began, in monotonic microseconds.
  explicit ActiveHandshakeTimer(std::uint64_t started_us) noexcept
      : started_us_(started_us) {}

  /// @brief When the current window began.
  ///
  /// @return Monotonic microseconds.
  auto started_us() const noexcept -> std::uint64_t { return started_us_; }
  /// @brief Whether a phase's window has run out.
  ///
  /// @param state The phase.
  /// @param now_us The monotonic time, in microseconds.
  /// @return True inside a window older than handshake_stage_timeout_us.
  auto expired(ActiveHandshakeState state, std::uint64_t now_us) const noexcept
      -> bool;
  /// @brief When a phase's window runs out.
  ///
  /// @param state The phase.
  /// @return Monotonic microseconds; nothing outside the handshake.
  auto deadline_us(ActiveHandshakeState state) const noexcept
      -> std::optional<std::uint64_t>;
  /// @brief Why a window that ran out failed.
  ///
  /// @return The remembered refusal, or ActiveSessionError::handshake_timeout.
  auto timeout_cause() const noexcept -> ActiveSessionError {
    return refusal_.value_or(ActiveSessionError::handshake_timeout);
  }
  /// @brief Remembers a refusal to report if its window runs out.
  ///
  /// @param error The refusal.
  /// @param state The phase it happened in.
  void refused(ActiveSessionError error, ActiveHandshakeState state) noexcept {
    refusal_ = error;
    refused_stage_ = state;
  }
  /// @brief Follows the handshake to a phase, restarting the timer when the
  /// phase belongs to another window.
  ///
  /// @param state The phase now.
  /// @param now_us The monotonic time, in microseconds.
  /// @return True when the phase changed.
  auto moved_to(ActiveHandshakeState state, std::uint64_t now_us) noexcept
      -> bool;

 private:
  /// @brief The handshake windows timed by handshake_stage_timeout_us.
  enum class Window : std::uint8_t {
    /// @brief Not in the handshake.
    none,
    /// @brief Connecting and committing the version.
    connect,
    /// @brief Waiting for and verifying BANG, and installing the ciphers.
    bang,
    /// @brief Waiting for and checking the stream info.
    stream_info
  };

  /// @brief The window a handshake phase belongs to.
  ///
  /// @param state The phase.
  /// @return The window.
  static auto window(ActiveHandshakeState state) noexcept -> Window;

  /// @brief The phase the timer belongs to.
  ActiveHandshakeState timed_stage_{ActiveHandshakeState::awaiting_connection};
  /// @brief The failure to report if the window times out after a refusal.
  std::optional<ActiveSessionError> refusal_;
  /// @brief The phase of that refusal.
  ActiveHandshakeState refused_stage_{
      ActiveHandshakeState::awaiting_connection};
  /// @brief When the current window began, in monotonic microseconds.
  std::uint64_t started_us_;
};

/// @brief The seats of an active session: how many take part, the
/// controller type each is announced with, and the pad colours the console
/// sent, published as feedback when they change.
///
/// A colour sent for a seat not yet taking part is held until the seat is
/// admitted. Not thread-safe.
class ActivePads {
 public:
  /// @brief Makes the seats with one taking part.
  ///
  /// @param feedback Where colour changes are published; must outlive them.
  /// @param controller_type The type every seat is announced with until set.
  ActivePads(ActiveFeedbackQueue& feedback,
             takion::ControllerConnectionType controller_type) noexcept
      : feedback_(feedback), default_type_(controller_type) {
    types_.fill(controller_type);
  }

  /// @brief How many seats take part.
  ///
  /// @return 1 to 4.
  auto count() const noexcept -> std::uint8_t { return count_; }
  /// @brief Sets the controller type a seat is announced with; a seat out of
  /// range is ignored.
  ///
  /// @param pad The seat.
  /// @param type The Takion controller type value.
  void set_controller_type(std::uint8_t pad, std::uint8_t type) noexcept;
  /// @brief The controller type a seat is announced with.
  ///
  /// @param pad The seat.
  /// @return The type; the default for a seat out of range.
  auto controller_type(std::uint8_t pad) const noexcept
      -> takion::ControllerConnectionTypeValue;
  /// @brief Learns the host pad's colour and player index, and a seat's
  /// colour, from the console's pad info.
  ///
  /// @param info The pad info.
  void on_pad_info(const takion::client_event::PadInfo& info);
  /// @brief Lets a seat take part, publishing pad colours the console sent
  /// for it while it was not.
  ///
  /// @param pad The seat.
  /// @param publish_held Whether to publish the seat's own held colour.
  void admit(std::uint8_t pad, bool publish_held) noexcept;
  /// @brief Forgets a seat's pad colour.
  ///
  /// @param pad The seat.
  void release(std::uint8_t pad) noexcept;
  /// @brief A seat's pad colour.
  ///
  /// @param pad The seat.
  /// @return The colour, or black when unknown.
  auto led(std::uint8_t pad) const noexcept -> std::array<std::uint8_t, 3>;

 private:
  /// @brief Publishes a seat's pad colour when it changed.
  ///
  /// @param pad The seat.
  /// @param led The colour.
  void report_led(std::uint8_t pad, const std::array<std::uint8_t, 3>& led);

  /// @brief Where colour changes are published.
  ActiveFeedbackQueue& feedback_;
  /// @brief The type a seat out of range is announced with.
  takion::ControllerConnectionType default_type_;
  /// @brief The host pad's player index.
  std::uint8_t player_index_{};
  /// @brief The host pad's light bar colour.
  std::array<std::uint8_t, 3> led_state_{};
  /// @brief Each seat's pad colour.
  std::array<std::array<std::uint8_t, 3>, Pad::count> leds_{};
  /// @brief Whether each seat's colour is known.
  std::array<bool, Pad::count> led_known_{};
  /// @brief Colours the console sent for seats not yet taking part.
  std::array<std::optional<std::array<std::uint8_t, 3>>, Pad::count>
      held_leds_{};
  /// @brief Each seat's announced controller type.
  std::array<takion::ControllerConnectionTypeValue, Pad::count> types_{};
  /// @brief Seats taking part.
  std::uint8_t count_{1};
};

/// @brief The clocks that keep an active session alive: when the console
/// was last heard from, and when the last congestion report and heartbeat
/// were sent.
///
/// A congestion report is due every congestion interval, 200 ms unless the
/// stream info names another, and a heartbeat every heartbeat_interval_us.
/// Times are monotonic microseconds; a time before the clock it is compared
/// with is never due. Not thread-safe.
class ActiveKeepalive {
 public:
  /// @brief Starts the congestion clock.
  ///
  /// @param now_us The monotonic time, in microseconds.
  explicit ActiveKeepalive(std::uint64_t now_us) noexcept
      : last_congestion_us_(now_us) {}

  /// @brief When the console was last heard from.
  ///
  /// @return Monotonic microseconds; 0 before anything arrived.
  auto last_inbound_us() const noexcept -> std::uint64_t {
    return last_inbound_us_;
  }
  /// @brief Records that the console was heard from.
  ///
  /// @param now_us The monotonic time, in microseconds.
  /// @return True when nothing had arrived for network_trouble_gap_us.
  auto heard(std::uint64_t now_us) noexcept -> bool {
    const bool quiet = now_us >= last_inbound_us_ &&
                       now_us - last_inbound_us_ >= network_trouble_gap_us;
    last_inbound_us_ = now_us;
    return quiet;
  }
  /// @brief Restarts the heartbeat and silence clocks as the stream becomes
  /// ready.
  ///
  /// @param now_us The monotonic time, in microseconds.
  void ready(std::uint64_t now_us) noexcept {
    last_heartbeat_us_ = now_us;
    last_inbound_us_ = now_us;
  }
  /// @brief Takes the congestion interval a stream info names, clamped to
  /// the allowed range.
  ///
  /// @param interval_ms The interval, in milliseconds.
  void set_congestion_interval_ms(std::uint64_t interval_ms) noexcept {
    congestion_interval_us_ = std::clamp<std::uint64_t>(
        interval_ms * 1'000U, congestion_report_interval_min_us,
        congestion_report_interval_max_us);
  }
  /// @brief Whether nothing arrived for a silence timeout.
  ///
  /// @param now_us The monotonic time, in microseconds.
  /// @param timeout_us The timeout; 0 for none.
  /// @return True once the timeout passed since the console was heard from.
  auto silent(std::uint64_t now_us, std::uint64_t timeout_us) const noexcept
      -> bool {
    return timeout_us != 0 && now_us >= last_inbound_us_ &&
           now_us - last_inbound_us_ >= timeout_us;
  }
  /// @brief When a silence timeout ends.
  ///
  /// @param timeout_us The timeout.
  /// @return Monotonic microseconds.
  auto silence_deadline_us(std::uint64_t timeout_us) const noexcept
      -> std::uint64_t {
    return last_inbound_us_ + timeout_us;
  }
  /// @brief Whether a congestion report is due.
  ///
  /// @param now_us The monotonic time, in microseconds.
  /// @return True once the interval passed since the last report.
  auto congestion_due(std::uint64_t now_us) const noexcept -> bool {
    return now_us >= last_congestion_us_ &&
           now_us - last_congestion_us_ >= congestion_interval_us_;
  }
  /// @brief When the next congestion report is due.
  ///
  /// @return Monotonic microseconds.
  auto congestion_deadline_us() const noexcept -> std::uint64_t {
    return last_congestion_us_ + congestion_interval_us_;
  }
  /// @brief Records a congestion report sent.
  ///
  /// @param now_us The monotonic time, in microseconds.
  void congestion_sent(std::uint64_t now_us) noexcept {
    last_congestion_us_ = now_us;
  }
  /// @brief Whether a heartbeat is due.
  ///
  /// @param now_us The monotonic time, in microseconds.
  /// @return True once heartbeat_interval_us passed since the last one.
  auto heartbeat_due(std::uint64_t now_us) const noexcept -> bool {
    return now_us >= last_heartbeat_us_ &&
           now_us - last_heartbeat_us_ >= heartbeat_interval_us;
  }
  /// @brief When the next heartbeat is due.
  ///
  /// @return Monotonic microseconds.
  auto heartbeat_deadline_us() const noexcept -> std::uint64_t {
    return last_heartbeat_us_ + heartbeat_interval_us;
  }
  /// @brief Records a heartbeat sent.
  ///
  /// @param now_us The monotonic time, in microseconds.
  void heartbeat_sent(std::uint64_t now_us) noexcept {
    last_heartbeat_us_ = now_us;
  }

 private:
  /// @brief When the last heartbeat was sent.
  std::uint64_t last_heartbeat_us_{};
  /// @brief When the last congestion report was sent.
  std::uint64_t last_congestion_us_;
  /// @brief How often a congestion report is sent, in microseconds.
  std::uint64_t congestion_interval_us_{congestion_report_interval_us};
  /// @brief When the console was last heard from.
  std::uint64_t last_inbound_us_{};
};

/// @brief The running Takion stream of a session.
///
/// Owned through a unique_ptr; neither copyable nor movable. Not
/// thread-safe: the stream's worker pumps it and the play context calls the
/// post and take members from the same thread.
class ActiveSession final : private ActiveMediaSink {
 public:
  /// @brief Starts the Takion client and the handshake on a calibrated route.
  ///
  /// @param route The calibrated route; its transport is taken over.
  /// @param clock The monotonic clock; must outlive the session.
  /// @param threading Provider of the Takion client's mutex; must outlive
  ///     the session.
  /// @param random Randomness; must outlive the session.
  /// @param crypto The platform crypto provider; must outlive the session.
  /// @param takion_crypto The Takion crypto provider; must outlive the
  ///     session.
  /// @param handshake_key The 16-byte handshake key.
  /// @param prepared The client's ECDH key.
  /// @param session_key The session id.
  /// @param encrypted_launch_spec The sealed launch specification.
  /// @param config The Takion client configuration.
  /// @param control_profile How the session behaves.
  /// @return The session.
  /// @retval ActiveSessionError::start_failed Takion could not start.
  [[nodiscard]] static auto start(
      EstablishedRoute route, platform::ClockProvider& clock,
      platform::ThreadingProvider& threading,
      platform::SecureRandomProvider& random, platform::CryptoProvider& crypto,
      takion::CryptoProvider& takion_crypto,
      SecretArray<ecdh_handshake_key_size> handshake_key, PreparedEcdh prepared,
      std::shared_ptr<const SecretString> session_key,
      std::shared_ptr<const SecretString> encrypted_launch_spec,
      takion::ClientConfig config = {},
      ActiveControlProfile control_profile = {})
      -> Expected<std::unique_ptr<ActiveSession>, ActiveSessionError>;

  /// @brief Destroys the session, its Takion client first, since the client
  /// reads the media pipeline as its video gate.
  ~ActiveSession() override;
  ActiveSession(const ActiveSession&) = delete;
  ActiveSession& operator=(const ActiveSession&) = delete;
  ActiveSession(ActiveSession&&) = delete;
  ActiveSession& operator=(ActiveSession&&) = delete;

  /// @brief Advances the session as far as it can without waiting.
  ///
  /// @return Nothing while it runs.
  /// @retval ActiveSessionError Why it ended; see failure().
  [[nodiscard]] auto pump_once() -> Expected<void, ActiveSessionError>;
  /// @brief When the session next needs pumping for a timer.
  ///
  /// @return Monotonic microseconds, or nothing once stopped.
  auto next_deadline_us() const noexcept -> std::optional<std::uint64_t>;
  /// @brief When the session next has work that no datagram brings: its own
  /// timers or the Takion client's, such as queued commands and resends.
  ///
  /// @return Monotonic microseconds, or nothing once stopped with no timer.
  auto next_wake_us() const -> std::optional<std::uint64_t>;
  /// @brief Sets how long request_stop() keeps the Takion association open
  /// for its DISCONNECT to be acknowledged; 0, the default, closes at once.
  ///
  /// @param drain The longest wait.
  void set_disconnect_drain(std::chrono::microseconds drain) noexcept {
    disconnect_drain_ = drain;
  }
  /// @brief Passes on what a poll of the route found, so the next pump skips
  /// reading when the Takion socket was polled and found not readable.
  ///
  /// @param polled What was polled.
  /// @param readable Whether the poll found it readable.
  void note_route_readiness(const platform::Pollable& polled, bool readable);
  /// @brief Sends a controller's state.
  ///
  /// @param state The state.
  /// @return Nothing on success.
  /// @retval ActiveSessionError::not_ready Not ready or stopped.
  /// @retval ActiveSessionError::invalid_controller_state Out of range.
  /// @retval ActiveSessionError::post_failed Takion refused it.
  [[nodiscard]] auto post_controller_state(const ControllerState& state)
      -> Expected<void, ActiveSessionError>;
  /// @brief Sends one controller's history events, all of them new.
  ///
  /// @param controller_id The seat, 0 to 3.
  /// @param events The events, newest first.
  /// @return Nothing on success; nothing is sent for no events.
  /// @retval ActiveSessionError::not_ready Not ready or stopped.
  /// @retval ActiveSessionError::invalid_controller_state The seat is out of
  ///     range.
  /// @retval ActiveSessionError::post_failed Takion refused it.
  [[nodiscard]] auto post_controller_history_events(
      std::uint8_t controller_id,
      std::vector<takion::FeedbackHistoryEvent> events)
      -> Expected<void, ActiveSessionError>;
  /// @brief Sends a history window mixing seats.
  ///
  /// Every history event takes the next sequence number and a packet carries
  /// its newest event's number. The console applies as many events as the
  /// number moved since the last packet it took, so a resend keeps the number
  /// and is ignored unless the original was lost.
  ///
  /// @param events The events, newest first.
  /// @param controller_ids Each event's seat.
  /// @param fresh How many events are new since the last window, counting
  ///     events of windows that were never sent; 0 resends the last window.
  /// @return Nothing on success; nothing is sent for no events.
  /// @retval ActiveSessionError::not_ready Not ready or stopped.
  /// @retval ActiveSessionError::invalid_controller_state The lists differ in
  ///     length or a seat is out of range.
  /// @retval ActiveSessionError::post_failed Takion refused it.
  [[nodiscard]] auto post_controller_history_window(
      std::vector<takion::FeedbackHistoryEvent> events,
      std::vector<std::uint8_t> controller_ids, std::size_t fresh)
      -> Expected<void, ActiveSessionError>;
  /// @brief Sends a history window mixing seats, copying the events from
  /// views.
  ///
  /// @param events The events, newest first.
  /// @param controller_ids Each event's seat.
  /// @param fresh How many events are new since the last window; 0 resends
  ///     the last window.
  /// @return As the overload taking vectors.
  [[nodiscard]] auto post_controller_history_window(
      std::span<const takion::FeedbackHistoryEvent> events,
      std::span<const std::uint8_t> controller_ids, std::size_t fresh)
      -> Expected<void, ActiveSessionError>;
  /// @brief Sends the history events between two states of a controller.
  ///
  /// @param previous The state last sent.
  /// @param current The new state.
  /// @return As post_controller_history_events().
  [[nodiscard]] auto post_controller_history(const ControllerState& previous,
                                             const ControllerState& current)
      -> Expected<void, ActiveSessionError>;
  /// @brief Why stopping Takion failed, if it did.
  ///
  /// @return The error, or nothing.
  [[nodiscard]] auto stop_failure() const noexcept
      -> std::optional<takion::ClientError> {
    return stop_failure_;
  }
  /// @brief Stops the stream: drops queued media and sends a disconnect.
  ///
  /// @return Nothing on success, also when already stopped.
  /// @retval ActiveSessionError::pump_failed The disconnect failed; see
  ///     stop_failure().
  [[nodiscard]] auto request_stop() -> Expected<void, ActiveSessionError>;
  /// @brief Sends one microphone frame, with the tagged packet header when
  /// the negotiated AV layout tags audio.
  ///
  /// @param opus_frame A 40-byte Opus frame.
  /// @return Nothing on success, also while priming.
  /// @retval ActiveSessionError::not_ready Not ready or stopped.
  /// @retval ActiveSessionError::post_failed Takion refused it.
  [[nodiscard]] auto post_microphone(std::span<const std::byte> opus_frame)
      -> Expected<void, ActiveSessionError>;
  /// @brief Asks the console for a keyframe.
  ///
  /// @return Nothing on success.
  /// @retval ActiveSessionError::not_ready Not ready or stopped.
  /// @retval ActiveSessionError::post_failed Takion refused it (a full
  ///     mailbox is counted and does not end the session).
  [[nodiscard]] auto post_idr_request() -> Expected<void, ActiveSessionError>;
  /// @brief Discards the next video frame as if the network had lost it, to
  /// exercise loss recovery.
  ///
  /// @return Nothing on success.
  /// @retval ActiveSessionError::not_ready Not ready or stopped.
  [[nodiscard]] auto post_video_frame_drop()
      -> Expected<void, ActiveSessionError>;
  /// @brief Builds the history record of a keyboard state.
  ///
  /// @param modifiers The modifier bits.
  /// @param leds The keyboard LED bits, 0 to 7.
  /// @param keys The pressed keys as 16-bit codes.
  /// @return The record, or nothing for too many keys or bad LED bits.
  [[nodiscard]] static auto keyboard_record(std::uint8_t modifiers,
                                            std::uint8_t leds,
                                            std::span<const std::uint16_t> keys)
      -> std::optional<takion::FeedbackKind0Record>;
  /// @brief The seat keyboard records are sent for.
  static constexpr std::uint8_t keyboard_controller_id = 0;
  /// @brief Builds the history event that carries a keyboard state for seat
  /// 0, for the controller delivery to send with the other history events.
  ///
  /// @param modifiers The modifier bits.
  /// @param leds The keyboard LED bits, 0 to 7.
  /// @param keys The pressed keys.
  /// @return The event.
  /// @retval ActiveSessionError::not_ready Not ready or stopped.
  /// @retval ActiveSessionError::unsupported_request The Takion version
  ///     predates keyboard records.
  /// @retval ActiveSessionError::invalid_controller_state The state is out of
  ///     range.
  [[nodiscard]] auto keyboard_event(std::uint8_t modifiers, std::uint8_t leds,
                                    std::span<const std::uint16_t> keys) const
      -> Expected<takion::FeedbackHistoryEvent, ActiveSessionError>;
  /// @brief Sends an audio state message.
  ///
  /// @param kind What the body describes.
  /// @param data The body.
  /// @return Nothing on success.
  /// @retval ActiveSessionError::not_ready Not ready or stopped.
  /// @retval ActiveSessionError::unsupported_request The features cannot
  ///     carry it.
  /// @retval ActiveSessionError::post_failed Takion refused it.
  [[nodiscard]] auto post_audio_state(AudioStateKind kind,
                                      std::span<const std::byte> data)
      -> Expected<void, ActiveSessionError>;
  /// @brief Sends a blob by generic data, as a cloud gateway message travels.
  ///
  /// @param data The blob.
  /// @param metadata Its metadata.
  /// @return Nothing on success.
  /// @retval ActiveSessionError::not_ready Not ready or stopped.
  /// @retval ActiveSessionError::post_failed Takion refused it.
  [[nodiscard]] auto post_generic_data(std::vector<std::byte> data,
                                       std::string metadata)
      -> Expected<void, ActiveSessionError>;
  /// @brief Sends a deep link as a DIRECTMESSAGE to the cloud service.
  ///
  /// @param link The deep link.
  /// @return Nothing on success.
  /// @retval ActiveSessionError::not_ready Not ready or stopped.
  /// @retval ActiveSessionError::post_failed Takion refused it.
  [[nodiscard]] auto post_deep_link(const takion::DeepLink& link)
      -> Expected<void, ActiveSessionError>;
  /// @brief Connects the microphone of controller slot 0 to a PS5 cloud
  /// service with MICCONNECTION.
  ///
  /// @return Nothing on success.
  /// @retval ActiveSessionError::not_ready Not ready or stopped.
  /// @retval ActiveSessionError::post_failed Takion refused it.
  [[nodiscard]] auto post_microphone_connection()
      -> Expected<void, ActiveSessionError>;
  /// @brief Opens or closes the cloud console's system menu for one pad
  /// with XMBCOMMAND TOGGLE, which is how LegacyCloudStream takes the PS
  /// button, naming the pad whose user pressed it.
  ///
  /// @param pad The pressing controller's pad, 0 for player one.
  /// @return Nothing on success.
  /// @retval ActiveSessionError::not_ready Not ready or stopped.
  /// @retval ActiveSessionError::post_failed Takion refused it.
  [[nodiscard]] auto post_system_menu_toggle(std::uint32_t pad)
      -> Expected<void, ActiveSessionError>;
  /// @brief Tells a PS5 cloud service the microphone of controller slot 0 is
  /// muted or unmuted, with a MICINFO direct message. Without direct message
  /// encryption the service cannot read it, so nothing is sent.
  ///
  /// @param muted Whether the microphone is muted.
  /// @return Nothing on success.
  /// @retval ActiveSessionError::not_ready Not ready or stopped.
  /// @retval ActiveSessionError::post_failed Takion refused it.
  [[nodiscard]] auto post_microphone_mute(bool muted)
      -> Expected<void, ActiveSessionError>;
  /// @brief Reports frames the caller could not decode.
  ///
  /// @param first The first bad frame.
  /// @param last The last bad frame.
  /// @return Nothing, or why reporting failed.
  [[nodiscard]] auto report_video_loss(std::uint16_t first, std::uint16_t last)
      -> Expected<void, ActiveSessionError>;
  /// @brief Takion's state.
  ///
  /// @return The snapshot.
  auto snapshot() const -> takion::Snapshot;
  /// @brief Why the session ended.
  ///
  /// @return The failure, or nothing.
  [[nodiscard]] auto failure() const noexcept
      -> const std::optional<ActiveSessionFailure>& {
    return failure_;
  }
  /// @brief Sets where steps are traced.
  ///
  /// @param trace The trace, or null; must outlive the session.
  void set_diagnostics(const diagnostics::Trace* trace) noexcept {
    trace_ = trace;
  }
  /// @brief When anything last arrived from the console.
  ///
  /// @return Monotonic microseconds; 0 before anything arrived.
  auto last_inbound_us() const noexcept -> std::uint64_t {
    return keepalive_.last_inbound_us();
  }
  /// @brief The handshake phase.
  ///
  /// @return The state.
  auto state() const noexcept -> ActiveHandshakeState {
    return handshake_.state();
  }
  /// @brief The accepted stream info.
  ///
  /// @return The info, or null before it is accepted.
  auto ready_info() const noexcept -> const ValidatedStreamInfo* {
    return ready_info_ ? &*ready_info_ : nullptr;
  }
  /// @brief Takes the oldest queued media.
  ///
  /// @return The media, or nothing.
  [[nodiscard]] auto take_media_event() -> std::optional<ActiveMediaEvent>;
  /// @brief Takes the oldest controller feedback.
  ///
  /// @return The feedback, or nothing.
  [[nodiscard]] auto take_feedback_event()
      -> std::optional<ActiveFeedbackEvent> {
    return feedback_events_.take();
  }
  /// @brief Takes the latest play time warning from a cloud service.
  ///
  /// @return The warning, or nothing when none arrived since the last call.
  [[nodiscard]] auto take_play_time_left() noexcept
      -> std::optional<takion::PlayTimeLeft> {
    return std::exchange(play_time_left_, std::nullopt);
  }
  /// @brief Takes the oldest unrecognised message.
  ///
  /// @return The message, or nothing when none is waiting.
  [[nodiscard]] auto take_unrecognised() -> std::optional<UnrecognisedMessage> {
    return unrecognised_.take();
  }
  /// @brief Media dropped from the queues.
  ///
  /// @return The count.
  auto dropped_media_events() const noexcept -> std::uint64_t {
    return media_.dropped();
  }
  /// @brief Media units refused.
  ///
  /// @return The count.
  auto rejected_media_units() const noexcept -> std::uint64_t {
    return media_.rejected_units();
  }
  /// @brief The counters and Takion's link measurements.
  ///
  /// @return The snapshot; StreamDiagnostics::available is false when Takion's
  ///     measurements could not be copied.
  auto diagnostics() const noexcept -> StreamDiagnostics;
  /// @brief How the session behaves.
  ///
  /// @return The profile.
  auto control_profile() const noexcept -> const ActiveControlProfile& {
    return control_profile_;
  }
  /// @brief The Takion version the session settled on.
  ///
  /// @return The version, or nothing before it is committed.
  auto committed_takion_version() const noexcept
      -> std::optional<std::uint32_t> {
    return committed_version_;
  }
  /// @brief How many seats take part.
  ///
  /// @return 1 to 4.
  auto couch_pad_count() const noexcept -> std::uint8_t {
    return pads_.count();
  }
  /// @brief Sets the controller type a seat is announced with.
  ///
  /// @param pad The seat.
  /// @param type The Takion controller type value.
  void set_pad_controller_type(std::uint8_t pad, std::uint8_t type) noexcept;
  /// @brief The controller type a seat is announced with.
  ///
  /// @param pad The seat.
  /// @return The type.
  auto pad_controller_type(std::uint8_t pad) const noexcept
      -> takion::ControllerConnectionTypeValue;
  /// @brief Announces a seat's controller to the console.
  ///
  /// @param pad The seat.
  /// @param uses_controller_id Whether to state the seat's id (when more than
  ///     one seat takes part).
  /// @return Nothing on success.
  /// @retval ActiveSessionError::not_ready Stopped or the seat is out of
  ///     range.
  /// @retval ActiveSessionError::post_failed Takion refused it.
  [[nodiscard]] auto announce_pad(std::uint8_t pad, bool uses_controller_id)
      -> Expected<void, ActiveSessionError>;
  /// @brief Announces every seat taking part.
  ///
  /// @param session_pad_count The seats the caller expects.
  /// @return As announce_pad().
  [[nodiscard]] auto announce_pads(std::uint8_t session_pad_count)
      -> Expected<void, ActiveSessionError>;
  /// @brief Tells the console a seat's controller is gone.
  ///
  /// @param pad The seat.
  /// @return As announce_pad().
  [[nodiscard]] auto disconnect_pad(std::uint8_t pad)
      -> Expected<void, ActiveSessionError>;
  /// @brief Lets a seat take part, publishing pad colours the console sent
  /// for it while it was not.
  ///
  /// @param pad The seat.
  /// @param publish_held Whether to publish the seat's own held colour.
  void admit_pad(std::uint8_t pad, bool publish_held) noexcept;
  /// @brief Forgets a seat's pad colour.
  ///
  /// @param pad The seat.
  void release_pad(std::uint8_t pad) noexcept;
  /// @brief A seat's pad colour.
  ///
  /// @param pad The seat.
  /// @return The colour, or black when unknown.
  auto pad_led(std::uint8_t pad) const noexcept -> std::array<std::uint8_t, 3>;

 private:
  /// @brief The highest keyboard LED value.
  static constexpr std::uint8_t keyboard_leds_max = 7;

  /// @brief Makes a session around a handshake.
  ///
  /// @param handshake The handshake.
  /// @param clock The monotonic clock.
  /// @param crypto The platform crypto provider.
  /// @param takion_crypto The Takion crypto provider.
  /// @param handshake_key The 16-byte handshake key.
  /// @param control_profile How the session behaves.
  ActiveSession(ActiveHandshake handshake, platform::ClockProvider& clock,
                platform::CryptoProvider& crypto,
                takion::CryptoProvider& takion_crypto,
                SecretArray<ecdh_handshake_key_size> handshake_key,
                ActiveControlProfile control_profile) noexcept;
  /// @brief Whether a Takion event shows the console is there.
  ///
  /// @param event The event.
  /// @return False for the client's own sends, closes and failures.
  static auto inbound_activity(const takion::Event& event) noexcept -> bool;
  /// @brief Copies Takion's link measurements into diagnostics.
  ///
  /// @param result The diagnostics, changed in place.
  /// @param link The measurements.
  static void apply_link_metrics(StreamDiagnostics& result,
                                 const takion::LinkMetrics& link) noexcept;
  /// @brief Stops Takion with a disconnect, once.
  void stop_route_once() noexcept;
  /// @brief Tells the console a range of frames is corrupt.
  ///
  /// @param first The first frame.
  /// @param last The last frame.
  /// @return Nothing on success.
  /// @retval ActiveSessionError::not_ready Not ready or stopped.
  [[nodiscard]] auto post_corrupt_frame(std::uint16_t first, std::uint16_t last)
      -> Expected<void, ActiveSessionError> override;
  /// @brief Asks for a keyframe after a loss.
  ///
  /// @return True when sent, false when the mailbox was full.
  /// @retval ActiveSessionError::post_failed Takion refused it.
  [[nodiscard]] auto post_idr_after_loss()
      -> Expected<bool, ActiveSessionError> override;
  /// @brief Queues an FEC failure notice.
  ///
  /// @param frame_index The last frame of the loss.
  /// @param idr_request_sent Whether a keyframe request is outstanding.
  void on_video_fec_failure(std::uint16_t frame_index,
                            bool idr_request_sent) override;
  /// @brief Posts a keyframe request.
  ///
  /// @return Takion's result.
  [[nodiscard]] auto post_idr() -> takion::Result<void>;
  /// @brief Learns pad feedback from the console's pad info.
  ///
  /// @param info The pad info.
  void on_pad_info(const takion::client_event::PadInfo& info);
  /// @brief Posts a history packet whose newest event is `fresh` numbers past
  /// the last one sent, and counts those events numbered once Takion took
  /// it.
  ///
  /// @param events The events, newest first.
  /// @param controller_id The seat of every event when `controller_ids` is
  ///     empty.
  /// @param controller_ids Each event's seat, or empty.
  /// @param fresh How many events are new.
  /// @return Nothing on success.
  /// @retval ActiveSessionError::post_failed Takion refused it.
  [[nodiscard]] auto post_history_packet(
      std::vector<takion::FeedbackHistoryEvent> events,
      std::uint8_t controller_id, std::vector<std::uint8_t> controller_ids,
      std::size_t fresh) -> Expected<void, ActiveSessionError>;
  /// @brief Settles one event Takion produced during a pump: notes inbound
  /// activity, drives the handshake, passes media on and keeps feedback.
  ///
  /// @param event The event; its payload may be moved out.
  /// @param now The pump's monotonic time, in microseconds.
  /// @return Nothing, or why the session ended.
  [[nodiscard]] auto on_takion_event(takion::Event& event, std::uint64_t now)
      -> Expected<void, ActiveSessionError>;
  /// @brief Settles a control message from the console: a disconnect, a
  /// heartbeat to echo, cloud notices, the stream info and BANG.
  ///
  /// @param control The message; its payload may be moved out.
  /// @return Nothing, or why the session ended.
  [[nodiscard]] auto on_control(takion::client_event::ControlReceived& control)
      -> Expected<void, ActiveSessionError>;
  /// @brief Authenticates BANG and installs the ciphers it carries; a refusal
  /// while BANG is awaited waits for the console to resend.
  ///
  /// @param bang The BANG.
  /// @return Nothing, or why the session ended.
  [[nodiscard]] auto on_bang(takion::Bang bang)
      -> Expected<void, ActiveSessionError>;
  /// @brief Takes an AV unit unless its kind is dropped: video goes through
  /// the reorder queue, audio and haptics straight to the pipeline, once the
  /// stream is ready.
  ///
  /// @param values The unit; moved on once ready.
  /// @return Nothing, or why the session ended.
  [[nodiscard]] auto on_av_unit(takion::AvPacketValues& values)
      -> Expected<void, ActiveSessionError>;
  /// @brief Records that the console was heard from, marking network trouble
  /// when a ready stream heard nothing for network_trouble_gap_us.
  ///
  /// @param now The pump's monotonic time, in microseconds.
  void note_inbound(std::uint64_t now) noexcept;
  /// @brief Counts a packet Takion rejected, keeping the first few for the
  /// log; a transient send error only counts as a dropped send.
  ///
  /// @param rejected The rejection.
  void note_rejection(const takion::client_event::PacketRejected& rejected);
  /// @brief Describes how one of the client's generic data transfers ended.
  ///
  /// @param ended The transfer's end.
  /// @return The outcome as text.
  static auto describe(
      const takion::client_event::GenericDataTransferEnded& ended)
      -> std::string;
  /// @brief Describes a deep link for the log of unrecognised messages.
  ///
  /// @param link The deep link.
  /// @return Its fields as text.
  static auto describe(const takion::DeepLink& link) -> std::string;
  /// @brief Describes a cloud server's microphone connection answer.
  ///
  /// @param connection The answer.
  /// @return Its fields as text.
  static auto describe(const takion::MicConnection& connection) -> std::string;
  /// @brief Describes a cloud server's microphone information.
  ///
  /// @param microphone The information.
  /// @return Its fields as text.
  static auto describe(const takion::MicInfo& microphone) -> std::string;
  /// @brief Passes on the video units the reorder queue stopped waiting for,
  /// while the stream is ready.
  ///
  /// @return Nothing, or the first failure.
  [[nodiscard]] auto flush_reorder_queue()
      -> Expected<void, ActiveSessionError>;
  /// @brief Checks a stream info held back until the ciphers were installed,
  /// once they are.
  ///
  /// @return Nothing, or why the handshake failed.
  [[nodiscard]] auto accept_buffered_stream_info()
      -> Expected<void, ActiveSessionError>;
  /// @brief Restarts the handshake window timer when the phase moved to
  /// another window, and starts the keepalive clocks once ready.
  ///
  /// @param current The phase after the pump.
  /// @param now The pump's monotonic time, in microseconds.
  void retime_stage(ActiveHandshakeState current, std::uint64_t now) noexcept;
  /// @brief Ends a silent stream, sends the due congestion report and
  /// heartbeat, and flushes Takion's queued commands.
  ///
  /// @param current The phase after the pump.
  /// @param now The pump's monotonic time, in microseconds.
  /// @return Nothing, or why the session ended.
  [[nodiscard]] auto keep_alive(ActiveHandshakeState current, std::uint64_t now)
      -> Expected<void, ActiveSessionError>;
  /// @brief Drains the inbox for feedback only, after Takion failed.
  void keep_feedback_events();
  /// @brief Keeps a Takion event when it is controller feedback.
  ///
  /// @param event The event.
  void keep_feedback_event(takion::Event& event);
  /// @brief Ends the session, keeping the first failure.
  ///
  /// @param error Why.
  /// @param transport Takion's error.
  /// @param peer_reason The console's disconnect reason.
  /// @return The first failure's cause.
  [[nodiscard]] auto fail(ActiveSessionError error,
                          std::optional<takion::ClientError> transport = {},
                          std::optional<std::string> peer_reason = {})
      -> Expected<void, ActiveSessionError>;
  /// @brief Passes an AV unit to the pipeline.
  ///
  /// @param values The unit, moved on to the pipeline.
  /// @return As ActiveMediaPipeline::consume().
  [[nodiscard]] auto consume_av_unit(takion::AvPacketValues values)
      -> Expected<void, ActiveSessionError>;
  /// @brief Passes the units the reorder queue released to the pipeline.
  ///
  /// @return Nothing, or the first failure.
  [[nodiscard]] auto consume_reordered_units()
      -> Expected<void, ActiveSessionError>;
  /// @brief Posts a command the caller asked for: a full mailbox is counted
  /// and refused, any other refusal ends the session.
  ///
  /// @param command The command.
  /// @return Nothing, or ActiveSessionError::post_failed.
  [[nodiscard]] auto post_request(takion::TakionCommand command)
      -> Expected<void, ActiveSessionError>;
  /// @brief Posts a command, counting a full mailbox rather than failing.
  ///
  /// @param command The command.
  /// @return Nothing, or ActiveSessionError::post_failed.
  [[nodiscard]] auto post_best_effort(takion::TakionCommand command)
      -> Expected<void, ActiveSessionError>;
  /// @brief Checks the stream info, acknowledges it, announces the pads and
  /// the microphone format.
  ///
  /// An unusable stream info or a refused announcement waits for the console
  /// to resend.
  ///
  /// @param info The stream info.
  /// @return Nothing, or ActiveSessionError::handshake_failed.
  [[nodiscard]] auto validate_and_ack(takion::StreamInfo info)
      -> Expected<void, ActiveSessionError>;

  /// @brief Takion's events.
  ActiveEventInbox inbox_;
  /// @brief The Takion client.
  std::unique_ptr<ActiveTakion> takion_;
  /// @brief How long request_stop() waits for its DISCONNECT to be
  /// acknowledged.
  std::chrono::microseconds disconnect_drain_{0};
  /// @brief The handshake.
  ActiveHandshake handshake_;
  /// @brief The monotonic clock.
  platform::ClockProvider& clock_;
  /// @brief Where steps are traced.
  const diagnostics::Trace* trace_{};
  /// @brief The phase last traced.
  std::optional<ActiveHandshakeState> logged_stage_;
  /// @brief Traces the phase when it changed.
  void trace_state() noexcept;
  /// @brief Records the wait since the previous read of the socket, and the
  /// socket's receive buffer on the first read.
  ///
  /// @param now_us The current monotonic time in microseconds.
  void note_read(std::uint64_t now_us) noexcept;
  /// @brief A wait between reads longer than this, one frame at 60 fps, is
  /// counted as late.
  static constexpr std::uint64_t late_read_us = 16'667;
  /// @brief When the socket was last read, in monotonic microseconds.
  std::optional<std::uint64_t> last_read_us_;
  /// @brief Whether the socket's receive buffer has been read.
  bool receive_buffer_read_{};
  /// @brief Times each handshake window and remembers its last refusal.
  ActiveHandshakeTimer stage_timer_;
  /// @brief The phase reported on failure.
  ActiveHandshakeState failure_stage_{
      ActiveHandshakeState::awaiting_connection};
  /// @brief When the console was last heard from, and when the next
  /// congestion report and heartbeat are due.
  ActiveKeepalive keepalive_;
  /// @brief Why the session ended.
  std::optional<ActiveSessionFailure> failure_;
  /// @brief Why stopping Takion failed.
  std::optional<takion::ClientError> stop_failure_;
  /// @brief The platform crypto provider.
  platform::CryptoProvider& crypto_;
  /// @brief The Takion crypto provider.
  takion::CryptoProvider& takion_crypto_;
  /// @brief The handshake key.
  SecretArray<ecdh_handshake_key_size> handshake_key_;
  /// @brief How the session behaves.
  ActiveControlProfile control_profile_;
  /// @brief The accepted stream info.
  std::optional<ValidatedStreamInfo> ready_info_;
  /// @brief Controller feedback for the caller.
  ActiveFeedbackQueue feedback_events_;
  /// @brief Video units back in order.
  VideoReorderQueue video_reorder_;
  /// @brief Units the reorder queue released.
  std::vector<takion::AvPacketValues> reordered_units_;
  /// @brief The counters.
  StreamDiagnostics diagnostics_{.available = true};
  /// @brief The media pipeline.
  ActiveMediaPipeline media_;
  /// @brief The console's haptics intensity.
  std::uint8_t haptic_intensity_{1};
  /// @brief The console's trigger intensity.
  std::uint8_t trigger_intensity_{1};
  /// @brief The console's motion settings.
  std::optional<std::array<std::uint8_t, 2>> motion_settings_;
  /// @brief The latest play time warning not yet taken.
  std::optional<takion::PlayTimeLeft> play_time_left_;
  /// @brief Unrecognised messages not yet taken.
  UnrecognisedMessages unrecognised_;
  /// @brief Keeps a cloud server's latest measurements in the diagnostics.
  ///
  /// @param overlay The METRICSOVERLAY.
  void note_service_metrics(const takion::MetricsOverlay& overlay) noexcept;
  /// @brief The seats taking part, their controller types and colours.
  ActivePads pads_;
  /// @brief The next feedback state sequence number.
  std::uint16_t feedback_sequence_{};
  /// @brief How many history events were numbered; the newest sent took
  /// sequence number one less, modulo 65536.
  std::uint64_t history_numbered_{};
  /// @brief The microphone packer.
  MicrophoneSender microphone_;
  /// @brief Whether the session stopped, whether Takion was lost before it
  /// connected, and whether queued media survives the stop.
  ActiveRouteLifecycle route_;
  /// @brief The committed Takion version.
  std::optional<std::uint32_t> committed_version_;
  /// @brief Commits the Takion version, replacing the ECDH key when its curve
  /// differs, and sends BIG.
  ///
  /// @param version The version.
  /// @return Nothing, or why the handshake failed.
  [[nodiscard]] auto commit_profile(std::uint32_t version)
      -> Expected<void, ActiveSessionError>;
};

}  // namespace libchiaking::net::protocol::remote_play
