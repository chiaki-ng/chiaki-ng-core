/// @file
/// @brief Declares MediaDelivery, which hands a session's encoded video,
/// audio and controller feedback to the frontend's MediaSink and keeps the
/// first media fault that should end the session.
#pragma once

#include "libchiaking/detail/media/media_sink.hpp"
#include "libchiaking/detail/play/outcome.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/media/video_intake.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/play/adapters/session/rest_request.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace libchiaking::play {

/// @brief The stage names a media fault is reported with, one per place media
/// can fail. Plain data; the defaults are the names shown to the player.
struct MediaStages {
  /// @brief Stage of a stream description whose video profiles are invalid.
  std::string profile_validation{"Video stream profile validation"};
  /// @brief Stage of a frame that is empty, too large or names no profile.
  std::string frame_validation{"Video frame metadata validation"};
  /// @brief Stage of a decoder that could not be set up for the format.
  std::string format_setup{"Video decoder format setup"};
  /// @brief Stage of a decoder that refused a frame.
  std::string submission{"Video decoder submission"};
  /// @brief Stage of a keyframe or loss report the link could not send.
  std::string recovery_request{"Video recovery request"};
  /// @brief Stage of a decoder that failed while presenting.
  std::string decoder_output{"Video decoder output"};
  /// @brief Stage of audio output that failed and is set to end the run.
  std::string audio_output{"Audio output"};
};

/// @brief How a MediaDelivery validates and reports. Plain data.
struct MediaSettings {
  /// @brief Limits on profiles, headers, frames and keyframe pacing.
  media::VideoLimits video{};
  /// @brief The stage names faults are reported with.
  MediaStages stages{};
  /// @brief Whether an audio output failure ends the session; otherwise it is
  /// only counted and logged.
  bool audio_failure_ends_run{};
};

/// @brief The session's media path: validates what the link delivers, passes
/// it to the frontend's MediaSink and records the first fault.
///
/// Video passes through a VideoIntake that tracks profiles, losses and
/// keyframe requests. A fault is kept once; later ones are ignored, and none
/// is kept after MediaDelivery::seal. The session loop reads the fault and
/// ends the run. Deliveries come from the link's thread, presentation reports
/// from the frontend's decoder thread. The video intake takes no lock; the
/// fault, the seal and the audio failure count are guarded by a mutex, which
/// a frame takes only when it records a fault. The sink and log must outlive
/// it. Not copyable.
class MediaDelivery {
 public:
  /// @brief The cause recorded for validation faults, which have no decoder
  /// cause of their own.
  static constexpr int validation_cause = 1;

  /// @brief Builds the media path.
  ///
  /// @param sink The frontend's sink; must outlive this object.
  /// @param threading Provider of the path's fault mutex; must outlive this
  ///     object.
  /// @param clock The monotonic clock keyframe requests are paced by; must
  ///     outlive this object.
  /// @param log Receives warnings and errors; must outlive this object.
  /// @param settings Limits, stage names and the audio failure rule.
  MediaDelivery(media::MediaSink& sink, platform::ThreadingProvider& threading,
                platform::ClockProvider& clock,
                platform::DiagnosticsProvider& log,
                MediaSettings settings = {});

  /// @brief Not copyable; one path per session.
  MediaDelivery(const MediaDelivery&) = delete;
  /// @brief Not copy-assignable; one path per session.
  auto operator=(const MediaDelivery&) -> MediaDelivery& = delete;

