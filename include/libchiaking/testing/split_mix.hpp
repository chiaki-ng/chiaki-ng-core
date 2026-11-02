/// @file
/// @brief Declares SplitMix64, the deterministic generator behind the test
/// doubles' random bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace libchiaking::testing {

/// @brief Sebastiano Vigna's SplitMix64 generator, for tests that need
/// repeatable randomness.
///
/// A seed fixes the whole sequence: two generators built with the same seed
/// produce the same words and the same bytes in the same order. The fakes in
/// libchiaking::testing use it wherever they stand in for a random source, so a
/// test that seeds them sees the same nonces, keys and identifiers on every
/// run.
///
/// @warning Not cryptographically secure. It must never feed real keys.
///
/// @note A generator does no locking. Use one from one thread at a time.
class SplitMix64 {
 public:
  /// @brief The step added to the state before each word, 2^64 divided by
  /// the golden ratio.
  static constexpr std::uint64_t golden_gamma = 0x9e3779b97f4a7c15ULL;

  /// @brief Starts a sequence.
  ///
  /// @param seed Chooses the sequence. Any value, including zero, is valid.
  constexpr explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

  /// @brief Applies the SplitMix64 finaliser to one state value.
  ///
  /// It is a bijection that spreads every input bit over the whole output.
  /// next() returns `mix` of the advanced state; a fake that keeps its own
  /// state can call it directly.
  ///
  /// @param state The value to scramble.
  /// @return The scrambled word. Never fails and does not block.
  static constexpr auto mix(std::uint64_t state) noexcept -> std::uint64_t {
    state = (state ^ (state >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    state = (state ^ (state >> 27U)) * 0x94d049bb133111ebULL;
    return state ^ (state >> 31U);
  }

  /// @brief Advances the state by golden_gamma and returns the next word.
  ///
  /// It does not touch the bytes fill() has buffered.
  ///
  /// @return The next 64-bit word of the sequence. Never fails and does not
  ///     block.
  constexpr auto next() noexcept -> std::uint64_t {
    state_ += golden_gamma;
    return mix(state_);
  }

  /// @brief Draws a value smaller than a bound.
  ///
  /// It takes the next word modulo @p bound, so it is slightly biased
  /// towards small values; that is fine for scheduling and loss in tests.
  ///
  /// @param bound One more than the largest value wanted.
  /// @return A value in `[0, bound)`, or zero without advancing the
  ///     sequence when @p bound is zero. Never fails and does not block.
  constexpr auto below(std::uint64_t bound) noexcept -> std::uint64_t {
    return bound == 0 ? 0 : next() % bound;
  }

  /// @brief Overwrites a buffer with the next bytes of the sequence.
  ///
  /// Bytes are handed out from each word low byte first. Bytes of a word
  /// left over when @p output ends are kept and start the next call, so
  /// filling eight bytes once gives the same bytes as filling one byte
  /// eight times.
  ///
  /// Never fails and does not block.
  ///
  /// @param output Buffer to fill completely. May be empty.
  constexpr void fill(std::span<std::byte> output) noexcept {
    for (auto& byte : output) {
      if (buffered_ == 0) {
        word_ = next();
        buffered_ = sizeof(word_);
      }
      byte = static_cast<std::byte>(word_ & 0xffU);
      word_ >>= 8U;
      --buffered_;
    }
  }

 private:
  std::uint64_t state_;
  std::uint64_t word_{};
  std::size_t buffered_{};
};

}  // namespace libchiaking::testing
