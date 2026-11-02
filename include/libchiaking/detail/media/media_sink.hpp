/// @file
/// @brief Declares MediaSink, through which a session hands video, audio and
/// controller feedback to the frontend, and the units it delivers.
#pragma once

#include "libchiaking/detail/controllers/controller_feedback.hpp"
#include "libchiaking/detail/controllers/seat_number.hpp"
#include "libchiaking/detail/media/video_codec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::media {

class VideoIntake;

/// @brief Where the speaker for one audio channel sits.
enum class AudioChannelPosition : std::uint8_t {
  /// @brief The stream did not say.
  unknown = 0,
  /// @brief Front left.
  front_left = 1,
  /// @brief Front right.
  front_right = 2,
  /// @brief Front centre.
  front_center = 3,
  /// @brief Low-frequency effects.
  lfe = 4,
  /// @brief Side left.
  side_left = 5,
  /// @brief Side right.
  side_right = 6,
  /// @brief Back left.
  back_left = 7,
  /// @brief Back right.
  back_right = 8,
  /// @brief Top front left.
  top_front_left = 9,
  /// @brief Top front right.
  top_front_right = 10,
  /// @brief Top back left.
  top_back_left = 11,
  /// @brief Top back right.
  top_back_right = 12,
  /// @brief The single channel of a mono stream.
  mono = 13,
};

/// @brief The format of a session's audio.
///
/// Pass it to AudioDecoder::create, or use it to configure another Opus
/// decoder.
struct AudioFormat {
  /// @brief Largest number of channels a format can describe.
  static constexpr std::size_t max_channels = 12;

  /// @brief Number of channels.
  std::uint8_t channels{};
  /// @brief Bits per sample.
  std::uint8_t bits{};
  /// @brief Sample rate, in hertz.
  std::uint32_t rate{};
  /// @brief Samples per channel in one audio packet.
  std::uint32_t frame_size{};
  /// @brief Number of Opus streams in a multistream packet; 0 when the stream
  /// did not say, in which case the usual layout for the channel count
  /// applies.
  std::uint8_t streams{};
  /// @brief How many of those Opus streams are stereo pairs.
  std::uint8_t coupled_streams{};
  /// @brief Opus multistream channel mapping; the first channels entries are
  /// used.
  std::array<std::uint8_t, max_channels> mapping{};
  /// @brief Speaker position of each channel; the first channels entries are
  /// used.
  std::array<AudioChannelPosition, max_channels> positions{};

  /// @brief Compares every field.
  friend constexpr auto operator==(const AudioFormat&,
                                   const AudioFormat&) noexcept
      -> bool = default;
};

/// @brief One of the picture sizes a stream can switch between, with the codec
/// header that goes with it.
struct VideoProfile {
  /// @brief Picture width, in pixels.
  std::uint32_t width{};
  /// @brief Picture height, in pixels.
  std::uint32_t height{};
  /// @brief The codec's parameter sets for this size, to be given to the
  /// decoder before its frames.
  std::vector<std::byte> header{};

  /// @brief Compares every field.
  friend auto operator==(const VideoProfile&, const VideoProfile&)
      -> bool = default;
};

/// @brief The picture the console or service actually granted, which may differ
/// from what was asked for.
struct GrantedPicture {
  /// @brief Video codec of the stream.
  VideoCodec codec{VideoCodec::h264};
  /// @brief Picture width, in pixels.
  std::uint32_t width{};
  /// @brief Picture height, in pixels.
  std::uint32_t height{};
  /// @brief Highest frame rate of the stream, in frames per second.
  std::uint32_t max_fps{};
  /// @brief Video bit rate, in kilobits per second.
  std::uint32_t bitrate_kbps{};

  /// @brief Compares every field.
  friend constexpr auto operator==(const GrantedPicture&,
                                   const GrantedPicture&) noexcept
      -> bool = default;
};

/// @brief What a stream consists of, announced before its first frame.
///
/// @see MediaSink::on_stream_description
struct StreamDescription {
  /// @brief Size in bytes of the raw audio header.
  static constexpr std::size_t audio_header_size = 14;

  /// @brief The audio format at the start of the stream.
  AudioFormat audio{};
  /// @brief The picture sizes the stream may use; a VideoUnit names one by
  /// index.
  std::vector<VideoProfile> profiles{};
  /// @brief The granted picture, when the session was told it.
  std::optional<GrantedPicture> picture{};
  /// @brief The audio header exactly as the stream sent it.
  std::array<std::byte, audio_header_size> audio_header{};

