#include "libchiaking/detail/controllers/feedback_inbox.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/utils/bytes/endian.hpp"

#include <rigtorp/SPSCQueue.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <utility>

namespace libchiaking::controllers {

class FeedbackInbox::Core {
 public:
  static constexpr std::array<float, 4> console_haptic_scale{0.0F, 1.0F, 0.5F,
                                                             0.33F};

  Core(platform::ThreadingProvider& threading,
       const platform::ClockProvider& clock, const Configuration& configuration)
      : clock_(clock),
        configuration_(configuration),
        blocks_(std::max<std::size_t>(configuration.buffered_blocks, 1)),
        capacity_(blocks_ * HapticBlock::size),
        mutex_(threading) {
    if (!std::isfinite(configuration_.gain_ceiling) ||
        configuration_.gain_ceiling < 0.0F) {
      configuration_.gain_ceiling = 0.0F;
    }
    for (auto& seat : seats_) {
      seat.ring = std::make_unique<Ring>(blocks_ + ring_spare_blocks);
    }
    gain_ = std::min(gain_, configuration_.gain_ceiling);
    publish_gains();
  }

  auto now() const noexcept -> Clock::time_point {
    return Clock::time_point{std::chrono::microseconds{
        static_cast<std::int64_t>(clock_.now_microseconds())}};
  }

  auto accept(const ControllerFeedback& feedback, Clock::time_point now)
      -> Admission {
    const platform::Lock lock{mutex_};
    auto& seat = seats_[feedback.seat.index()];
    if (const auto* audio = feedback.get<HapticAudio>()) {
      return hold(seat, audio->pcm, now.time_since_epoch().count());
    }
    if (const auto* rumble = feedback.get<Rumble>()) {
      ++statistics_.rumble_packets;
      seat.rumble = *rumble;
      return Admission::held;
    }
    if (const auto* effects = feedback.get<TriggerEffects>()) {
      ++statistics_.trigger_packets;
      seat.triggers = *effects;
      seat.triggers_known = true;
      seat.triggers_pending = true;
      return Admission::held;
    }
    if (const auto* haptic = feedback.get<HapticIntensity>()) {
      if (static_cast<std::size_t>(haptic->level) >=
          console_haptic_scale.size())
        return Admission::malformed;
      haptic_intensity_.store(haptic->level, std::memory_order_release);
      publish_gains();
      return Admission::held;
    }
    if (const auto* trigger = feedback.get<TriggerIntensity>()) {
      trigger_intensity_ = trigger->level;
      for (auto& held : seats_) {
        held.triggers_pending = held.triggers_known;
      }
      seats_.front().triggers_pending = true;
      return Admission::held;
    }
    return Admission::not_held;
  }

  auto take_haptic_block(SeatNumber number, Clock::time_point now) noexcept
      -> std::optional<HapticBlock> {
    auto& seat = seats_[number.index()];
    const auto stamp = now.time_since_epoch().count();
    expire_taken(seat, stamp);
    while (const auto* block = seat.ring->front()) {
      const auto sequence = seat.consumed.load(std::memory_order_relaxed);
      if (sequence < seat.discard_below.load(std::memory_order_acquire)) {
        release_front(seat, sequence);
        continue;
      }
      const HapticBlock taken{*block,
                              haptic_intensity_.load(std::memory_order_acquire),
                              seat.scaled_gain.load(std::memory_order_acquire)};
      release_front(seat, sequence);
      seat.last_taken.store(stamp, std::memory_order_release);
      blocks_taken_.fetch_add(1, std::memory_order_relaxed);
      return taken;
    }
    return std::nullopt;
  }

  auto take_triggers(SeatNumber number) -> std::optional<Triggers> {
    const platform::Lock lock{mutex_};
    auto& seat = seats_[number.index()];
    if (!std::exchange(seat.triggers_pending, false)) {
      return std::nullopt;
    }
    return Triggers{seat.triggers, trigger_intensity_};
  }

  auto take_rumble(SeatNumber number) -> std::optional<Rumble> {
    const platform::Lock lock{mutex_};
    return std::exchange(seats_[number.index()].rumble, std::nullopt);
  }

  void set_gain(float gain) {
    const platform::Lock lock{mutex_};
    gain_ = clamped(gain);
    publish_gains();
  }

