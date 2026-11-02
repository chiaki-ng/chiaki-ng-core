/// @file
/// @brief Declares Version, the release of the library a build was made from.
#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace libchiaking {

/// @brief The release of the library a build was made from: the
/// MAJOR.MINOR.PATCH number in the source tree's VERSION.txt, the pre-release
/// label a pre-release was built with, and the source commit when the build
/// could tell it.
///
/// Versions compare by number alone, so pre-releases and builds from different
/// commits of the same number are equal.
class Version {
 public:
  /// @brief Returns the version this library was built as.
  ///
  /// @return The build's version.
  static auto current() noexcept -> Version;

  /// @brief Returns the major number, raised for changes that break callers.
  ///
  /// @return The major number.
  constexpr auto major_number() const noexcept -> std::uint32_t {
    return major_;
  }
  /// @brief Returns the minor number, raised for additions.
  ///
  /// @return The minor number.
  constexpr auto minor_number() const noexcept -> std::uint32_t {
    return minor_;
  }
  /// @brief Returns the patch number, raised for fixes.
  ///
  /// @return The patch number.
  constexpr auto patch_number() const noexcept -> std::uint32_t {
    return patch_;
  }
  /// @brief Returns the label of a pre-release, such as `rc1` in `0.1.0-rc1`.
  ///
  /// @return The label, or empty for a release.
  constexpr auto prerelease() const noexcept -> std::string_view {
    return prerelease_;
  }
  /// @brief Returns the abbreviated source commit the build was made from.
  ///
  /// @return The commit in lowercase hexadecimal, or empty when the build
  ///     could not tell it.
  constexpr auto commit() const noexcept -> std::string_view { return commit_; }
  /// @brief Formats the version for people to read.
  ///
  /// @return `0.1.0`, with `-rc1` for a pre-release and `+2b32454c9139` when
  ///     the commit is known.
  auto text() const -> std::string;

  /// @brief Compares the numbers of two versions, ignoring their commits.
  ///
  /// @return True when the major, minor and patch numbers match.
  friend constexpr auto operator==(const Version& left,
                                   const Version& right) noexcept -> bool {
    return left.numbers() == right.numbers();
  }
  /// @brief Orders two versions by major, then minor, then patch number.
  ///
  /// @return The order of the two versions' numbers.
  friend constexpr auto operator<=>(const Version& left,
                                    const Version& right) noexcept
      -> std::strong_ordering {
    return left.numbers() <=> right.numbers();
  }

 private:
  struct Numbers {
    std::uint32_t major;
    std::uint32_t minor;
    std::uint32_t patch;
    friend constexpr auto operator<=>(const Numbers&,
                                      const Numbers&) noexcept = default;
  };

  constexpr Version(std::uint32_t major_part, std::uint32_t minor_part,
                    std::uint32_t patch_part, std::string_view prerelease_label,
                    std::string_view source_commit) noexcept
      : major_{major_part},
        minor_{minor_part},
        patch_{patch_part},
        prerelease_{prerelease_label},
        commit_{source_commit} {}

  constexpr auto numbers() const noexcept -> Numbers {
    return {major_, minor_, patch_};
  }

  std::uint32_t major_;
  std::uint32_t minor_;
  std::uint32_t patch_;
  std::string_view prerelease_;
  std::string_view commit_;
};

}  // namespace libchiaking
