/// @file
/// @brief Declares AudioRouteAdvertisement, which repeats the audio route
/// report to PS5 cloud while a title starts.
#pragma once

#include <chrono>
#include <cstdint>

namespace libchiaking::play {

/// @brief Tracks the window after the stream is ready in which the audio
/// routes are reported again every second, since titles open their audio
/// ports some time after the stream starts.
///
/// A small value; not synchronised, used by ConsoleCommands from the session
/// thread.
class AudioRouteAdvertisement {
 public:
  /// @brief Where the repeats stand.
  enum class State : std::uint8_t {
    /// @brief The stream is not ready yet.
    idle,
    /// @brief The routes are repeated.
    advertising,
    /// @brief The window is over.
    settled,
  };

  /// @brief How long the routes are repeated after the stream is ready.
  static constexpr std::chrono::microseconds repeat_window{
      std::chrono::seconds{30}};
  /// @brief The time between two repeats.
  static constexpr std::chrono::microseconds repeat_spacing{
      std::chrono::seconds{1}};

  /// @brief Tells where the repeats stand.
  ///
  /// @return The state.
  [[nodiscard]] constexpr auto state() const noexcept -> State {
    return state_;
  }

  /// @brief Records that the routes were first reported and opens the window.
  ///
  /// @param now_us The monotonic time, in microseconds.
  constexpr void started(std::uint64_t now_us) noexcept {
    state_ = State::advertising;
    until_us_ = now_us + static_cast<std::uint64_t>(repeat_window.count());
    next_us_ = now_us + static_cast<std::uint64_t>(repeat_spacing.count());
  }

  /// @brief Tells whether a repeat is due.
  ///
  /// @param now_us The monotonic time, in microseconds.
  /// @return `true` inside the window once the spacing has passed.
  [[nodiscard]] constexpr auto due(std::uint64_t now_us) const noexcept
      -> bool {
    return state_ == State::advertising && now_us >= next_us_;
  }

  /// @brief Records a repeat; the window closes once the next repeat would
  /// fall outside it.
  ///
  /// @param now_us The monotonic time, in microseconds.
  constexpr void repeated(std::uint64_t now_us) noexcept {
    if (state_ != State::advertising) return;
    next_us_ = now_us + static_cast<std::uint64_t>(repeat_spacing.count());
    if (next_us_ > until_us_) state_ = State::settled;
  }

  /// @brief Tells how long until the next repeat.
  ///
  /// @param now_us The monotonic time, in microseconds.
  /// @return The time left; 0 outside the window or when a repeat is due.
  [[nodiscard]] constexpr auto remaining(std::uint64_t now_us) const noexcept
      -> std::chrono::microseconds {
    if (state_ != State::advertising || now_us >= next_us_)
      return std::chrono::microseconds{0};
    return std::chrono::microseconds{next_us_ - now_us};
  }

 private:
  /// @brief Where the repeats stand.
  State state_{State::idle};
  /// @brief When the window closes, in monotonic microseconds.
  std::uint64_t until_us_{};
  /// @brief When the next repeat is due, in monotonic microseconds.
  std::uint64_t next_us_{};
};

}  // namespace libchiaking::play