  /// @brief Compares every field.
  friend auto operator==(const StreamDescription&, const StreamDescription&)
      -> bool = default;
};

/// @brief One encoded video frame as received from the stream.
///
/// The frame owns its bytes. tail_padding zero bytes follow the frame data in
/// the same buffer, for decoders that read past the end of their input.
class VideoFrame {
 public:
  /// @brief Number of zero bytes kept after the frame data.
  static constexpr std::size_t tail_padding = 64;

  /// @brief Everything known about a frame when it is built.
  struct Facts {
    /// @brief The encoded frame.
    std::vector<std::byte> bytes{};
    /// @brief The stream's number for the frame; it wraps at 65536.
    std::uint16_t frame_index{};
    /// @brief The stream's codec identifier for the frame; 0 means H.264.
    std::uint8_t codec{};
    /// @brief Index into StreamDescription::profiles of the picture size
    /// the frame uses.
    std::uint8_t adaptive_stream_index{};
    /// @brief Number of frames lost between the previously delivered frame
    /// and this one.
    std::int32_t frames_lost{};
    /// @brief True when the stream pointed the frame at an older reference
    /// picture because its own was lost.
    bool recovered{};
    /// @brief True when the bytes are the codec header of a picture size
    /// rather than a picture.
    bool profile_header{};
    /// @brief The stream's dynamic range flag for the frame.
    bool dynamic_range_flag{};
    /// @brief The stream's flag asking that the frame be decoded but not
    /// shown.
    bool suppress_presentation{};
  };

  /// @brief Builds a frame and appends the tail padding to its bytes.
  ///
  /// @param facts The frame's bytes and attributes; the bytes are moved in.
  explicit VideoFrame(Facts facts)
      : facts_(std::move(facts)), size_(facts_.bytes.size()) {
    facts_.bytes.resize(size_ + tail_padding);
  }

  /// @brief The encoded frame, without the tail padding.
  ///
  /// @return The encoded frame, without the tail padding.
  auto bytes() const noexcept -> std::span<const std::byte> {
    return {facts_.bytes.data(), size_};
  }
  /// @brief The encoded frame followed by tail_padding zero bytes.
  ///
  /// @return The encoded frame followed by tail_padding zero bytes.
  auto padded_bytes() const noexcept -> std::span<const std::byte> {
    return facts_.bytes;
  }
  /// @brief Size of the encoded frame in bytes, without the tail padding.
  ///
  /// @return Size of the encoded frame in bytes, without the tail padding.
  auto size() const noexcept -> std::size_t { return size_; }

  /// @brief The stream's number for the frame; it wraps at 65536.
  ///
  /// @return The stream's number for the frame; it wraps at 65536.
  auto frame_index() const noexcept -> std::uint16_t {
    return facts_.frame_index;
  }
  /// @brief The stream's codec identifier for the frame; 0 means H.264.
  ///
  /// @return The stream's codec identifier for the frame; 0 means H.264.
  auto codec() const noexcept -> std::uint8_t { return facts_.codec; }
  /// @brief Index into StreamDescription::profiles of the picture size the
  /// frame uses.
  ///
  /// @return Index into StreamDescription::profiles of the picture size the
  ///     frame uses.
  auto adaptive_stream_index() const noexcept -> std::uint8_t {
    return facts_.adaptive_stream_index;
  }
  /// @brief Number of frames lost between the previously delivered frame and
  /// this one.
  ///
  /// @return Number of frames lost between the previously delivered frame and
  ///     this one.
  auto frames_lost() const noexcept -> std::int32_t {
    return facts_.frames_lost;
  }
  /// @brief Reports whether the stream had to repair the frame before
  /// delivering it.
  ///
  /// @return `true` when the stream had to repair the frame before delivering
  ///     it.
  auto recovered() const noexcept -> bool { return facts_.recovered; }
  /// @brief True when the bytes are the codec header of a picture size rather
  /// than a picture.
  ///
  /// @return `true` when the bytes are the codec header of a picture size
  ///     rather than a picture.
  auto profile_header() const noexcept -> bool { return facts_.profile_header; }
  /// @brief The stream's dynamic range flag for the frame.
  ///
  /// @return The stream's dynamic range flag for the frame.
  auto dynamic_range_flag() const noexcept -> bool {
    return facts_.dynamic_range_flag;
  }
  /// @brief The stream's flag asking that the frame be decoded but not shown.
  ///
  /// @return The stream's flag asking that the frame be decoded but not
  ///     shown.
  auto suppress_presentation() const noexcept -> bool {
    return facts_.suppress_presentation;
  }

