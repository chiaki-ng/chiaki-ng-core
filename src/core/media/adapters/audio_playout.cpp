#include "libchiaking/detail/media/audio_playout.hpp"

#include "libchiaking/detail/media/audio_decoder.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <new>
#include <utility>

#include <rigtorp/SPSCQueue.h>

namespace libchiaking::media {

static_assert(AudioPlayout::max_channels == AudioDecoder::max_output_channels);

namespace {

constexpr std::size_t cache_line = 64;

using BlockSamples =
    std::array<std::int16_t, std::size_t{AudioPlayout::block_frames} *
                                 AudioPlayout::max_channels>;

struct Block {
  Block(const BlockSamples& source, std::uint32_t count) noexcept
      : frames(count), samples(source) {}

  std::uint32_t frames{};
  BlockSamples samples;
};

void raise_maximum(std::atomic<std::uint64_t>& maximum,
                   std::uint64_t value) noexcept {
  auto seen = maximum.load(std::memory_order_relaxed);
  while (value > seen && !maximum.compare_exchange_weak(
                             seen, value, std::memory_order_relaxed)) {
  }
}

}  // namespace

struct AudioPlayout::Impl {
  Impl(std::uint32_t channel_count, std::uint32_t rate, std::uint64_t target,
       std::size_t blocks)
      : channels(channel_count),
        sample_rate(rate),
        target_frames(target),
        queue(blocks) {}

  struct alignas(cache_line) Produced {
    std::atomic<std::uint64_t> queued{};
    std::atomic<std::uint64_t> pushed{};
    std::atomic<std::uint64_t> overflows{};
    std::atomic<std::uint64_t> dropped{};
    std::atomic<std::uint64_t> maximum{};
  };

  struct alignas(cache_line) Consumed {
    std::atomic<std::uint64_t> taken{};
    std::atomic<std::uint64_t> underruns{};
    std::atomic<std::uint64_t> silent{};
    std::atomic<std::uint64_t> trims{};
    std::atomic<std::uint64_t> trimmed{};
    std::uint32_t offset{};
    std::uint64_t window{};
    std::uint64_t lowest{std::numeric_limits<std::uint64_t>::max()};
  };

  auto buffered() const noexcept -> std::uint64_t {
    const auto queued = produced.queued.load(std::memory_order_acquire);
    const auto taken = consumed.taken.load(std::memory_order_acquire);
    return queued > taken ? queued - taken : 0;
  }

  auto skip(std::uint64_t frames) noexcept -> std::uint64_t {
    std::uint64_t skipped = 0;
    while (skipped < frames) {
      auto* block = queue.front();
      if (block == nullptr) {
        break;
      }
      const std::uint64_t left = block->frames - consumed.offset;
      if (left > frames - skipped) {
        consumed.offset += static_cast<std::uint32_t>(frames - skipped);
        consumed.taken.fetch_add(frames - skipped, std::memory_order_release);
        skipped = frames;
        break;
      }
      skipped += left;
      consumed.offset = 0;
      consumed.taken.fetch_add(left, std::memory_order_release);
      queue.pop();
    }
    return skipped;
  }

  void trim(std::uint64_t residual, std::uint64_t pulled) noexcept {
    consumed.lowest = std::min(consumed.lowest, residual);
    consumed.window += pulled;
    if (consumed.window < sample_rate / trim_windows_per_second) {
      return;
    }
    const auto lowest = consumed.lowest;
    consumed.window = 0;
    consumed.lowest = std::numeric_limits<std::uint64_t>::max();
    if (lowest < target_frames * trim_above_targets) {
      return;
    }
    const auto skipped = skip(lowest - target_frames);
    consumed.trims.fetch_add(1, std::memory_order_relaxed);
    consumed.trimmed.fetch_add(skipped, std::memory_order_relaxed);
  }

