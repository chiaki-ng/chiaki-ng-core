/// @file
/// @brief Declares FeedbackInbox, which holds controller feedback between the
/// session's thread and the thread that drives a controller.
#pragma once

#include "libchiaking/detail/controllers/controller_feedback.hpp"
#include "libchiaking/detail/controllers/seat_number.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace libchiaking::platform {
class ClockProvider;
}

namespace libchiaking::platform {
class ThreadingProvider;
}

namespace libchiaking::controllers {

/// @brief A fixed-size block of haptic audio taken from a FeedbackInbox, with
/// the gain to play it at.
///
/// The block owns a copy of its samples: interleaved stereo, signed 16-bit
/// little-endian, at sample_rate.
class HapticBlock {
 public:
  /// @brief Size of a block in bytes.
  static constexpr std::size_t size = 256;
  /// @brief Number of interleaved channels.
  static constexpr std::size_t channels = HapticAudio::channels;
  /// @brief Number of frames in a block.
  static constexpr std::size_t frames = size / HapticAudio::frame_bytes;
  /// @brief Sample rate of the audio, in hertz.
  static constexpr std::uint32_t sample_rate = HapticAudio::sample_rate;

  /// @brief A block's samples as numbers, interleaved.
  using Samples = std::array<std::int16_t, frames * channels>;

  /// @brief Copies one block of samples.
  ///
  /// @param pcm The block's bytes.
  /// @param intensity The console's haptics strength setting when the block
  ///     was taken.
  /// @param gain The factor to play the block at, the console's setting
  ///     already included.
  HapticBlock(std::span<const std::byte, size> pcm, FeedbackIntensity intensity,
              float gain) noexcept;

  /// @brief The block's bytes as received, before any gain.
  ///
  /// @return The block's bytes as received, before any gain.
  auto pcm() const noexcept -> std::span<const std::byte, size> { return pcm_; }
  /// @brief The console's haptics strength setting when the block was taken.
  ///
  /// @return The console's haptics strength setting when the block was taken.
  auto intensity() const noexcept -> FeedbackIntensity { return intensity_; }
  /// @brief The factor to play the block at, the console's setting already
  /// included.
  ///
  /// @return The factor to play the block at, the console's setting already
  ///     included.
  auto gain() const noexcept -> float { return gain_; }
  /// @brief The samples multiplied by gain(), limited to the 16-bit range.
  ///
  /// @return The samples multiplied by gain(), limited to the 16-bit range.
  auto scaled_samples() const noexcept -> Samples;
  /// @brief Reports whether the block is too quiet to be worth playing: every
  /// sample, after gain, is below 256 in magnitude.
  ///
  /// @return `true` when the block is too quiet to be worth playing: every
  ///     sample, after gain, is below 256 in magnitude.
  auto silent() const noexcept -> bool;

 private:
  static auto sample_at(std::span<const std::byte, size> pcm,
                        std::size_t index) noexcept -> std::int16_t;
  static auto saturated(float value) noexcept -> std::int16_t;

  std::array<std::byte, size> pcm_{};
  FeedbackIntensity intensity_;
  float gain_;
};

/// @brief Holds the feedback a console sends for each seat until the frontend's
/// device code takes it.
///
/// MediaSink::on_feedback runs on the session's thread and must return quickly,
/// while a controller is usually driven from another thread at its own pace.
/// The inbox sits between the two: on_feedback calls accept, and the device
/// code calls the take functions. Haptic audio is copied, cut into HapticBlock
/// pieces, and discarded when it has waited too long to be worth playing.
/// Rumble and trigger effects keep only their latest value.
///
/// Facts the inbox does not hold, such as light colours and SeatConfirmed, are
/// left for the frontend to handle directly.
///
/// All members may be called from any thread, with one limit: each seat's
/// haptic blocks are taken by one thread at a time. take_haptic_block never
/// locks or allocates, so it may run on a real-time audio callback; the
/// other members take a short lock that take_haptic_block never waits on.
/// A seat holds up to Configuration::buffered_blocks blocks plus a few
/// already dropped that its taker has not yet passed over; when the taker
/// has fallen that far behind, new audio is dropped and counted as overrun
/// until it next takes a block.
///
/// @see MediaSink::on_feedback
class FeedbackInbox {
 public:
  /// @brief The clock whose time points age haptic audio; the inbox's own
  /// readings come from the platform::ClockProvider it was given.
  using Clock = std::chrono::steady_clock;

  /// @brief What to do when haptic audio arrives and a seat's buffer is full.
  enum class Overrun : std::uint8_t {
    /// @brief Drops everything buffered for the seat and keeps the new
    /// audio.
    discard_buffered,
    /// @brief Drops only the oldest block.
    drop_oldest_block,
  };
  /// @brief What accept did with a piece of feedback.
  enum class Admission : std::uint8_t {
    /// @brief It was stored or applied.
    held,
    /// @brief Haptic audio that is empty, not whole frames, or over
    /// HapticAudio::max_bytes, or a HapticIntensity that is not a
    /// FeedbackIntensity; it was ignored.
    malformed,
    /// @brief A kind of fact the inbox does not keep.
    not_held,
  };