 private:
  Facts facts_;
  std::size_t size_;
};

/// @brief A video frame ready for the decoder, together with the picture size
/// it belongs to.
///
/// Passed to MediaSink::on_video. The unit and everything it refers to are
/// valid only during that call; copy what is needed later.
class VideoUnit {
 public:
  /// @brief The encoded frame.
  ///
  /// @return The encoded frame.
  auto frame() const noexcept -> const VideoFrame& { return frame_; }
  /// @brief Index into StreamDescription::profiles of the frame's picture
  /// size.
  ///
  /// @return Index into StreamDescription::profiles of the frame's picture
  ///     size.
  auto profile_index() const noexcept -> std::size_t { return profile_index_; }
  /// @brief Reports whether this frame uses a different picture size from the
  /// last frame the sink took, or is the first frame.
  ///
  /// When true the decoder must be given profile_header() before the frame.
  ///
  /// @return `true` when this frame uses a different picture size from the
  ///     last frame the sink took, or is the first frame.
  auto profile_changed() const noexcept -> bool { return profile_changed_; }
  /// @brief The codec header for the frame's picture size.
  ///
  /// @return The header when profile_changed() is true, otherwise an empty
  ///     span.
  auto profile_header() const noexcept -> std::span<const std::byte> {
    return profile_changed_ ? header_ : std::span<const std::byte>{};
  }
  /// @brief Reports whether the frame is HEVC rather than H.264.
  ///
  /// @return `true` when the frame is HEVC rather than H.264.
  auto hevc() const noexcept -> bool { return hevc_; }
  /// @brief Picture width of the frame's profile, in pixels.
  ///
  /// @return Picture width of the frame's profile, in pixels.
  auto width() const noexcept -> std::uint32_t { return width_; }
  /// @brief Picture height of the frame's profile, in pixels.
  ///
  /// @return Picture height of the frame's profile, in pixels.
  auto height() const noexcept -> std::uint32_t { return height_; }

 private:
  friend class VideoIntake;

  VideoUnit(VideoFrame frame, std::size_t profile_index, bool profile_changed,
            std::span<const std::byte> header, bool hevc, std::uint32_t width,
            std::uint32_t height) noexcept
      : frame_(std::move(frame)),
        profile_index_(profile_index),
        profile_changed_(profile_changed),
        header_(header),
        hevc_(hevc),
        width_(width),
        height_(height) {}

  VideoFrame frame_;
  std::size_t profile_index_;
  bool profile_changed_;
  std::span<const std::byte> header_;
  bool hevc_;
  std::uint32_t width_;
  std::uint32_t height_;
};

/// @brief What became of a video frame given to MediaSink::on_video.
enum class VideoVerdict : std::uint8_t {
  /// @brief The frame was handed to the decoder.
  accepted,
  /// @brief The decoder rejected the frame; the session reports the loss to
  /// the console and may ask for a keyframe.
  recover,
  /// @brief The frame was deliberately skipped, for example because decoder
  /// buffers were full. It is counted as lost and reported to the console, and
  /// a keyframe is asked for, since the frames after it may refer to it.
  dropped,
  /// @brief The decoder could not be set up for the frame's format; the
  /// session ends with a media fault.
  format_failed,
  /// @brief The frame could not be submitted to the decoder; the session ends
  /// with a media fault.
  submit_failed,
  /// @brief The frame was decoded and shown within the call; pending loss is
  /// cleared as by Session::video_presented.
  presented,
};

/// @brief The sink's answer to one video frame.
struct VideoResult {
  /// @brief What became of the frame.
  VideoVerdict verdict{VideoVerdict::accepted};
  /// @brief The decoder's own error code for a failure; logged and kept in
  /// the media fault.
  int detail{};
};

/// @brief What became of an audio unit given to MediaSink::on_audio.
enum class AudioVerdict : std::uint8_t {
  /// @brief The unit was taken.
  accepted,
  /// @brief Audio output failed; the failure is counted in
  /// Statistics::audio_failures.
  failed,
};

/// @brief The sink's answer to one audio unit.
struct AudioResult {
  /// @brief What became of the unit.
  AudioVerdict verdict{AudioVerdict::accepted};
  /// @brief The frontend's own error code for a failure; logged by the
  /// session.
  int detail{};
};

