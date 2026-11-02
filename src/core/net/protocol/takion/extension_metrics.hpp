/// @file
/// @brief Declares ExtensionReceiveMetrics, the one-way jitter and loss a
/// session measures from the version 20 extended header of the packets it
/// receives.
#pragma once

#include "src/core/utils/numeric/checked.hpp"

#include <cstdint>

namespace libchiaking::net::protocol::takion {

/// @brief Length of a loss measurement window, in microseconds (1 s).
inline constexpr std::uint64_t extension_loss_interval_us = 1'000'000;

/// @brief Jitter and loss of the console's packets, from their extended
/// headers.
///
/// Jitter follows RFC 3550: for consecutive packets the transit delta is the
/// change in (local arrival - remote timestamp), both in microseconds modulo
/// 2^32, and the jitter estimate moves 1/16 of the way to its magnitude.
/// Loss compares the packets received with the span of extended-header
/// counters seen, per one-second window. Owned by the client's inbound path
/// and used under the client's lock.
class ExtensionReceiveMetrics {
 public:
  /// @brief Computes the transit delta between two packets.
  ///
  /// @param local_us This packet's local arrival, low 32 bits of microseconds.
  /// @param remote_us This packet's remote timestamp, in microseconds.
  /// @param previous_local_us The previous packet's local arrival.
  /// @param previous_remote_us The previous packet's remote timestamp.
  /// @return The change in transit time, in microseconds; wraps cleanly.
  static constexpr std::int32_t transit_delta(
      std::uint32_t local_us, std::uint32_t remote_us,
      std::uint32_t previous_local_us,
      std::uint32_t previous_remote_us) noexcept {
    return static_cast<std::int32_t>(local_us + previous_remote_us - remote_us -
                                     previous_local_us);
  }

  /// @brief Takes one received packet's extended header.
  ///
  /// @param remote_us Its timestamp field.
  /// @param counter Its counter field.
  /// @param now_us When it arrived, in monotonic microseconds.
  void observe(std::uint32_t remote_us, std::uint32_t counter,
               std::uint64_t now_us) noexcept;
  /// @brief Counts a packet whose arrival time is meaningless, such as one
  /// replayed after crypto was installed; it breaks the jitter chain.
  ///
  /// @param counter Its counter field.
  /// @param now_us The current time, in monotonic microseconds.
  void observe_untimed(std::uint32_t counter, std::uint64_t now_us) noexcept;

  /// @brief Reads the latest transit delta.
  ///
  /// @return Microseconds.
  std::int32_t last_transit_delta_us() const noexcept { return last_delta_us_; }
  /// @brief Reads the jitter estimate.
  ///
  /// @return Microseconds.
  std::uint64_t jitter_us() const noexcept { return jitter_scaled_ >> 4U; }
  /// @brief Reads the packets counted.
  ///
  /// @return The count.
  std::uint64_t received() const noexcept { return received_; }
  /// @brief Reads the packets the counters say were sent.
  ///
  /// @return Highest counter - first counter + 1; 0 before the first.
  std::uint64_t expected() const noexcept {
    return received_ == 0 ? 0 : highest_ - base_ + 1U;
  }
  /// @brief Reads the packets missing.
  ///
  /// @return expected() - received(), not below 0.
  std::uint64_t lost() const noexcept {
    return numeric::saturating_sub(expected(), received_);
  }
  /// @brief Reads the loss of the last complete window.
  ///
  /// @return A fraction from 0 to 1.
  double loss_ratio() const noexcept { return loss_ratio_; }

 private:
  /// @brief Counts a packet's counter, closing a loss window after 1 s.
  ///
  /// @param counter The counter field.
  /// @param now_us The current time, in monotonic microseconds.
  void count(std::uint32_t counter, std::uint64_t now_us) noexcept;

  /// @brief Previous packet's local arrival, low 32 bits of microseconds.
  std::uint32_t previous_local_us_{};
  /// @brief Previous packet's remote timestamp.
  std::uint32_t previous_remote_us_{};
  /// @brief Whether the previous packet was timed, so a delta may be taken.
  bool timed_{};
  /// @brief Latest transit delta, in microseconds.
  std::int32_t last_delta_us_{};
  /// @brief Jitter estimate times 16, in microseconds.
  std::uint64_t jitter_scaled_{};
  /// @brief First counter seen.
  std::uint64_t base_{};
  /// @brief Highest counter seen, unwrapped to 64 bits.
  std::uint64_t highest_{};
  /// @brief Packets counted.
  std::uint64_t received_{};
  /// @brief When the current loss window began.
  std::uint64_t interval_start_us_{};
  /// @brief Expected count at the start of the current window.
  std::uint64_t interval_expected_{};
  /// @brief Received count at the start of the current window.
  std::uint64_t interval_received_{};
  /// @brief Loss of the last complete window.
  double loss_ratio_{};
};

}  // namespace libchiaking::net::protocol::takion