  /// @brief Tunable limits of an inbox.
  struct Configuration {
    /// @brief Upper limit for every gain set on the inbox.
    float gain_ceiling{2.4F};
    /// @brief How long buffered haptic audio may wait before it is
    /// discarded.
    std::chrono::milliseconds stale_after{40};
    /// @brief Haptic audio buffered for each seat, in blocks; at least one.
    std::size_t buffered_blocks{2};
    /// @brief What to do when a seat's buffer is full.
    Overrun overrun{Overrun::discard_buffered};
  };

  /// @brief Trigger effects together with the strength to apply them at.
  struct Triggers {
    /// @brief The effects for both triggers.
    TriggerEffects effects{};
    /// @brief The console's trigger strength setting.
    FeedbackIntensity intensity{FeedbackIntensity::strong};

    /// @brief Compares both parts.
    friend constexpr auto operator==(const Triggers&, const Triggers&) noexcept
        -> bool = default;
  };

  /// @brief What is waiting to be taken for one seat.
  struct Pending {
    /// @brief Number of whole haptic blocks buffered.
    std::size_t haptic_blocks{};
    /// @brief True when take_triggers would return something.
    bool triggers{};
    /// @brief True when take_rumble would return something.
    bool rumble{};

    /// @brief Reports whether anything at all is waiting.
    ///
    /// @return `true` when anything at all is waiting.
    constexpr auto any() const noexcept -> bool {
      return haptic_blocks != 0 || triggers || rumble;
    }

    /// @brief Compares every field.
    friend constexpr auto operator==(const Pending&, const Pending&) noexcept
        -> bool = default;
  };

  /// @brief Running totals for an inbox since it was created.
  struct Statistics {
    /// @brief Haptic audio bursts given to accept, malformed ones included.
    std::uint64_t haptic_packets{};
    /// @brief Bytes of haptic audio given to accept, counting at most
    /// HapticAudio::max_bytes for each burst.
    std::uint64_t haptic_bytes{};
    /// @brief Haptic audio bursts refused as malformed.
    std::uint64_t haptic_malformed{};
    /// @brief Blocks returned by take_haptic_block.
    std::uint64_t haptic_blocks_taken{};
    /// @brief Bytes discarded because they waited too long.
    std::uint64_t haptic_bytes_stale{};
    /// @brief Bytes discarded because a buffer was full.
    std::uint64_t haptic_bytes_overrun{};
    /// @brief Bytes buffered now, over all seats.
    std::uint64_t haptic_bytes_buffered{};
    /// @brief Rumble changes given to accept.
    std::uint64_t rumble_packets{};
    /// @brief Trigger effect changes given to accept.
    std::uint64_t trigger_packets{};

    /// @brief Compares every field.
    friend constexpr auto operator==(const Statistics&,
                                     const Statistics&) noexcept
        -> bool = default;
  };

  /// @brief Creates an inbox with the default Configuration.
  ///
  /// @param threading Provider of the inbox's mutex; must outlive the inbox.
  /// @param clock Monotonic clock read when no time is passed; must outlive
  ///     the inbox.
  FeedbackInbox(platform::ThreadingProvider& threading,
                const platform::ClockProvider& clock);
  /// @brief Creates an inbox with the given limits.
  ///
  /// @param threading Provider of the inbox's mutex; must outlive the inbox.
  /// @param clock Monotonic clock read when no time is passed; must outlive
  ///     the inbox.
  /// @param configuration The limits. A negative or non-finite gain ceiling
  ///     is taken as 0.
  FeedbackInbox(platform::ThreadingProvider& threading,
                const platform::ClockProvider& clock,
                const Configuration& configuration);
  /// @brief Not copyable; the threads on both sides share one inbox.
  FeedbackInbox(const FeedbackInbox&) = delete;
  /// @brief Not copy-assignable; the threads on both sides share one inbox.
  auto operator=(const FeedbackInbox&) -> FeedbackInbox& = delete;
  /// @brief Discards everything still held.
  ~FeedbackInbox();

