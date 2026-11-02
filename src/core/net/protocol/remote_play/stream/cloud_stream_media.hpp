/// @file
/// @brief Declares CloudStreamMediaAssembly and its parts, which turn received
/// Takion AV units into video frames, audio frames and haptics, for consoles
/// and for PS cloud streams alike, and count what was lost for the congestion
/// reports.
///
/// Consoles send audio as packets of several units: source units carry the
/// newest frames and the rest repeat earlier frames (temporal redundancy). PS
/// cloud may instead send each frame's units with Cauchy parity, on several
/// audio lanes (AV tags 0, 1 and 6 to 9). Audio leaves through a jitter
/// buffer that puts frames in order and marks gaps with empty frames.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "src/core/net/protocol/remote_play/stream/cauchy_fec8.hpp"
#include "src/core/net/protocol/remote_play/stream/limits.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/net/protocol/remote_play/stream/video_assembler.hpp"
#include "src/core/net/protocol/takion/client_types.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/utils/numeric/sequence_number.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace libchiaking::net::protocol::remote_play {

/// @brief A 16-bit frame index compared with wraparound.
using FrameIndex = numeric::SequenceNumber<std::uint16_t>;

/// @brief The Takion audio codec byte of Opus.
inline constexpr std::uint8_t audio_codec_opus = 5;
/// @brief The source count bits of a unit word when Takion encodes unit
/// counts only.
inline constexpr unsigned audio_fec_count_only_source_mask = 0x1fffU;
/// @brief A 4-bit field of a console audio unit word.
inline constexpr unsigned audio_fec_nibble_mask = 0xfU;
/// @brief Where the redundant unit count sits in a console audio unit word
/// (bits 4 to 7; source count is bits 0 to 3).
inline constexpr unsigned audio_fec_count_shift = 4U;
/// @brief Where the unit size sits in a console audio unit word (bits 8 and
/// up).
inline constexpr unsigned audio_unit_size_shift = 8U;
/// @brief The audio mode of temporal redundancy, which uses console units
/// even on PS cloud.
inline constexpr std::uint8_t audio_mode_temporal_redundancy = 2;
/// @brief The size of the header before each audio frame in
/// console-by-mode generations, in bytes.
inline constexpr std::size_t audio_unit_header_size = 2;
/// @brief Frame indexes above this end the start-up period in which
/// redundant units that would precede frame 0 are skipped.
inline constexpr std::uint16_t audio_startup_frame_limit = 0x8000;
/// @brief The most audio frames the jitter buffer holds.
inline constexpr std::size_t audio_jitter_buffer_size = 64;
/// @brief How many frames the jitter buffer collects before it releases the
/// first.
inline constexpr std::size_t audio_jitter_prefill = 3;

/// @brief Why a console audio packet was refused.
enum class ConsoleAudioError : std::uint8_t { invalid_packet, invalid_units };
/// @var ConsoleAudioError::invalid_packet
/// @brief Not Opus, no units, or data not divisible into units.
/// @var ConsoleAudioError::invalid_units
/// @brief The unit counts and size do not add up to the data.

/// @brief One audio (or haptics) frame leaving an assembler.
struct ConsoleAudioFrame {
  /// @brief The frame index.
  std::uint16_t index;
  /// @brief The frame; empty for a lost frame to conceal.
  std::vector<std::byte> bytes;
  /// @brief The AV lane it came on.
  std::optional<std::uint8_t> av_tag{};
  /// @brief The audio profile it was encoded with.
  std::uint8_t audio_profile{};
};

/// @brief Splits console audio packets into frames and orders them.
///
/// Haptics frames skip the jitter buffer and are passed on when newer than
/// the last. Not thread-safe.
class ConsoleAudioAssembler {
 public:
  /// @brief Splits a packet into frames and releases what is now in order.
  ///
  /// @param packet The packet.
  /// @param count_only Whether Takion encodes unit counts only.
  /// @return The frames released, in order, with empty frames for gaps.
  /// @retval ConsoleAudioError Why the packet was refused.
  [[nodiscard]] auto consume(const takion::AvPacketValues& packet,
                             bool count_only)
      -> Expected<std::vector<ConsoleAudioFrame>, ConsoleAudioError>;

  /// @brief Adds one frame and releases what is now in order.
  ///
  /// Late and repeated frames are dropped; a full buffer drops its newest
  /// frame for an older one.
  ///
  /// @param frame The frame index.
  /// @param bytes The frame.
  /// @param packet The packet it came in, for its lane, profile and kind.
  /// @param result Where released frames are appended.
  void push_frame(std::uint16_t frame, std::span<const std::byte> bytes,
                  const takion::AvPacketValues& packet,
                  std::vector<ConsoleAudioFrame>& result);

