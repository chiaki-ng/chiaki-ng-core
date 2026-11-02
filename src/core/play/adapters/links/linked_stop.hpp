/// @file
/// @brief Declares LinkedStop, a stop state that stops when its parent does
/// or when cancelled itself.
#pragma once

#include "libchiaking/platform/runtime/threading.hpp"

#include <atomic>

namespace libchiaking::play {

/// @brief A stop token's state that a link can cancel on its own while still
/// following the session's stop.
///
/// Its own cancellation is an atomic flag, so any thread may cancel or ask.
/// A deadline of the parent counts only while it was not cancelled.
class LinkedStop final : public platform::StopState {
 public:
  /// @brief Links to a parent.
  ///
  /// @param parent The session's stop.
  explicit LinkedStop(platform::StopToken parent) noexcept;

  /// @brief Requests a stop of this state alone.
  void cancel() noexcept;
  /// @brief Reports whether a stop was requested.
  ///
  /// @return `true` once cancelled or once the parent stopped.
  bool stop_requested() const noexcept override;
  /// @brief Reports whether the parent's deadline passed.
  ///
  /// @return `true` when it passed and this state was not cancelled.
  bool deadline_expired() const noexcept override;

 private:
  /// @brief The session's stop.
  platform::StopToken parent_;
  /// @brief Whether this state was cancelled.
  std::atomic<bool> cancelled_{};
};

}  // namespace libchiaking::play
