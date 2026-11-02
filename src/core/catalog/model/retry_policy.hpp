/// @file
/// @brief Declares RetryPolicy, how often the catalog asks PS again after a
/// request fails and how long it pauses first.
#pragma once

#include <chrono>

namespace libchiaking::catalog {

/// @brief How many times one catalog request is asked, and the pause before
/// each retry.
///
/// The adapters ask, and stop at once when the caller cancels; the policy
/// only says whether another attempt is due. A plain value; copy freely.
class RetryPolicy {
 public:
  /// @brief A shelf of the LegacyCloudStream store: asked twice, half a
  /// second apart, then skipped.
  ///
  /// @return The policy.
  static constexpr auto shelf() noexcept -> RetryPolicy {
    return RetryPolicy{2, std::chrono::milliseconds{500}};
  }
  /// @brief A page of a Store streaming category: asked three times, without
  /// a pause.
  ///
  /// @return The policy.
  static constexpr auto category_page() noexcept -> RetryPolicy {
    return RetryPolicy{3, std::chrono::milliseconds{0}};
  }

  /// @brief How many times a request is asked in all.
  ///
  /// @return The attempts, at least one.
  constexpr auto attempts() const noexcept -> int { return attempts_; }
  /// @brief The pause before each retry.
  ///
  /// @return The pause; zero asks again at once.
  constexpr auto pause() const noexcept -> std::chrono::milliseconds {
    return pause_;
  }
  /// @brief Whether a request that has failed is asked again.
  ///
  /// @param attempts_made How many times it has been asked so far.
  /// @return True while fewer than attempts() asks have been made.
  constexpr auto asks_again(int attempts_made) const noexcept -> bool {
    return attempts_made < attempts_;
  }

 private:
  /// @brief Makes a policy.
  ///
  /// @param attempts How many times a request is asked in all.
  /// @param pause The pause before each retry.
  constexpr RetryPolicy(int attempts, std::chrono::milliseconds pause) noexcept
      : attempts_(attempts), pause_(pause) {}

  /// @brief How many times a request is asked in all.
  int attempts_;
  /// @brief The pause before each retry.
  std::chrono::milliseconds pause_;
};

}  // namespace libchiaking::catalog
