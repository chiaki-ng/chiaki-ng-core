/// @file
/// @brief Declares the error reported by the runtime ports.
#pragma once

#include <cstdint>

namespace libchiaking::platform {

/// @brief Reason a threading, clock or random operation failed.
enum class PlatformError : std::uint8_t {
  /// @brief Memory, or a system resource counted like memory, was exhausted.
  out_of_memory,
  /// @brief A thread could not be created, joined or queried.
  ///
  /// The stock threading provider also reports this for a join attempted from
  /// the thread itself or while another join is in progress, and for
  /// native::Thread::result read before the entry has returned.
  thread_failure,
  /// @brief A mutex and a condition variable from different providers were
  /// used together.
  wrong_provider,
  /// @brief The operating system source behind the call failed, for example
  /// the entropy source.
  io_failure,
  /// @brief Any other operating system failure.
  system_failure,
};

}  // namespace libchiaking::platform
