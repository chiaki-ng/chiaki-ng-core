/// @file
/// @brief Declares PageCount, which a paging source tells of each page it
/// reads and of each total it learns, and which reports the running count.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>

namespace libchiaking::catalog {

/// @brief Counts the pages one activity reads and the pages it expects.
///
/// A source that reads in pages calls expect() when it learns how many pages
/// one of its lists has, and read() as each page lands. Every change is passed
/// on to the report the count was made with. A count made without one counts
/// and tells nobody. Not thread-safe; a source uses its count on the thread
/// that runs it.
class PageCount {
 public:
  /// @brief Told the pages read so far and the pages expected, when known.
  using Report =
      std::function<void(std::uint32_t read, std::optional<std::uint32_t>)>;

  /// @brief Makes a count that tells nobody.
  PageCount() noexcept = default;
  /// @brief Makes a count that passes every change on.
  ///
  /// @param report Told every change; may be empty.
  explicit PageCount(Report report) noexcept;

  /// @brief Adds pages to the total the activity expects.
  ///
  /// @param pages The pages one more list holds.
  void expect(std::uint32_t pages);
  /// @brief Counts pages that landed.
  ///
  /// @param pages How many landed; 1 by default.
  void read(std::uint32_t pages = 1);
  /// @brief Pages read so far.
  ///
  /// @return The count.
  auto pages_read() const noexcept -> std::uint32_t { return read_; }
  /// @brief Pages expected in all.
  ///
  /// @return The total, or nothing while no list has stated one.
  auto pages_expected() const noexcept -> std::optional<std::uint32_t> {
    return expected_;
  }

 private:
  /// @brief Passes the count on.
  void tell() const;

  /// @brief Told every change.
  Report report_{};
  /// @brief Pages read.
  std::uint32_t read_{};
  /// @brief Pages expected.
  std::optional<std::uint32_t> expected_{};
};

}  // namespace libchiaking::catalog