  /// @brief Takes in one piece of feedback.
  ///
  /// Haptic audio is copied into the seat's buffer. Rumble and trigger
  /// effects replace the seat's previous value. The two strength settings
  /// apply to all seats; a change of trigger strength makes the trigger
  /// effects of every seat that has any, and of seat 0, available to take
  /// again.
  ///
  /// @param feedback The feedback, typically straight from
  ///     MediaSink::on_feedback.
  /// @param now The time to stamp haptic audio with.
  /// @return What was done with it; see Admission.
  auto accept(const ControllerFeedback& feedback, Clock::time_point now)
      -> Admission;
  /// @brief Takes in one piece of feedback, stamping haptic audio with the
  /// inbox clock's current time.
  ///
  /// @param feedback The feedback, typically straight from
  ///     MediaSink::on_feedback.
  /// @return What was done with it; see Admission.
  auto accept(const ControllerFeedback& feedback) -> Admission;
  /// @brief Takes the oldest block of haptic audio buffered for a seat,
  /// without locking or allocating.
  ///
  /// Audio that has waited longer than Configuration::stale_after is
  /// discarded first; a partial block that went stale is discarded with the
  /// next accept. Call it for a seat from one thread at a time.
  ///
  /// @param seat The seat whose controller will play the block.
  /// @param now The time to measure staleness against.
  /// @return A block, or nothing when less than one block is buffered.
  auto take_haptic_block(SeatNumber seat, Clock::time_point now)
      -> std::optional<HapticBlock>;
  /// @brief Takes the oldest block of haptic audio buffered for a seat,
  /// measuring staleness against the inbox clock's current time; otherwise
  /// as the overload taking a time.
  ///
  /// @param seat The seat whose controller will play the block.
  /// @return A block, or nothing when less than one block is buffered.
  auto take_haptic_block(SeatNumber seat) -> std::optional<HapticBlock>;
  /// @brief Takes a seat's trigger effects if they have changed since they
  /// were last taken.
  ///
  /// @param seat The seat whose controller will apply them.
  /// @return The latest effects with the current trigger strength setting, or
  ///     nothing when there is no change.
  auto take_triggers(SeatNumber seat) -> std::optional<Triggers>;
  /// @brief Takes a seat's rumble strengths if they have changed since they
  /// were last taken.
  ///
  /// @param seat The seat whose controller will apply them.
  /// @return The latest strengths, or nothing when there is no change.
  auto take_rumble(SeatNumber seat) -> std::optional<Rumble>;

  /// @brief Sets the haptics gain used for every seat without its own.
  ///
  /// @param gain The factor, 1 for unchanged. It is limited to between 0 and
  ///     Configuration::gain_ceiling; a non-finite value becomes 0.
  void set_gain(float gain);
  /// @brief Sets or clears one seat's own haptics gain.
  ///
  /// @param seat The seat.
  /// @param gain The factor, limited like the general gain; nothing to return
  ///     the seat to the general gain.
  void set_gain(SeatNumber seat, std::optional<float> gain);
  /// @brief The general haptics gain; 1 unless set.
  ///
  /// @return The general haptics gain; 1 unless set.
  auto gain() const -> float;
  /// @brief The gain in force for one seat: its own if set, otherwise the
  /// general gain.
  ///
  /// @param seat The seat.
  /// @return The gain in force for one seat: its own if set, otherwise the
  ///     general gain.
  auto gain(SeatNumber seat) const -> float;
  /// @brief The general gain multiplied by the console's haptics strength
  /// setting.
  ///
  /// @return gain() times 1 for strong, 0.5 for medium, 0.33 for weak and 0
  ///     for off.
  auto haptic_gain() const -> float;
  /// @brief One seat's gain multiplied by the console's haptics strength
  /// setting; the gain its blocks carry.
  ///
  /// @param seat The seat.
  /// @return One seat's gain multiplied by the console's haptics strength
  ///     setting; the gain its blocks carry.
  auto haptic_gain(SeatNumber seat) const -> float;
  /// @brief The console's haptics strength setting last received; strong
  /// until then.
  ///
  /// @return The console's haptics strength setting last received; strong
  ///     until then.
  auto haptic_intensity() const -> FeedbackIntensity;
  /// @brief The console's trigger strength setting last received; strong
  /// until then.
  ///
  /// @return The console's trigger strength setting last received; strong
  ///     until then.
  auto trigger_intensity() const -> FeedbackIntensity;

  /// @brief Discards everything held for a seat: haptic audio, trigger
  /// effects and rumble.
  ///
  /// Use it when the seat's controller goes away. The seat's own gain is
  /// kept.
  ///
  /// @param seat The seat.
  void forget(SeatNumber seat);
  /// @brief Discards the haptic audio buffered for a seat and keeps the rest.
  ///
  /// @param seat The seat.
  void clear_haptics(SeatNumber seat);
  /// @brief Reports what is waiting to be taken for a seat.
  ///
  /// @param seat The seat.
  /// @return What is waiting to be taken for the seat.
  auto pending(SeatNumber seat) const -> Pending;
  /// @brief Returns a copy of the running totals, with the bytes buffered
  /// now.
  ///
  /// @return A copy of the running totals, with the bytes buffered now.
  auto statistics() const -> Statistics;

 private:
  class Core;

  std::unique_ptr<Core> core_;
};

}  // namespace libchiaking::controllers
