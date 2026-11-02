/// @file
/// @brief Declares Grade, the four kinds of trophy, TrophyCounts, a count of
/// trophies of each grade, and Time, the wall-clock time trophy answers are
/// stamped with.
#pragma once

#include <chrono>
#include <cstdint>

namespace libchiaking::trophies {

/// @brief Wall-clock time in milliseconds since the Unix epoch.
using Time = std::chrono::sys_time<std::chrono::milliseconds>;

/// @brief The grade of a trophy, from the most common to the one awarded for
/// earning every other trophy in a set.
enum class Grade : std::uint8_t {
  /// @brief A bronze trophy.
  bronze,
  /// @brief A silver trophy.
  silver,
  /// @brief A gold trophy.
  gold,
  /// @brief The platinum trophy.
  platinum,
};

/// @brief How many trophies there are of each grade. Plain data.
struct TrophyCounts {
  /// @brief Bronze trophies.
  std::uint32_t bronze{};
  /// @brief Silver trophies.
  std::uint32_t silver{};
  /// @brief Gold trophies.
  std::uint32_t gold{};
  /// @brief Platinum trophies.
  std::uint32_t platinum{};

  /// @brief The count of one grade.
  ///
  /// @param grade The grade.
  /// @return The trophies of that grade.
  constexpr auto of(Grade grade) const noexcept -> std::uint32_t {
    switch (grade) {
      case Grade::bronze:
        return bronze;
      case Grade::silver:
        return silver;
      case Grade::gold:
        return gold;
      case Grade::platinum:
        return platinum;
    }
    return 0;
  }

  /// @brief Every trophy counted.
  ///
  /// @return The sum of the four grades.
  constexpr auto total() const noexcept -> std::uint64_t {
    return std::uint64_t{bronze} + silver + gold + platinum;
  }

  /// @brief Compares every grade.
  friend auto operator==(const TrophyCounts&, const TrophyCounts&)
      -> bool = default;
};

}  // namespace libchiaking::trophies