  /// @brief Forgets everything.
  void reset() noexcept;

 private:
  /// @brief Whether one index is newer than another, with wraparound.
  ///
  /// @param a One index.
  /// @param b The other.
  /// @return True when a is newer.
  static auto newer(std::uint16_t a, std::uint16_t b) noexcept -> bool;

  /// @brief The buffered frame with the oldest or newest index.
  ///
  /// @param oldest True for the oldest.
  /// @return Its position in the buffer.
  auto extreme(bool oldest) const noexcept -> std::size_t;

  /// @brief Releases frames in order from the next expected, filling a gap
  /// with an empty frame once newer frames have piled up behind it.
  ///
  /// @param result Where released frames are appended.
  void drain(std::vector<ConsoleAudioFrame>& result);

  /// @brief Frames waiting for their turn.
  std::vector<ConsoleAudioFrame> buffer_;
  /// @brief The next frame to release, once primed.
  std::optional<std::uint16_t> next_;
  /// @brief The newest haptics frame passed on.
  std::uint16_t last_haptic_{};
  /// @brief Whether still in the start-up period.
  bool startup_{true};
};

/// @brief Why a PS cloud audio unit was refused.
enum class CloudStreamAudioUnitError : std::uint8_t {
  /// @brief Not audio or haptics.
  invalid_family,
  /// @brief Not Opus.
  invalid_codec,
  /// @brief The unit counts are impossible.
  invalid_count,
  /// @brief The unit index is out of range.
  invalid_unit_index,
  /// @brief The unit is empty.
  empty_data,
  /// @brief The unit exceeds max_unit_bytes.
  data_too_large,
};

/// @brief One checked PS cloud audio unit.
struct CloudStreamAudioUnit {
  /// @brief The frame index the packet states.
  std::uint16_t frame_index{};
  /// @brief The unit's index in its generation.
  std::uint16_t unit_index{};
  /// @brief Source units in the generation.
  std::uint16_t source_count{};
  /// @brief Parity units in the generation.
  std::uint16_t fec_count{};
  /// @brief The unit.
  std::vector<std::byte> data{};
};

/// @brief Why a PS cloud audio unit could not be assembled.
enum class CloudStreamAudioAssemblerError : std::uint8_t {
  /// @brief The unit's counts, index or size are out of range.
  invalid_unit,
  /// @brief The unit arrived twice.
  duplicate_unit,
  /// @brief Reserved; not produced.
  count_mismatch,
  /// @brief Reserved; not produced.
  base_mismatch,
  /// @brief The unit belongs to a generation already passed or emitted.
  stale_generation,
  /// @brief Holding the unit would exceed max_held_bytes.
  hold_limit_exceeded,
  /// @brief Reserved; not produced.
  fec_rejected,
};

/// @brief One audio frame out of a PS cloud generation.
struct CloudStreamAudioFrame {
  /// @brief The frame index.
  std::uint16_t frame_index{};
  /// @brief The frame.
  std::vector<std::byte> bytes;
};

/// @brief How a generation's first frame index is found.
enum class AudioGenerationNumbering : std::uint8_t { per_unit, per_frame };
/// @var AudioGenerationNumbering::per_unit
/// @brief Each unit's frame index counts up with its unit index (first frame
/// = frame index less unit index).
/// @var AudioGenerationNumbering::per_frame
/// @brief Every unit states the generation's first frame index.

/// @brief Joins PS cloud audio generations: source units plus Cauchy parity
/// units.
///
/// A generation is emitted once every source unit is there or enough units
/// arrived to recover them; a newer generation, a restart at unit 0 or a
/// generation older than 150 ms flushes the current one with what it has.
/// Not thread-safe.
class CloudStreamAudioAssembler {
 public:
  /// @brief Makes an empty assembler.
  ///
  /// @param numbering How first frame indexes are found.
  explicit CloudStreamAudioAssembler(
      AudioGenerationNumbering numbering =
          AudioGenerationNumbering::per_unit) noexcept;

  /// @brief The most units in a generation.
  static constexpr std::size_t max_total_units = 256;
  /// @brief The largest unit, in bytes.
  static constexpr std::size_t max_unit_bytes = 4096;