  void set_gain(SeatNumber number, std::optional<float> gain) {
    const platform::Lock lock{mutex_};
    seats_[number.index()].gain =
        gain ? std::optional<float>{clamped(*gain)} : std::nullopt;
    publish_gains();
  }

  auto gain() const -> float {
    const platform::Lock lock{mutex_};
    return gain_;
  }

  auto gain(SeatNumber number) const -> float {
    const platform::Lock lock{mutex_};
    return seat_gain(seats_[number.index()]);
  }

  auto haptic_gain() const -> float {
    const platform::Lock lock{mutex_};
    return gain_ * scale();
  }

  auto haptic_gain(SeatNumber number) const -> float {
    const platform::Lock lock{mutex_};
    return scaled_gain(seats_[number.index()]);
  }

  auto haptic_intensity() const -> FeedbackIntensity {
    return haptic_intensity_.load(std::memory_order_acquire);
  }

  auto trigger_intensity() const -> FeedbackIntensity {
    const platform::Lock lock{mutex_};
    return trigger_intensity_;
  }

  void forget(SeatNumber number) {
    const platform::Lock lock{mutex_};
    auto& seat = seats_[number.index()];
    discard_all(seat);
    seat.triggers = {};
    seat.triggers_known = false;
    seat.triggers_pending = false;
    seat.rumble.reset();
  }

  void clear_haptics(SeatNumber number) {
    const platform::Lock lock{mutex_};
    discard_all(seats_[number.index()]);
  }

  auto pending(SeatNumber number) const -> Pending {
    const platform::Lock lock{mutex_};
    const auto& seat = seats_[number.index()];
    return {buffered_blocks(seat), seat.triggers_pending,
            seat.rumble.has_value()};
  }

  auto statistics() const -> Statistics {
    const platform::Lock lock{mutex_};
    auto statistics = statistics_;
    statistics.haptic_blocks_taken =
        blocks_taken_.load(std::memory_order_relaxed);
    statistics.haptic_bytes_stale =
        bytes_stale_.load(std::memory_order_relaxed);
    for (const auto& seat : seats_) {
      statistics.haptic_bytes_buffered += buffered_bytes(seat);
    }
    return statistics;
  }

 private:
  using Block = std::array<std::byte, HapticBlock::size>;
  using Ring = rigtorp::SPSCQueue<Block>;

  static constexpr std::size_t ring_spare_blocks =
      2 * (HapticAudio::max_bytes / HapticBlock::size);

  struct Seat {
    std::unique_ptr<Ring> ring;
    Block partial{};
    std::size_t partial_size{};
    std::atomic<std::uint64_t> produced{};
    std::atomic<std::uint64_t> consumed{};
    std::atomic<std::uint64_t> discard_below{};
    std::atomic<Clock::rep> started{};
    std::atomic<Clock::rep> last_taken{std::numeric_limits<Clock::rep>::min()};
    std::atomic<float> scaled_gain{1.0F};
    TriggerEffects triggers{};
    bool triggers_known{};
    bool triggers_pending{};
    std::optional<Rumble> rumble{};
    std::optional<float> gain{};
  };

  static auto buffered_blocks(const Seat& seat) noexcept -> std::size_t {
    const auto produced = seat.produced.load(std::memory_order_acquire);
    const auto floor =
        std::max(seat.consumed.load(std::memory_order_acquire),
                 seat.discard_below.load(std::memory_order_acquire));
    return produced > floor ? static_cast<std::size_t>(produced - floor) : 0;
  }

  static auto buffered_bytes(const Seat& seat) noexcept -> std::size_t {
    return buffered_blocks(seat) * HapticBlock::size + seat.partial_size;
  }

  static void release_front(Seat& seat, std::uint64_t sequence) noexcept {
    seat.ring->pop();
    seat.consumed.store(sequence + 1, std::memory_order_release);
  }

  static void discard_all(Seat& seat) noexcept {
    seat.discard_below.store(seat.produced.load(std::memory_order_relaxed),
                             std::memory_order_release);
    seat.partial_size = 0;
  }

  static void discard_oldest(Seat& seat) noexcept {
    const auto floor =
        std::max(seat.consumed.load(std::memory_order_acquire),
                 seat.discard_below.load(std::memory_order_relaxed));
    seat.discard_below.store(floor + 1, std::memory_order_release);
  }

