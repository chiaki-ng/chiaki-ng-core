/// @file
/// @brief Declares Audio3dPacing, which spaces out 3D audio changes sent to
/// the console.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace libchiaking::play {

/// @brief Remembers when a 3D audio change was last sent and how long until
/// the next may go.
///
/// A small value; not synchronised, used by ConsoleCommands from the session
/// thread.
class Audio3dPacing {
 public:
  /// @brief The least time between two changes.
  static constexpr std::chrono::milliseconds spacing{2000};

  /// @brief Records that a change was sent.
  ///
  /// @param now_microseconds The monotonic time, in microseconds.
  void sent(std::uint64_t now_microseconds) noexcept {
    last_sent_ = now_microseconds;
  }

  /// @brief Tells how long until the next change may be sent.
  ///
  /// @param now_microseconds The monotonic time, in microseconds.
  /// @return The time left of Audio3dPacing::spacing; 0 when nothing was sent
  ///     yet, the spacing passed, or the clock went back.
  auto remaining(std::uint64_t now_microseconds) const noexcept
      -> std::chrono::milliseconds {
    if (!last_sent_ || now_microseconds < *last_sent_)
      return std::chrono::milliseconds{0};
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::microseconds{now_microseconds - *last_sent_});
    return std::max(spacing - elapsed, std::chrono::milliseconds{0});
  }

 private:
  /// @brief When the last change was sent, in monotonic microseconds.
  std::optional<std::uint64_t> last_sent_;
};

}  // namespace libchiaking::play