  /// @brief Checks an AV packet as a PS cloud audio unit.
  ///
  /// @param packet The packet.
  /// @return The unit.
  /// @retval CloudStreamAudioUnitError Why it was refused.
  [[nodiscard]] static auto validate_unit(const takion::AvPacketValues& packet)
      -> Expected<CloudStreamAudioUnit, CloudStreamAudioUnitError>;

  /// @brief The most bytes held for one generation.
  static constexpr std::size_t max_held_bytes = 256 * 1024;
  /// @brief How long a generation may wait for its units, in microseconds.
  static constexpr std::uint64_t generation_timeout_us = 150'000;

  /// @brief Adds a unit.
  ///
  /// @param unit The unit.
  /// @param now_us Monotonic microseconds now.
  /// @return The frames emitted, in order; often none.
  /// @retval CloudStreamAudioAssemblerError Why the unit was refused.
  [[nodiscard]] auto accept(const CloudStreamAudioUnit& unit,
                            std::uint64_t now_us = 0)
      -> Expected<std::vector<CloudStreamAudioFrame>,
                  CloudStreamAudioAssemblerError>;

  /// @brief Forgets everything.
  void reset() noexcept;

 private:
  /// @brief How first frame indexes are found.
  AudioGenerationNumbering numbering_;
  /// @brief Recovers lost units from parity units.
  CauchyFec8Decoder decoder_;
  /// @brief The generation being assembled.
  struct Generation {
    /// @brief Starts a generation.
    ///
    /// @param source Source units.
    /// @param fec Parity units.
    /// @param frame The first frame index.
    /// @param total All units.
    /// @param first_unit_size The size of the first unit, taken as the unit
    ///     size for recovery.
    /// @param now_us Monotonic microseconds now.
    Generation(std::uint16_t source, std::uint16_t fec, std::uint16_t frame,
               std::size_t total, std::size_t first_unit_size,
               std::uint64_t now_us);
    /// @brief Source units.
    std::uint16_t source_count{};
    /// @brief Parity units.
    std::uint16_t fec_count{};
    /// @brief The first frame index.
    std::uint16_t base{};
    /// @brief The unit size for recovery, in bytes.
    std::size_t unit_size{};
    /// @brief When it started, in monotonic microseconds.
    std::uint64_t started_us{};
    /// @brief Whether its frames were emitted.
    bool emitted{};
    /// @brief Its units by index.
    std::vector<std::optional<std::vector<std::byte>>> slots;
  };

  /// @brief Whether one index is newer than another, with wraparound.
  ///
  /// @param candidate One index.
  /// @param current The other.
  /// @return True when candidate is newer.
  static auto newer(std::uint16_t candidate, std::uint16_t current) noexcept
      -> bool;

  /// @brief Marks the generation emitted and frees its units.
  void finish() noexcept;

  /// @brief Recovers the generation's sources from its parity units.
  ///
  /// @return The sources.
  /// @retval CauchyFec8Error Why recovery failed.
  [[nodiscard]] auto recover()
      -> Expected<std::vector<std::vector<std::byte>>, CauchyFec8Error>;

  /// @brief Emits what an unfinished generation has: recovered sources, or the
  /// sources that arrived.
  ///
  /// @param frames Where frames are appended.
  void flush(std::vector<CloudStreamAudioFrame>& frames);

  /// @brief The generation being assembled.
  std::optional<Generation> active_;
  /// @brief Bytes held for it.
  std::size_t held_bytes_{};
};

/// @brief Counts received and lost media units for the congestion reports.
///
/// Video is counted by its packet index: every arrival is received, and
/// indexes skipped since the last report are lost, however its frames were
/// assembled. Audio is counted by the span of frame indexes seen. Not
/// thread-safe.
class MediaPacketStats {
 public:
  /// @brief Counts a video packet, including late and repeated ones.
  ///
  /// @param packet The packet.
  void video(const takion::AvPacketValues& packet) noexcept;

  /// @brief Counts an audio frame.
  ///
  /// @param frame The frame index.
  /// @param units Units per frame, at least 1.
  void audio(std::uint16_t frame, std::uint16_t units = 1) noexcept;

  /// @brief The default ceiling on the reported loss share.
  static constexpr double default_packet_loss_max =
      remote_play::default_packet_loss_max;

  /// @brief Takes the counts since the last call as a congestion report.
  ///
  /// @param packet_loss_max The largest loss share to report; more is capped.
  /// @return Packets the server sent and packets lost (16-bit each).
  auto take(double packet_loss_max = default_packet_loss_max) noexcept
      -> takion::CongestionValues;

  /// @brief Forgets everything.
  void reset() noexcept;

