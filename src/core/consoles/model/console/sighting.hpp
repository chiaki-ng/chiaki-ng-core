/// @file
/// @brief Declares Sighting, what one search reply showed about a console.
#pragma once

#include "libchiaking/detail/consoles/console.hpp"
#include "libchiaking/detail/consoles/console_name.hpp"
#include "libchiaking/detail/consoles/firmware.hpp"
#include "libchiaking/detail/consoles/hardware_id.hpp"
#include "libchiaking/detail/consoles/host.hpp"

#include <optional>

namespace libchiaking::consoles {

/// @brief What one search reply showed about a console.
struct Sighting {
  /// @brief The console's hardware id, which identifies it in the roster.
  HardwareId hardware_id;
  /// @brief The firmware the console reported, which fixes its
  /// generation.
  Firmware firmware;
  /// @brief The name the console stated, with
  /// ConsoleName::Source::discovered; empty when it stated none usable.
  std::optional<ConsoleName> name;
  /// @brief The address the reply came from.
  Host address;
  /// @brief Whether the console answered that it is in rest mode rather
  /// than switched on.
  bool in_rest_mode{};
  /// @brief The title the console said it is running; empty when none is.
  std::optional<RunningTitle> running_title{};
};

}  // namespace libchiaking::consoles
