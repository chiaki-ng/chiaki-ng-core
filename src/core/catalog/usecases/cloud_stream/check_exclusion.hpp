/// @file
/// @brief Declares usecases::cloud_stream::CheckExclusion, which judges a PS5
/// launch against the exclusion list and keeps that list current.
///
/// A held snapshot younger than 24 hours is used as it is; an older one is
/// refreshed, and when the refresh fails (or there never was a snapshot)
/// every PS5 launch is refused.
#pragma once

#include "src/core/catalog/adapters/cache/held_answers.hpp"
#include "src/core/catalog/model/cloud_stream/exclusions.hpp"

#include <libchiaking/detail/catalog/product_id.hpp>
#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>

#include <optional>

namespace libchiaking::catalog::usecases::cloud_stream {

/// @brief Judges launches against the current exclusion list.
///
/// Borrows the provider, clock, log and held answers, which must outlive it.
/// Concurrent refreshes share one request.
class CheckExclusion {
 public:
  /// @brief The token that abandons a call.
  using Stop = platform::StopToken;
  /// @brief The verdict on one launch.
  using Verdict = catalog::cloud_stream::ExclusionVerdict;

  /// @brief Sets up the check.
  ///
  /// @param http Sends the refresh. Must outlive this object.
  /// @param clock The monotonic clock request deadlines are measured on. Must
  ///     outlive this object.
  /// @param wall_clock Stamps and ages snapshots. Must outlive this object.
  /// @param log Receives log lines; may be null.
  /// @param held Where the snapshot is held. Must outlive this object.
  CheckExclusion(platform::HttpProvider& http,
                 const platform::ClockProvider& clock,
                 const platform::ClockProvider& wall_clock,
                 platform::DiagnosticsProvider* log,
                 HeldAnswers& held) noexcept;

  /// @brief Judges a launch, refreshing the list when it is not fresh.
  ///
  /// May block on the exclusion list request.
  ///
  /// @param title The launch's title id, when known.
  /// @param product The launch's product id.
  /// @param stop Abandons the refresh.
  /// @return The verdict; unavailable with the refresh's failure when no fresh
  ///     list could be had.
  auto of(const std::optional<TitleId>& title, const ProductId& product,
          const Stop& stop) -> Verdict;
  /// @brief Judges a launch against the held list only; never blocks on the
  /// network.
  ///
  /// @param title The launch's title id, when known.
  /// @param product The launch's product id.
  /// @return The verdict; unavailable when no fresh list is held.
  auto held(const std::optional<TitleId>& title, const ProductId& product)
      -> Verdict;
  /// @brief Refreshes the list when it is not fresh.
  ///
  /// @param stop Abandons the refresh.
  /// @return Nothing when a fresh list is held; the refresh's failure.
  [[nodiscard]] auto keep_current(const Stop& stop)
      -> Expected<void, Unanswered>;

 private:
  /// @brief The held list when fresh, else a refreshed one, fetched once for
  /// every concurrent asker.
  ///
  /// @param stop Abandons the refresh.
  /// @return The list; the refresh's failure.
  [[nodiscard]] auto current_list(const Stop& stop)
      -> Expected<catalog::cloud_stream::ExclusionList, Unanswered>;
  /// @brief Fetches the list and holds it when it was read.
  ///
  /// @param stop Abandons the request.
  /// @return The list; the request's failure.
  [[nodiscard]] auto refreshed_list(const Stop& stop)
      -> Expected<catalog::cloud_stream::ExclusionList, Unanswered>;

  /// @brief Sends requests; not owned.
  platform::HttpProvider& http_;
  /// @brief The monotonic clock request deadlines are measured on.
  const platform::ClockProvider& clock_;
  /// @brief The wall clock; not owned.
  const platform::ClockProvider& wall_;
  /// @brief Receives log lines; may be null.
  platform::DiagnosticsProvider* log_;
  /// @brief The held answers; not owned.
  HeldAnswers& held_;
};

}  // namespace libchiaking::catalog::usecases::cloud_stream