 private:
  /// @brief Whether one index is newer than another, with wraparound.
  ///
  /// @param a One index.
  /// @param b The other.
  /// @return True when a is newer.
  static auto newer(std::uint16_t a, std::uint16_t b) noexcept -> bool;

  /// @brief The newest video packet index seen.
  std::optional<std::uint16_t> video_newest_;
  /// @brief Video packet indexes passed since the last report.
  std::uint64_t video_span_{};
  /// @brief The oldest audio frame index since the last report.
  std::uint16_t audio_min_{};
  /// @brief The newest audio frame index seen.
  std::uint16_t audio_max_{};
  /// @brief Video packets received since the last report.
  std::uint64_t received_{};
  /// @brief Audio frames received since the last report.
  std::uint64_t audio_received_{};
  /// @brief Audio units per frame.
  std::uint16_t audio_units_{1};
};

/// @brief Why a media unit was refused.
enum class CloudStreamMediaAssemblyError : std::uint8_t {
  /// @brief Neither video nor audio.
  unsupported_family,
  /// @brief The video assembler refused it; see last_video_error().
  video_rejected,
  /// @brief An audio unit was malformed or on an unknown lane.
  audio_unit_rejected,
  /// @brief An audio generation refused it.
  audio_rejected,
};

/// @brief The AV lane a PS4 host sends the party voice chat on, as mono Opus
/// beside the game audio.
inline constexpr std::uint8_t console_voice_lane = 1;

/// @brief The AV lane PS cloud sends the first controller's speaker audio on,
/// as mono Opus; the next controllers follow on the lanes after it.
inline constexpr std::uint8_t controller_speaker_lane_base = 6;

/// @brief The number of PS cloud controller speaker lanes.
inline constexpr std::uint8_t controller_speaker_lanes = 4;

/// @brief How a stream's audio is assembled.
enum class AudioAssemblyPolicy : std::uint8_t {
  /// @brief Console units only (PS4).
  console_units,
  /// @brief Console units, or PS cloud generations numbered per frame when
  /// Takion encodes unit counts only and the audio mode asks for it (PS5).
  console_by_mode,
  /// @brief PS cloud generations per lane, console units for temporal
  /// redundancy.
  cloud_generation
};

/// @brief A frame out of the assembly.
using AssembledMedia = std::variant<takion::client_event::EncodedVideo,
                                    takion::client_event::EncodedAudio,
                                    takion::client_event::Haptics>;

/// @brief Turns a stream's AV units into frames.
///
/// Not thread-safe; the stream's worker feeds it.
class CloudStreamMediaAssembly {
 public:
  /// @brief Makes an assembly.
  ///
  /// @param policy How audio is assembled.
  /// @param count_only Whether Takion encodes unit counts only.
  explicit CloudStreamMediaAssembly(
      AudioAssemblyPolicy policy = AudioAssemblyPolicy::cloud_generation,
      bool count_only = false) noexcept;
  /// @brief Takes one AV unit.
  ///
  /// Haptics on AV tag 2 and up belong to pad tag - 2.
  ///
  /// @param packet The unit; a video unit's data moves into the assembler.
  /// @param now_us Monotonic microseconds now.
  /// @param expected_video_blocks The picture's block count, or 0.
  /// @return The frames completed, often none.
  /// @retval CloudStreamMediaAssemblyError Why the unit was refused.
  [[nodiscard]] auto consume(takion::AvPacketValues packet,
                             std::uint64_t now_us = 0,
                             std::uint32_t expected_video_blocks = 0)
      -> Expected<std::vector<AssembledMedia>, CloudStreamMediaAssemblyError>;

  /// @brief Why the last video unit was refused.
  ///
  /// @return The error, or nothing.
  [[nodiscard]] auto last_video_error() const noexcept
      -> std::optional<VideoAssemblerError> {
    return last_video_error_;
  }

  /// @brief Takes the oldest video loss.
  ///
  /// @return The loss, or nothing.
  [[nodiscard]] auto take_video_loss() noexcept
      -> std::optional<VideoFrameLoss>;

  /// @brief Records a delivered video frame.
  ///
  /// @param frame_index The frame.
  void mark_video_complete(std::uint16_t frame_index) noexcept;

  /// @brief Moves the last delivered video frame back before a loss.
  ///
  /// @param first_lost The first lost frame.
  void rewind_video_complete(std::uint16_t first_lost) noexcept;

  /// @brief The last delivered video frame.
  ///
  /// @return Its index.
  auto last_video_complete() const noexcept -> std::uint16_t;

