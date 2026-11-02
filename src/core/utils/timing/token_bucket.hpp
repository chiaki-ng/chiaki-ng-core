/// @file
/// @brief Declares TokenBucket, the rate limiter behind Kakusan's transport
/// limits and the pacing of requests to PSN.
#pragma once

#include <cstdint>

namespace libchiaking::timing {

/// @brief A token bucket on the caller's monotonic clock, used for Kakusan's
/// rate limits (control messages, control bytes, media acknowledgements) and
/// for pacing requests to PSN.
///
/// Tokens are kept in millionths so fractional refills are not lost. Starts
/// full. Not thread-safe.
class TokenBucket {
 public:
  /// @brief Makes a full bucket.
  ///
  /// @param tokens_per_second Refill rate; must not be 0.
  /// @param capacity Most tokens held.
  /// @param now_us The current monotonic time, in microseconds.
  TokenBucket(std::uint64_t tokens_per_second, std::uint64_t capacity,
              std::uint64_t now_us) noexcept;

  /// @brief Adds the tokens earned since the last refill, up to capacity.
  ///
  /// @param now_us The current monotonic time; earlier times are ignored.
  void refill(std::uint64_t now_us) noexcept;
  /// @brief Whether the bucket holds enough, without refilling.
  ///
  /// @param amount Tokens wanted.
  /// @return True when at least that many are held.
  auto holds(std::uint64_t amount) const noexcept -> bool;
  /// @brief Takes tokens if enough are held, without refilling.
  ///
  /// @param amount Tokens wanted.
  /// @return Whether they were taken.
  [[nodiscard]] auto take(std::uint64_t amount) noexcept -> bool;
  /// @brief When enough tokens will be held.
  ///
  /// @param amount Tokens wanted.
  /// @return The monotonic instant, in microseconds; the last refill time
  ///     when
  ///     already enough.
  auto ready_at_us(std::uint64_t amount) const noexcept -> std::uint64_t;

 private:
  /// @brief Units per token.
  static constexpr std::uint64_t scale = 1'000'000;

  /// @brief Refill rate, units per microsecond (tokens per second).
  std::uint64_t rate_;
  /// @brief Most units held.
  std::uint64_t capacity_;
  /// @brief Units held.
  std::uint64_t tokens_;
  /// @brief When the bucket was last refilled, monotonic microseconds.
  std::uint64_t updated_us_;
};

}  // namespace libchiaking::timing