/// @brief One piece of audio from the stream: a packet of samples, or notice
/// that the format changed.
struct AudioUnit {
  /// @brief What the unit carries.
  enum class Kind : std::uint8_t {
    /// @brief One encoded audio packet in bytes.
    samples,
    /// @brief Announces that the stream's audio format changed; format
    /// holds the new one and the decoder must be rebuilt for it.
    format,
  };

  /// @brief What the unit carries.
  Kind kind{Kind::samples};
  /// @brief The packet: Opus, or little-endian 16-bit PCM when raw_pcm is
  /// true.
  std::vector<std::byte> bytes{};
  /// @brief The audio format in force for this unit.
  AudioFormat format{};
  /// @brief True when bytes hold PCM that needs no decoding.
  bool raw_pcm{};
  /// @brief True for the party voice chat a PS4 host or PS Now sends beside
  /// the game audio; it has its own format and decoder, and is mixed into
  /// the game audio rather than replacing it.
  bool voice{};
  /// @brief The seat whose controller speaker a mono 48 kHz Opus packet is
  /// for, sent by PS5 cloud beside the game audio; it never belongs in the
  /// game audio, and an empty packet marks a lost frame. Nothing for other
  /// audio.
  std::optional<std::uint8_t> controller_speaker{};
};

/// @brief Video loss the frontend has not yet accounted for.
///
/// @see Session::video_presented
struct PresentationLoss {
  /// @brief Frames lost since the previous report.
  std::int32_t frames_lost{};
  /// @brief True when a frame since the previous report was pointed at an
  /// older reference picture because its own was lost.
  bool recovered{};
};

/// @brief Receives a session's video, audio and controller feedback.
///
/// The frontend implements this interface and passes it to Play when starting a
/// session, or to ForwardedStream::open when watching a shared one. The object
/// must stay alive until that Session or ForwardedStream has been destroyed.
///
/// A Session makes every call on its own thread. A ForwardedStream makes them
/// on the thread that calls ForwardedStream::receive or
/// ForwardedStream::service. Calls should return quickly; decoding and output
/// belong on the frontend's own threads. Arguments are valid only during the
/// call.
///
/// @see SessionEvents
/// @see AudioDecoder
/// @see FeedbackInbox
class MediaSink {
 public:
  /// @brief Destroys the sink.
  virtual ~MediaSink() = default;
  /// @brief Called when the stream has been negotiated, before its first
  /// frame, and again if it is negotiated anew.
  ///
  /// @param description The stream's audio format and picture sizes.
  virtual void on_stream_description(const StreamDescription& description) = 0;
  /// @brief Called for each video frame, in stream order.
  ///
  /// Must never block or sleep, waiting neither for the decoder nor for the
  /// screen. When the decoder cannot take the frame now, return
  /// VideoVerdict::dropped at once; the session reports the loss and asks for
  /// a keyframe.
  ///
  /// @param unit The frame and the picture size it belongs to; valid only
  ///     during the call. A unit whose frame has VideoFrame::profile_header
  ///     set carries a codec header rather than a picture.
  /// @return What the sink did with the frame. VideoVerdict::format_failed
  ///     and VideoVerdict::submit_failed end the session.
  /// @see Session::video_rejected
  /// @see Session::video_presented
  virtual auto on_video(const VideoUnit& unit) -> VideoResult = 0;
  /// @brief Called when the stream could not rebuild a video frame, so the
  /// frames that follow may not decode cleanly until the next keyframe.
  virtual void on_video_gap() = 0;
  /// @brief Called for each audio packet and each audio format change.
  ///
  /// @param unit The packet or format notice; valid only during the call.
  /// @return Whether the sink took the unit.
  virtual auto on_audio(const AudioUnit& unit) -> AudioResult = 0;
  /// @brief Called when the console sends something for a controller: rumble,
  /// haptics, trigger effects, light and player indicators.
  ///
  /// @param feedback What was sent and for which seat. A HapticAudio fact
  ///     refers to bytes that are valid only during the call.
  /// @see FeedbackInbox::accept
  virtual void on_feedback(const controllers::ControllerFeedback& feedback) = 0;
  /// @brief Names the frontend's video decoder for diagnostics.
  ///
  /// The name is read once when a Session is created and prefixes the stage
  /// names of media faults, as in "<name> submission".
  ///
  /// @return The decoder's name; default_decoder_name unless overridden.
  virtual auto decoder_name() const -> std::string_view {
    return default_decoder_name;
  }

  /// @brief The decoder name used when a sink does not override decoder_name.
  static constexpr std::string_view default_decoder_name = "Video decoder";
};

}  // namespace libchiaking::media