  /// @brief How many video frames needed parity units to be completed.
  ///
  /// @return The count since the last reset.
  auto fec_repaired_frames() const noexcept -> std::uint64_t {
    return fec_repaired_frames_;
  }

  /// @brief The last video frame seen.
  ///
  /// @return Its index, or nothing.
  auto last_video_frame() const noexcept -> std::optional<std::uint16_t>;

  /// @brief Tells whether consume() refuses units of a video frame as stale.
  ///
  /// @param frame_index The frame.
  /// @return As VideoAssembler::stale().
  auto video_frame_stale(std::uint16_t frame_index) const noexcept -> bool;

  /// @brief Takes the counts as a congestion report.
  ///
  /// @param packet_loss_max The largest loss share to report.
  /// @return The report values.
  auto take_congestion(double packet_loss_max =
                           MediaPacketStats::default_packet_loss_max) noexcept
      -> takion::CongestionValues;

  /// @brief Forgets audio state when a new stream info arrives.
  void on_stream_info() noexcept;

  /// @brief Changes whether Takion encodes unit counts only.
  ///
  /// @param count_only The new setting.
  void set_count_only(bool count_only) noexcept { count_only_ = count_only; }

  /// @brief Forgets everything.
  void reset() noexcept;

 private:
  /// @brief Whether an audio unit uses the console unit layout under this
  /// policy.
  ///
  /// @param packet The unit.
  /// @return True for console units.
  [[nodiscard]] auto uses_console_units(
      const takion::AvPacketValues& packet) const noexcept -> bool;
  /// @brief Drops the per-frame header of a console-by-mode audio frame.
  ///
  /// @param unit The frame.
  /// @return The frame without its 2-byte header; empty when shorter.
  static auto without_unit_header(std::span<const std::byte> unit)
      -> std::vector<std::byte>;

  /// @brief The first AV tag of haptics; tag 2 is pad 0.
  static constexpr std::uint8_t haptics_tag_base = 2;
  /// @brief The audio mode of PS cloud frames sent whole.
  static constexpr std::uint8_t complete_frame_mode = 3;
  /// @brief The number of PS cloud audio lanes tracked.
  static constexpr std::size_t cloud_audio_lane_count = 10;

  /// @brief The lane of a PS cloud AV tag.
  ///
  /// @param tag The tag; none is lane 0.
  /// @return 0, 1 or 6 to 9; nothing for other tags.
  static auto cloud_lane(std::optional<std::uint8_t> tag) noexcept
      -> std::optional<std::size_t>;
  /// @brief Turns released audio frames into events, counting lane 0's.
  ///
  /// @param frames The frames.
  /// @param lane Their lane.
  /// @param count_frames Whether to count them for congestion.
  /// @return The events.
  auto cloud_audio_events(std::vector<ConsoleAudioFrame> frames,
                          std::size_t lane, bool count_frames)
      -> std::vector<AssembledMedia>;

  /// @brief Turns an audio frame into an event.
  ///
  /// @param frame The frame.
  /// @return The event.
  static auto encoded_audio(ConsoleAudioFrame frame)
      -> takion::client_event::EncodedAudio;

  /// @brief Why the last video unit was refused.
  std::optional<VideoAssemblerError> last_video_error_;
  /// @brief The congestion counts.
  MediaPacketStats stats_;
  /// @brief How audio is assembled.
  AudioAssemblyPolicy policy_;
  /// @brief Whether Takion encodes unit counts only.
  bool count_only_;
  /// @brief Video frames completed from parity units.
  std::uint64_t fec_repaired_frames_{};
  /// @brief Console audio.
  ConsoleAudioAssembler console_audio_;
  /// @brief Console party voice chat, on console_voice_lane.
  ConsoleAudioAssembler console_voice_;
  /// @brief Console haptics.
  ConsoleAudioAssembler console_haptics_;
  /// @brief PS cloud haptics.
  CloudStreamAudioAssembler cloud_haptics_;
  /// @brief Video.
  VideoAssembler video_;
  /// @brief Console-by-mode audio generations.
  CloudStreamAudioAssembler audio_;
  /// @brief PS cloud audio generations per lane.
  std::array<CloudStreamAudioAssembler, cloud_audio_lane_count>
      cloud_audio_units_;
  /// @brief PS cloud audio jitter buffers per lane.
  std::array<ConsoleAudioAssembler, cloud_audio_lane_count> cloud_audio_jitter_;
};

}  // namespace libchiaking::net::protocol::remote_play