  auto stale(const Seat& seat, Clock::rep now) const noexcept -> bool {
    const auto started =
        std::max(seat.started.load(std::memory_order_acquire),
                 seat.last_taken.load(std::memory_order_acquire));
    return Clock::duration{now - started} > configuration_.stale_after;
  }

  auto hold(Seat& seat, std::span<const std::byte> pcm, Clock::rep now)
      -> Admission {
    ++statistics_.haptic_packets;
    statistics_.haptic_bytes += std::min(pcm.size(), HapticAudio::max_bytes);
    if (pcm.empty() || pcm.size() % HapticAudio::frame_bytes != 0 ||
        pcm.size() > HapticAudio::max_bytes) {
      ++statistics_.haptic_malformed;
      return Admission::malformed;
    }
    while (!pcm.empty()) {
      expire_held(seat, now);
      if (buffered_bytes(seat) == 0) {
        seat.started.store(now, std::memory_order_release);
      }
      const auto available = capacity_ - buffered_bytes(seat);
      if (available == 0) {
        make_room(seat);
        continue;
      }
      const auto copied = pcm.first(std::min(
          {pcm.size(), available, HapticBlock::size - seat.partial_size}));
      std::ranges::copy(copied,
                        seat.partial.begin() +
                            static_cast<std::ptrdiff_t>(seat.partial_size));
      seat.partial_size += copied.size();
      pcm = pcm.subspan(copied.size());
      if (seat.partial_size == HapticBlock::size) {
        commit(seat);
      }
    }
    return Admission::held;
  }

  void commit(Seat& seat) noexcept {
    seat.partial_size = 0;
    if (!seat.ring->try_push(seat.partial)) {
      statistics_.haptic_bytes_overrun += HapticBlock::size;
      return;
    }
    seat.produced.fetch_add(1, std::memory_order_release);
  }

  void make_room(Seat& seat) noexcept {
    if (configuration_.overrun == Overrun::drop_oldest_block) {
      statistics_.haptic_bytes_overrun += HapticBlock::size;
      discard_oldest(seat);
      return;
    }
    statistics_.haptic_bytes_overrun += buffered_bytes(seat);
    discard_all(seat);
  }

  void expire_held(Seat& seat, Clock::rep now) noexcept {
    const auto buffered = buffered_bytes(seat);
    if (buffered == 0 || !stale(seat, now)) {
      return;
    }
    bytes_stale_.fetch_add(buffered, std::memory_order_relaxed);
    discard_all(seat);
  }

  void expire_taken(Seat& seat, Clock::rep now) noexcept {
    if (buffered_blocks(seat) == 0 || !stale(seat, now)) {
      return;
    }
    const auto produced = seat.produced.load(std::memory_order_acquire);
    auto sequence = seat.consumed.load(std::memory_order_relaxed);
    while (sequence < produced && seat.ring->front() != nullptr) {
      if (sequence >= seat.discard_below.load(std::memory_order_acquire)) {
        bytes_stale_.fetch_add(HapticBlock::size, std::memory_order_relaxed);
      }
      release_front(seat, sequence);
      ++sequence;
    }
  }

  void publish_gains() noexcept {
    for (auto& seat : seats_) {
      seat.scaled_gain.store(scaled_gain(seat), std::memory_order_release);
    }
  }

  auto clamped(float gain) const noexcept -> float {
    return std::isfinite(gain)
               ? std::clamp(gain, 0.0F, configuration_.gain_ceiling)
               : 0.0F;
  }

  auto scale() const noexcept -> float {
    return console_haptic_scale[static_cast<std::size_t>(
        haptic_intensity_.load(std::memory_order_acquire))];
  }

  auto seat_gain(const Seat& seat) const noexcept -> float {
    return seat.gain.value_or(gain_);
  }
  auto scaled_gain(const Seat& seat) const noexcept -> float {
    return seat_gain(seat) * scale();
  }