  const std::uint32_t channels;
  const std::uint32_t sample_rate;
  const std::uint64_t target_frames;
  rigtorp::SPSCQueue<Block> queue;
  BlockSamples staged{};
  std::uint32_t staged_frames{};
  Produced produced;
  Consumed consumed;
};

auto AudioPlayout::create(std::uint32_t channels, std::uint32_t sample_rate,
                          std::uint32_t target_ms)
    -> std::optional<AudioPlayout> {
  if (channels == 0 || channels > max_channels || sample_rate == 0) {
    return std::nullopt;
  }
  const auto target = std::clamp(target_ms, min_target_ms, max_target_ms);
  const std::uint64_t target_frames =
      std::max<std::uint64_t>(std::uint64_t{sample_rate} * target / 1000, 1);
  const auto capacity = target_frames * capacity_targets;
  const auto blocks = static_cast<std::size_t>(
      std::max((capacity + block_frames - 1) / block_frames,
               (target_frames + block_frames - 1) / block_frames + 2));
  try {
    return AudioPlayout{
        std::make_unique<Impl>(channels, sample_rate, target_frames, blocks)};
  } catch (const std::bad_alloc&) {
    return std::nullopt;
  }
}

AudioPlayout::AudioPlayout(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

AudioPlayout::~AudioPlayout() = default;

AudioPlayout::AudioPlayout(AudioPlayout&&) noexcept = default;

auto AudioPlayout::operator=(AudioPlayout&&) noexcept
    -> AudioPlayout& = default;

void AudioPlayout::push(std::span<const std::int16_t> samples) noexcept {
  auto& state = *impl_;
  const std::uint64_t frames = samples.size() / state.channels;
  state.produced.pushed.fetch_add(frames, std::memory_order_relaxed);
  std::uint64_t used = 0;
  std::uint64_t dropped = 0;
  while (used < frames) {
    const auto take = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        block_frames - state.staged_frames, frames - used));
    const auto source = samples.subspan(used * state.channels,
                                        std::size_t{take} * state.channels);
    std::copy(source.begin(), source.end(),
              state.staged.begin() +
                  static_cast<std::ptrdiff_t>(std::size_t{state.staged_frames} *
                                              state.channels));
    state.staged_frames += take;
    used += take;
    if (state.staged_frames < block_frames) {
      break;
    }
    state.staged_frames = 0;
    if (state.queue.try_emplace(state.staged, block_frames)) {
      state.produced.queued.fetch_add(block_frames, std::memory_order_release);
    } else {
      dropped += block_frames;
    }
  }
  if (dropped != 0) {
    state.produced.overflows.fetch_add(1, std::memory_order_relaxed);
    state.produced.dropped.fetch_add(dropped, std::memory_order_relaxed);
  }
  raise_maximum(state.produced.maximum, state.buffered());
}

auto AudioPlayout::pull(std::span<std::int16_t> samples) noexcept
    -> std::size_t {
  auto& state = *impl_;
  const std::size_t wanted = samples.size() / state.channels;
  std::size_t supplied = 0;
  while (supplied < wanted) {
    auto* block = state.queue.front();
    if (block == nullptr) {
      break;
    }
    const auto count = std::min<std::size_t>(
        block->frames - state.consumed.offset, wanted - supplied);
    const auto from = block->samples.begin() +
                      static_cast<std::ptrdiff_t>(
                          std::size_t{state.consumed.offset} * state.channels);
    std::copy_n(from, count * state.channels,
                samples.begin() +
                    static_cast<std::ptrdiff_t>(supplied * state.channels));
    supplied += count;
    state.consumed.offset += static_cast<std::uint32_t>(count);
    state.consumed.taken.fetch_add(count, std::memory_order_release);
    if (state.consumed.offset == block->frames) {
      state.consumed.offset = 0;
      state.queue.pop();
    }
  }
  std::fill(
      samples.begin() + static_cast<std::ptrdiff_t>(supplied * state.channels),
      samples.end(), std::int16_t{0});
  if (supplied < wanted) {
    state.consumed.underruns.fetch_add(1, std::memory_order_relaxed);
    state.consumed.silent.fetch_add(wanted - supplied,
                                    std::memory_order_relaxed);
  }
  state.trim(state.buffered(), wanted);
  return supplied;
}

auto AudioPlayout::statistics() const noexcept -> Statistics {
  const auto& state = *impl_;
  return Statistics{
      .pushed_frames = state.produced.pushed.load(std::memory_order_relaxed),
      .overflows = state.produced.overflows.load(std::memory_order_relaxed),
      .dropped_frames = state.produced.dropped.load(std::memory_order_relaxed),
      .underruns = state.consumed.underruns.load(std::memory_order_relaxed),
      .silent_frames = state.consumed.silent.load(std::memory_order_relaxed),
      .trims = state.consumed.trims.load(std::memory_order_relaxed),
      .trimmed_frames = state.consumed.trimmed.load(std::memory_order_relaxed),
      .buffered_frames = state.buffered(),
      .maximum_buffered_frames =
          state.produced.maximum.load(std::memory_order_relaxed),
  };
}

auto AudioPlayout::buffered_frames() const noexcept -> std::uint64_t {
  return impl_->buffered();
}

auto AudioPlayout::target_frames() const noexcept -> std::uint64_t {
  return impl_->target_frames;
}

}  // namespace libchiaking::media