  /// @brief Takes the stream's description, checks its video profiles and
  /// passes the description to the sink.
  ///
  /// The sink is told even when the profiles are invalid.
  ///
  /// @param info The link's stream description.
  /// @return `true` when the profiles are valid; `false` after recording a
  ///     profile validation fault.
  auto configure(const net::protocol::remote_play::event::StreamInfoReady& info)
      -> bool;
  /// @brief Passes one video frame to the sink and settles its result.
  ///
  /// Calls into the sink, which must not block. Records a frame validation,
  /// format or submission fault when one occurs, and logs decoder rejections
  /// and drops on powers of two. Takes no lock unless a fault is recorded or a
  /// line is logged.
  ///
  /// @param frame The encoded frame.
  void deliver(media::VideoFrame frame);
  /// @brief Passes an audio unit or controller feedback to the sink.
  ///
  /// Video units are ignored here. An FEC failure tells the video intake, which
  /// may ask for a keyframe, and tells the sink of the gap. A failed audio
  /// output is counted and, when so configured, recorded as a fault.
  ///
  /// @param media The encoded unit.
  void deliver(net::protocol::remote_play::event::EncodedMedia&& media);

  /// @brief Records that the frontend presented a frame.
  ///
  /// @return The frames lost and whether one was recovered since the last
  ///     presentation, which resets.
  auto presented() noexcept -> media::PresentationLoss;
  /// @brief Records that the frontend's decoder rejected a frame, so it is
  /// reported as lost; logged on powers of two.
  ///
  /// @param frame The frame index.
  /// @param cause The decoder's own cause.
  void rejected(std::uint16_t frame, int cause);
  /// @brief Records that the frontend's decoder failed while presenting.
  ///
  /// @param cause The decoder's own cause.
  void output_failed(int cause);
  /// @brief Records that a keyframe or loss report could not be sent.
  ///
  /// @param cause The link's cause.
  void recovery_failed(int cause);

  /// @brief Reads the recorded fault.
  ///
  /// @return The first fault; empty when none was recorded or it was pardoned.
  auto fault() const -> std::optional<media::MediaFault>;
  /// @brief Forgets the recorded fault, for a stream that stopped because the
  /// console went to rest.
  ///
  /// @return The fault that was forgotten; empty when there was none.
  auto pardon() -> std::optional<media::MediaFault>;
  /// @brief Stops recording faults; called as the session finishes.
  void seal();
  /// @brief Reads how many times audio output failed.
  ///
  /// @return The count since the session began.
  auto audio_failures() const -> std::uint64_t;

  /// @brief Gives the video intake.
  ///
  /// @return The intake, owned by this object.
  auto video() noexcept -> media::VideoIntake& { return video_; }
  /// @brief Gives the video intake.
  ///
  /// @return The intake, owned by this object.
  auto video() const noexcept -> const media::VideoIntake& { return video_; }
  /// @brief Gives the stage names.
  ///
  /// @return The names this path reports with.
  auto stages() const noexcept -> const MediaStages& { return stages_; }

 private:
  /// @brief Records a fault unless one is held or the path is sealed, and logs
  /// it.
  ///
  /// @param kind What failed.
  /// @param stage The stage name reported.
  /// @param cause The decoder's or link's cause.
  void fail(media::MediaFailure kind, std::string_view stage, int cause);
  /// @brief Counts an audio output failure and records it as a fault when audio
  /// failures end the run; otherwise logs it on powers of two.
  ///
  /// @param cause The sink's cause.
  void audio_failed(int cause);
  /// @brief Writes one line to the log.
  ///
  /// @param severity The line's severity.
  /// @param message The line.
  void log(platform::LogSeverity severity,
           std::string_view message) const noexcept;

  /// @brief The frontend's sink.
  media::MediaSink& sink_;
  /// @brief Receives warnings and errors.
  platform::DiagnosticsProvider& log_;
  /// @brief The stage names reported.
  MediaStages stages_;
  /// @brief Whether an audio failure ends the run.
  bool audio_failure_ends_run_;
  /// @brief Tracks profiles, losses and keyframe requests.
  media::VideoIntake video_;
  /// @brief Guards the fault, the seal and the audio failure count.
  mutable platform::Mutex mutex_;
  /// @brief The first fault recorded.
  std::optional<media::MediaFault> fault_;
  /// @brief Set by MediaDelivery::seal; no fault is recorded afterwards.
  bool sealed_{};
  /// @brief Audio output failures since the session began.
  std::uint64_t audio_failures_{};
};

}  // namespace libchiaking::play
