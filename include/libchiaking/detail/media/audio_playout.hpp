/// @file
/// @brief Declares AudioPlayout, which holds decoded PCM between the thread
/// that decodes it and the real-time thread that plays it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace libchiaking::media {

/// @brief A playout buffer for interleaved 16-bit PCM, filled by one thread
/// and drained by one real-time thread.
///
/// A frontend creates one for each format, pushes every decoded packet from
/// the thread that decodes audio, and pulls what the audio device asks for
/// from its render thread. The two sides share a single-producer
/// single-consumer queue of fixed-size blocks, so pull() never waits, locks
/// or allocates. push() fills a block before handing it over, so packets of
/// any size use the whole buffer, and up to block_frames - 1 frames wait in
/// push() until the next packet completes the block, so the last few
/// milliseconds before the decoder pauses wait until it resumes. The buffer
/// holds up to three times the target delay, and always at least two blocks
/// beyond the target so a partly pulled block never shuts the next one out.
///
/// When the buffer is full, push() drops the newest audio that does not fit
/// and counts it as an overflow. When the audio left after each pull stays at
/// twice the target delay or more for half a second, pull() skips ahead to
/// the target once and counts a trim, so a burst does not leave the delay
/// high for the rest of the stream. Measuring after the pull keeps a large
/// render quantum from looking like a full buffer.
///
/// push() is called from one thread at a time and pull() from one thread at a
/// time; statistics() may be called from any thread.
class AudioPlayout {
 public:
  /// @brief Largest number of interleaved channels accepted.
  static constexpr std::uint32_t max_channels = 12;
  /// @brief Frames per queued block.
  static constexpr std::uint32_t block_frames = 240;
  /// @brief Shortest target delay accepted, in milliseconds.
  static constexpr std::uint32_t min_target_ms = 5;
  /// @brief Longest target delay accepted, in milliseconds.
  static constexpr std::uint32_t max_target_ms = 1000;
  /// @brief How many target delays the buffer holds.
  static constexpr std::uint32_t capacity_targets = 3;
  /// @brief How many target delays of audio must stay after every pull of a
  /// window before pull() trims.
  static constexpr std::uint32_t trim_above_targets = 2;
  /// @brief How many trim windows fit in one second of pulled audio.
  static constexpr std::uint32_t trim_windows_per_second = 2;

  /// @brief Running totals since the buffer was created.
  struct Statistics {
    /// @brief Frames handed to push(), kept or not; one frame holds one
    /// sample per channel.
    std::uint64_t pushed_frames{};
    /// @brief Calls to push() that dropped audio because the buffer was full.
    std::uint64_t overflows{};
    /// @brief Frames push() dropped because the buffer was full.
    std::uint64_t dropped_frames{};
    /// @brief Calls to pull() that supplied less than asked.
    std::uint64_t underruns{};
    /// @brief Frames pull() filled with silence.
    std::uint64_t silent_frames{};
    /// @brief Times pull() skipped ahead to the target delay.
    std::uint64_t trims{};
    /// @brief Frames pull() skipped to reach the target delay.
    std::uint64_t trimmed_frames{};
    /// @brief Frames handed over to pull() and not yet pulled; frames still
    /// filling a block in push() are not counted.
    std::uint64_t buffered_frames{};
    /// @brief The most frames ever waiting at once.
    std::uint64_t maximum_buffered_frames{};

    /// @brief Compares every field.
    friend constexpr auto operator==(const Statistics&,
                                     const Statistics&) noexcept
        -> bool = default;
  };

  /// @brief Creates a buffer for one format.
  ///
  /// @param channels Interleaved channels, 1 to max_channels.
  /// @param sample_rate Frames per second; must not be zero.
  /// @param target_ms The delay to keep, clamped to min_target_ms and
  ///     max_target_ms.
  /// @return The buffer, or nothing when the channels or rate are not usable
  ///     or the queue cannot be allocated.
  [[nodiscard]] static auto create(std::uint32_t channels,
                                   std::uint32_t sample_rate,
                                   std::uint32_t target_ms)
      -> std::optional<AudioPlayout>;

  /// @brief Releases the buffer.
  ~AudioPlayout();
  /// @brief Moves the buffer; the source is left empty and must not be used
  ///     except to be destroyed or assigned.
  AudioPlayout(AudioPlayout&&) noexcept;
  /// @brief Moves a buffer in, releasing this one.
  ///
  /// @return This buffer.
  auto operator=(AudioPlayout&&) noexcept -> AudioPlayout&;
  /// @brief Not copyable.
  AudioPlayout(const AudioPlayout&) = delete;
  /// @brief Not copyable.
  auto operator=(const AudioPlayout&) -> AudioPlayout& = delete;

  /// @brief Queues decoded audio, dropping what does not fit.
  ///
  /// @param samples Interleaved samples; a trailing partial frame is ignored.
  void push(std::span<const std::int16_t> samples) noexcept;

  /// @brief Takes audio for playback, filling what is missing with silence.
  ///
  /// Never waits, locks or allocates.
  ///
  /// @param samples Where the interleaved samples go; a trailing partial
  ///     frame is filled with silence.
  /// @return The frames of audio supplied before the silence.
  auto pull(std::span<std::int16_t> samples) noexcept -> std::size_t;

  /// @brief Reads the running totals.
  ///
  /// Each counter is read on its own, so a snapshot taken while audio moves
  /// may be a block out of step between fields.
  ///
  /// @return The totals.
  [[nodiscard]] auto statistics() const noexcept -> Statistics;

  /// @brief Frames of audio waiting to be pulled.
  ///
  /// @return The frames, as statistics() would report them.
  [[nodiscard]] auto buffered_frames() const noexcept -> std::uint64_t;

  /// @brief The delay the buffer keeps.
  ///
  /// @return The target, in frames.
  [[nodiscard]] auto target_frames() const noexcept -> std::uint64_t;

 private:
  /// @brief The queue, counters and trim window.
  struct Impl;

  /// @brief Wraps a created buffer.
  ///
  /// @param impl The buffer's state.
  explicit AudioPlayout(std::unique_ptr<Impl> impl) noexcept;

  /// @brief The buffer's state.
  std::unique_ptr<Impl> impl_;
};

}  // namespace libchiaking::media