  const platform::ClockProvider& clock_;
  Configuration configuration_;
  std::size_t blocks_;
  std::size_t capacity_;
  mutable platform::Mutex mutex_;
  std::array<Seat, SeatNumber::count> seats_{};
  float gain_{1.0F};
  std::atomic<FeedbackIntensity> haptic_intensity_{FeedbackIntensity::strong};
  FeedbackIntensity trigger_intensity_{FeedbackIntensity::strong};
  Statistics statistics_{};
  std::atomic<std::uint64_t> blocks_taken_{};
  std::atomic<std::uint64_t> bytes_stale_{};
};

HapticBlock::HapticBlock(std::span<const std::byte, size> pcm,
                         FeedbackIntensity intensity, float gain) noexcept
    : intensity_(intensity), gain_(gain) {
  std::ranges::copy(pcm, pcm_.begin());
}

auto HapticBlock::sample_at(std::span<const std::byte, size> pcm,
                            std::size_t index) noexcept -> std::int16_t {
  return static_cast<std::int16_t>(
      bytes::load_le<std::uint16_t>(pcm.subspan(index * 2)));
}

auto HapticBlock::saturated(float value) noexcept -> std::int16_t {
  if (value > static_cast<float>(std::numeric_limits<std::int16_t>::max()))
    return std::numeric_limits<std::int16_t>::max();
  if (value < static_cast<float>(std::numeric_limits<std::int16_t>::min()))
    return std::numeric_limits<std::int16_t>::min();
  return static_cast<std::int16_t>(value);
}

auto HapticBlock::scaled_samples() const noexcept -> Samples {
  Samples samples{};
  for (std::size_t index = 0; index < samples.size(); ++index)
    samples[index] =
        saturated(static_cast<float>(sample_at(pcm_, index)) * gain_);
  return samples;
}

auto HapticBlock::silent() const noexcept -> bool {
  constexpr float silence_divisor = 256.0F;
  for (std::size_t index = 0; index < frames * channels; ++index) {
    const auto level =
        static_cast<float>(sample_at(pcm_, index)) * gain_ / silence_divisor;
    if (std::fabs(level) >= 1.0F) return false;
  }
  return true;
}

FeedbackInbox::FeedbackInbox(platform::ThreadingProvider& threading,
                             const platform::ClockProvider& clock)
    : FeedbackInbox(threading, clock, Configuration{}) {}

FeedbackInbox::FeedbackInbox(platform::ThreadingProvider& threading,
                             const platform::ClockProvider& clock,
                             const Configuration& configuration)
    : core_(std::make_unique<Core>(threading, clock, configuration)) {}

FeedbackInbox::~FeedbackInbox() = default;

auto FeedbackInbox::accept(const ControllerFeedback& feedback,
                           Clock::time_point now) -> Admission {
  return core_->accept(feedback, now);
}

auto FeedbackInbox::accept(const ControllerFeedback& feedback) -> Admission {
  return core_->accept(feedback, core_->now());
}

auto FeedbackInbox::take_haptic_block(SeatNumber seat, Clock::time_point now)
    -> std::optional<HapticBlock> {
  return core_->take_haptic_block(seat, now);
}

auto FeedbackInbox::take_haptic_block(SeatNumber seat)
    -> std::optional<HapticBlock> {
  return core_->take_haptic_block(seat, core_->now());
}

auto FeedbackInbox::take_triggers(SeatNumber seat) -> std::optional<Triggers> {
  return core_->take_triggers(seat);
}

auto FeedbackInbox::take_rumble(SeatNumber seat) -> std::optional<Rumble> {
  return core_->take_rumble(seat);
}

void FeedbackInbox::set_gain(float gain) { core_->set_gain(gain); }

void FeedbackInbox::set_gain(SeatNumber seat, std::optional<float> gain) {
  core_->set_gain(seat, gain);
}

auto FeedbackInbox::gain() const -> float { return core_->gain(); }

auto FeedbackInbox::gain(SeatNumber seat) const -> float {
  return core_->gain(seat);
}

auto FeedbackInbox::haptic_gain() const -> float {
  return core_->haptic_gain();
}

auto FeedbackInbox::haptic_gain(SeatNumber seat) const -> float {
  return core_->haptic_gain(seat);
}

auto FeedbackInbox::haptic_intensity() const -> FeedbackIntensity {
  return core_->haptic_intensity();
}

auto FeedbackInbox::trigger_intensity() const -> FeedbackIntensity {
  return core_->trigger_intensity();
}

void FeedbackInbox::forget(SeatNumber seat) { core_->forget(seat); }

void FeedbackInbox::clear_haptics(SeatNumber seat) {
  core_->clear_haptics(seat);
}

auto FeedbackInbox::pending(SeatNumber seat) const -> Pending {
  return core_->pending(seat);
}

auto FeedbackInbox::statistics() const -> Statistics {
  return core_->statistics();
}

}  // namespace libchiaking::controllers
