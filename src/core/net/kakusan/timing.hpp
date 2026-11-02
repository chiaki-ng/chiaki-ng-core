/// @file
/// @brief Declares the timers of an active Kakusan spoke: sequence freshness,
/// controller-state pacing and the PING heartbeat.
#pragma once

#include "libchiaking/detail/secret_array.hpp"
#include "src/core/net/kakusan/control_message.hpp"

#include <cstdint>
#include <optional>

namespace libchiaking::net::kakusan {

/// @brief How often a PING is sent, in microseconds: 2 seconds.
inline constexpr std::uint64_t ping_interval_us = 2'000'000;
/// @brief Silence after which a spoke is closed, in microseconds: 6 seconds.
inline constexpr std::uint64_t liveness_timeout_us = 6'000'000;
/// @brief Longest gap between controller or rumble states, in microseconds:
/// 1/60 s.
inline constexpr std::uint64_t state_period_us = 16'666;
/// @brief Shortest gap between states, in microseconds: 1/240 s.
inline constexpr std::uint64_t state_min_gap_us = 4'167;

/// @brief The sequence freshness rule for one direction of controller or rumble
/// records: the first is accepted, then only newer ones within half the 32-bit
/// space. Not thread-safe.
class Freshness {
 public:
  /// @brief Accepts a sequence if it is fresh, remembering it.
  ///
  /// @param sequence The received sequence.
  /// @return Whether it was accepted.
  [[nodiscard]] auto accept(std::uint32_t sequence) noexcept -> bool;

 private:
  /// @brief Whether any sequence was accepted.
  bool seen_{};
  /// @brief The last accepted sequence.
  std::uint32_t last_{};
};

/// @brief When the next controller or rumble state is due: at once at start,
/// 1/240 s after the last when the state changed, 1/60 s after when periodic.
/// Not thread-safe.
class StatePacer {
 public:
  /// @brief Starts pacing; the first state is due at once.
  ///
  /// @param now_us The current monotonic time, in microseconds.
  void start(std::uint64_t now_us) noexcept;

  /// @brief Stops pacing.
  void stop() noexcept;

  /// @brief Records that a state was sent.
  ///
  /// @param now_us When, in monotonic microseconds.
  void mark(std::uint64_t now_us) noexcept;

  /// @brief When the next state may be sent.
  ///
  /// @param changed Whether the state changed since the last send.
  /// @param periodic Whether states are due even when unchanged.
  /// @return The monotonic instant; nothing when stopped or nothing is due.
  [[nodiscard]] auto next_send_us(bool changed, bool periodic) const noexcept
      -> std::optional<std::uint64_t>;

 private:
  /// @brief Whether pacing runs.
  bool started_{};
  /// @brief Whether a state was sent since start.
  bool sent_{};
  /// @brief When pacing started.
  std::uint64_t start_{};
  /// @brief When the last state was sent.
  std::uint64_t last_{};
};

/// @brief The PING liveness rule from admission on: one PING every 2 seconds,
/// at most one outstanding, and the spoke is silent after 6 seconds without
/// valid incoming traffic. The nonce is wiped on stop. Not thread-safe.
class Heartbeat {
 public:
  /// @brief Starts the heartbeat; the first PING is due 2 seconds later.
  ///
  /// @param now_us The current monotonic time, in microseconds.
  void start(std::uint64_t now_us) noexcept;

  /// @brief Stops the heartbeat and forgets the outstanding nonce.
  void stop() noexcept;

  /// @brief Records valid incoming application traffic.
  ///
  /// @param now_us When it arrived.
  void refresh(std::uint64_t now_us) noexcept;

  /// @brief Whether the heartbeat runs.
  ///
  /// @return True between start() and stop().
  auto running() const noexcept -> bool { return running_; }

  /// @brief Whether the peer has been silent too long.
  ///
  /// @param now_us The current monotonic time.
  /// @return True when running and 6 seconds passed since the last traffic.
  auto silent(std::uint64_t now_us) const noexcept -> bool;

  /// @brief Whether a PING should be sent.
  ///
  /// @param now_us The current monotonic time.
  /// @return True when running and the interval has passed.
  auto ping_due(std::uint64_t now_us) const noexcept -> bool;

  /// @brief Records a PING sent, replacing any unanswered one, and schedules
  /// the next.
  ///
  /// @param nonce Its nonce.
  /// @param now_us When it was sent.
  void issue(const control::Nonce& nonce, std::uint64_t now_us) noexcept;

  /// @brief Matches a PONG to the outstanding PING, in constant time.
  ///
  /// @param nonce The PONG's nonce.
  /// @return True when it answers the outstanding PING; unsolicited or stale
  ///     PONGs give false and are ignored.
  [[nodiscard]] auto answer(const control::Nonce& nonce) noexcept -> bool;

  /// @brief When the heartbeat next needs attention.
  ///
  /// @return The earlier of the next PING and the silence deadline; nothing
  ///     when stopped.
  auto next_deadline_us() const noexcept -> std::optional<std::uint64_t>;

 private:
  /// @brief Whether the heartbeat runs.
  bool running_{};
  /// @brief Whether a PING awaits its PONG.
  bool outstanding_{};
  /// @brief The outstanding PING's nonce.
  SecretArray<std::tuple_size_v<control::Nonce>> nonce_;
  /// @brief When valid traffic last arrived.
  std::uint64_t last_incoming_{};
  /// @brief When the next PING is due.
  std::uint64_t next_ping_{};
};

}  // namespace libchiaking::net::kakusan
