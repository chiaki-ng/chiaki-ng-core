/// @file
/// @brief Declares usecases::ReadCategories, the use case that reads and holds
/// the PS5 Premium and for-sale categories for a user's audience.
#pragma once

#include "src/core/catalog/adapters/cache/held_answers.hpp"
#include "src/core/catalog/usecases/fetch_progress.hpp"
#include "src/core/net/transport/http/client.hpp"

#include <libchiaking/detail/users/ps_user.hpp>
#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>

#include <memory>
#include <string_view>

namespace libchiaking::catalog::usecases {

/// @brief Reads and holds the PS5 categories.
///
/// Borrows the HTTP provider, clocks, log and held answers, which must outlive
/// it. Safe to call from several threads at once: all shared state lives in
/// HeldAnswers. Not copyable.
class ReadCategories {
 public:
  /// @brief The token that abandons a call.
  using Stop = platform::StopToken;
  /// @brief A held Premium category.
  using CategoryPtr = HeldAnswers::CategoryPtr;
  /// @brief A held for-sale category.
  using ForSalePtr = HeldAnswers::ForSalePtr;
  /// @brief What a caller wants from a held category.
  using Demand = cache::category::Demand;

  /// @brief Sets up the use case.
  ///
  /// @param http Sends every request. Must outlive this object.
  /// @param clock The monotonic clock request deadlines are measured on. Must
  ///     outlive this object.
  /// @param wall_clock Stamps answers. Must outlive this object.
  /// @param log Receives log lines; may be null.
  /// @param held Where answers are held. Must outlive this object.
  ReadCategories(platform::HttpProvider& http,
                 const platform::ClockProvider& clock,
                 const platform::ClockProvider& wall_clock,
                 platform::DiagnosticsProvider* log,
                 HeldAnswers& held) noexcept;
  /// @brief Not copyable: it borrows shared state.
  ReadCategories(const ReadCategories&) = delete;
  /// @brief Not copyable: it borrows shared state.
  auto operator=(const ReadCategories&) -> ReadCategories& = delete;

  /// @brief The Premium category for the user's audience.
  ///
  /// @param user The user.
  /// @param demand Whether the user asked to start over.
  /// @param stop Abandons the call.
  /// @param progress Told the scan's activity and pages.
  /// @return The category (null when the audience is unknown or nothing is
  ///     held after a failure); the fetch's failure when nothing is held; a
  ///     cancellation.
  [[nodiscard]] auto included(const users::PsUser& user, Demand demand,
                              const Stop& stop, const FetchProgress& progress)
      -> Expected<CategoryPtr, Unanswered>;
  /// @brief The for-sale category for the user's audience.
  ///
  /// @param user The user.
  /// @param demand Whether the user asked to start over.
  /// @param stop Abandons the call.
  /// @param progress Told the scan's activity and pages.
  /// @return As included.
  [[nodiscard]] auto for_sale(const users::PsUser& user, Demand demand,
                              const Stop& stop, const FetchProgress& progress)
      -> Expected<ForSalePtr, Unanswered>;
  /// @brief The held Premium category for the user's audience.
  ///
  /// @param user The user.
  /// @return The category, or null when none is held or the audience is
  ///     unknown.
  auto included_held_for(const users::PsUser& user) -> CategoryPtr;
  /// @brief The held for-sale category for the user's audience.
  ///
  /// @param user The user.
  /// @return The category, or null when none is held or the audience is
  ///     unknown.
  auto for_sale_held_for(const users::PsUser& user) -> ForSalePtr;

 private:
  /// @brief Serves a held category or fetches it once for every concurrent
  /// asker, holding the result.
  ///
  /// A failed fetch is recorded on the record and the last good category, if
  /// any, is still served.
  ///
  /// @tparam Category cloud_stream::PremiumCategory or
  /// cloud_stream::ForSaleCategory.
  /// @tparam Held A callable returning the HeldCategory.
  /// @tparam Fetch A callable taking a client and a PageCount and returning
  ///     Expected<Category, Unanswered>.
  /// @tparam Keep A callable taking the new HeldCategory and returning its
  ///     category.
  /// @param flight The flight for this category kind.
  /// @param question The question naming the locale and age.
  /// @param demand Whether the user asked to start over.
  /// @param stop Abandons the call.
  /// @param held Reads the held category.
  /// @param fetch Scans the category.
  /// @param keep Holds the new record.
  /// @param progress Told the scan's activity and pages when this caller
  ///     scans.
  /// @return The category (possibly null); the fetch's failure when nothing is
  ///     held; a cancellation.
  template <class Category, class Held, class Fetch, class Keep>
  [[nodiscard]] auto held_or_fetched(
      concurrency::SharedFlight<std::shared_ptr<const Category>, Unanswered>&
          flight,
      const HeldAnswers::Question& question, Demand demand, const Stop& stop,
      const Held& held, const Fetch& fetch, const Keep& keep,
      const FetchProgress& progress)
      -> Expected<std::shared_ptr<const Category>, Unanswered>;
  /// @brief Logs that a category is not asked because the audience is unknown.
  ///
  /// @param category The category's name for the log.
  void warn_of_an_unknown_audience(std::string_view category) const;

  /// @brief Sends every request; not owned.
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

}  // namespace libchiaking::catalog::usecases
