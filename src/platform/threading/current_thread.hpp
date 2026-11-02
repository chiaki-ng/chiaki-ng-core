/// @file
/// @brief Declares CurrentThread, which names the calling thread for
/// debuggers and profilers, as the stock threading provider does for every
/// thread it starts.
#pragma once

#include <string_view>

namespace libchiaking::platform::threading {

/// @brief The calling thread, as far as naming it goes.
class CurrentThread {
 public:
  /// @brief Gives the calling thread a name, as far as the system allows.
  ///
  /// The name is cut to 15 bytes on Linux and other POSIX systems and to
  /// 63 on Apple platforms, which may split a UTF-8 character. On Windows
  /// it is set with `SetThreadDescription` where the system has it, and an
  /// empty name is ignored. Failures are ignored.
  ///
  /// @param name The name, in UTF-8.
  static void rename(std::string_view name) noexcept;
};

}  // namespace libchiaking::platform::threading
