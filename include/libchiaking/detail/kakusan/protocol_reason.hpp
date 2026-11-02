/// @file
/// @brief Declares the reasons the Kakusan protocol gives when one side ends a
/// guest's visit.
#pragma once

#include <cstdint>

namespace libchiaking::kakusan {

/// @brief Why a visit is being ended, as sent between host and guest.
///
/// Frontends meet these as libchiaking::kakusan::EndReason. The note on each
/// value names the EndReason kind it becomes.
enum class ProtocolReason : std::uint8_t {
  /// @brief A person chose to end it: the guest left or the host removed
  /// them. Becomes EndReason::Kind::left.
  user,
  /// @brief The guest's seat was withdrawn because the host reduced the
  /// seats. Becomes EndReason::Kind::no_seat.
  capacity,
  /// @brief The shared session or the guest's place on the console ended.
  /// Becomes EndReason::Kind::session_ended.
  source,
  /// @brief The other side stopped responding in time. Becomes
  /// EndReason::Kind::timed_out.
  timeout,
  /// @brief The connection between the devices failed. Becomes
  /// EndReason::Kind::connection_lost.
  network,
  /// @brief The protocol's rules were broken. Becomes
  /// EndReason::Kind::protocol.
  protocol,
};

}  // namespace libchiaking::kakusan
