/// @file
/// @brief Declares cloud_stream::ExclusionList, the list of titles excluded
/// from cloud streaming, and ExclusionVerdict, what it says about one launch.
///
/// The list is excludedTitleIds from the anonymous feature configuration. A
/// title id matches exactly when the launch has one; otherwise the product id
/// must contain the excluded text. A match refuses, and so does having no
/// usable snapshot.
#pragma once

#include "src/core/catalog/model/library/exact_id.hpp"
#include "src/core/catalog/model/unanswered.hpp"

#include <libchiaking/detail/catalog/launch_verdict.hpp>
#include <libchiaking/detail/catalog/product_id.hpp>
#include <libchiaking/detail/expected.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace libchiaking::catalog::cloud_stream {

/// @brief One snapshot of the excluded title ids.
///
/// Immutable; may be shared between threads.
class ExclusionList {
 public:
  /// @brief Wall-clock time in milliseconds since the Unix epoch.
  using Time = std::chrono::sys_time<std::chrono::milliseconds>;

  /// @brief How an exclusion matched.
  enum class Rule : std::uint8_t {
    /// @brief The launch's title id equals an excluded id.
    exact_title_id,
    /// @brief The launch has no title id and its product id contains an
    /// excluded id.
    product_id_contains,
  };

  /// @brief An exclusion that matched a launch.
  struct Match {
    /// @brief How it matched.
    Rule rule;
    /// @brief The excluded id that matched.
    TitleId excluded;

    /// @brief Compares both fields.
    friend auto operator==(const Match&, const Match&) -> bool = default;
  };

  /// @brief How long a snapshot is trusted without a refresh; an older one
  /// is used only while a refresh has not failed.
  static constexpr std::chrono::hours fresh_for{24};

  /// @brief Keeps a snapshot.
  ///
  /// @param excluded The excluded ids in the order the service returns
  ///     them; may be empty.
  /// @param fetched_at When the snapshot was read.
  ExclusionList(std::vector<TitleId> excluded, Time fetched_at) noexcept;

  /// @brief The excluded ids.
  ///
  /// @return The ids, valid while the list lives; possibly empty.
  auto excluded() const noexcept -> const std::vector<TitleId>& {
    return excluded_;
  }
  /// @brief When the snapshot was read.
  ///
  /// @return Wall-clock time in milliseconds.
  auto fetched_at() const noexcept -> Time { return fetched_at_; }
  /// @brief Whether the snapshot is younger than ExclusionList::fresh_for.
  ///
  /// @param now The current wall-clock time.
  /// @return True when now is not before fetched_at and less than 24 hours
  ///     after it; false for a snapshot from the future.
  auto is_fresh_at(Time now) const noexcept -> bool;
  /// @brief Matches a launch against the list.
  ///
  /// @param title The launch's title id; when present only an exact match
  ///     counts.
  /// @param product The launch's product id, searched for each excluded id
  ///     when there is no title id.
  /// @return The first matching exclusion in list order, or std::nullopt.
  auto match(const std::optional<TitleId>& title,
             const ProductId& product) const -> std::optional<Match>;

  /// @brief Compares the ids and the time.
  friend auto operator==(const ExclusionList&, const ExclusionList&)
      -> bool = default;

 private:
  /// @brief The excluded ids in the order the service returns them.
  std::vector<TitleId> excluded_;
  /// @brief When the snapshot was read; wall-clock milliseconds.
  Time fetched_at_;
};

/// @brief What the exclusion list says about one launch: excluded, clear, or
/// unknown because no usable list was held.
///
/// Immutable; may be shared between threads.
class ExclusionVerdict {
 public:
  /// @brief Wall-clock time in milliseconds.
  using Time = ExclusionList::Time;

  /// @brief The text shown when a title is excluded.
  static constexpr std::string_view excluded_notice =
      "PlayStation has excluded this title from cloud streaming";

  /// @brief Judges a launch against the held list.
  ///
  /// @param list The usable snapshot, or why there is none.
  /// @param title The launch's title id, when known.
  /// @param product The launch's product id.
  /// @return Excluded with the match, clear, or unavailable with the
  ///     list's failure; each carrying the snapshot time when there is one.
  [[nodiscard]] static auto judged(
      const Expected<ExclusionList, Unanswered>& list,
      const std::optional<TitleId>& title, const ProductId& product)
      -> ExclusionVerdict;

  /// @brief Whether the title is excluded.
  ///
  /// @return True when an exclusion matched.
  auto is_excluded() const noexcept -> bool;
  /// @brief Whether there was no usable list.
  ///
  /// @return True when the list could not be loaded.
  auto is_unavailable() const noexcept -> bool;
  /// @brief Whether the launch must be refused.
  ///
  /// @return True when excluded or unavailable; false only when clear.
  auto refuses_launch() const noexcept -> bool;
  /// @brief The exclusion that matched.
  ///
  /// @return The match, or std::nullopt unless excluded.
  auto match() const -> std::optional<ExclusionList::Match>;
  /// @brief Why there was no usable list.
  ///
  /// @return The failure, or std::nullopt unless unavailable.
  auto cause() const noexcept -> std::optional<Unanswered>;
  /// @brief When the snapshot judged against was read.
  ///
  /// @return The time, or std::nullopt when unavailable.
  auto snapshot_at() const noexcept -> std::optional<Time>;
  /// @brief The refusal the verdict gives.
  ///
  /// @return LaunchRefusal::not_streamable with excluded_notice when
  ///     excluded; LaunchRefusal::check_unavailable with the cancellation
  ///     when the list was cancelled, or with
  ///     Unanswered::Reason::restriction_list_unavailable (keeping the HTTP
  ///     status) for any other failure; std::nullopt when clear.
  auto refusal() const -> std::optional<LaunchRefusal>;

 private:
  /// @brief The title matched an exclusion.
  struct Excluded {
    /// @brief The exclusion that matched.
    ExclusionList::Match match;
    /// @brief When the snapshot was read.
    Time snapshot_at;
  };
  /// @brief The title matched nothing.
  struct NotExcluded {
    /// @brief When the snapshot was read.
    Time snapshot_at;
  };
  /// @brief No usable list was held.
  struct Unavailable {
    /// @brief Why.
    Unanswered cause;
  };

  /// @brief The three possible verdicts.
  using Outcome = std::variant<Excluded, NotExcluded, Unavailable>;

  /// @brief Keeps one verdict.
  ///
  /// @param outcome The verdict.
  explicit ExclusionVerdict(Outcome outcome) noexcept;

  /// @brief The verdict; set once at construction.
  Outcome outcome_;
};

}  // namespace libchiaking::catalog::cloud_stream
