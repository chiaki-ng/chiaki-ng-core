/// @file
/// @brief Declares the console generations and streaming services a title can
/// belong to.
#pragma once

#include <cstdint>

namespace libchiaking::catalog {

/// @brief The console generation a title was made for.
enum class Platform : std::uint8_t {
  /// @brief Made for PlayStation 3.
  ps3,
  /// @brief Made for PlayStation 4.
  ps4,
  /// @brief Made for PlayStation 5.
  ps5,
};

/// @brief Which of PS's two cloud streaming services carries a title.
///
/// The two are listed, cleared and launched differently.
enum class ServiceGeneration : std::uint8_t {
  /// @brief The older LegacyCloudStream service.
  old_service,
  /// @brief The streaming offered through the PS5 store, which carries PS5
  /// titles and some PS4 titles.
  ps5_cloud,
};

}  // namespace libchiaking::catalog
