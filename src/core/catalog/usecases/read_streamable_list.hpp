/// @file
/// @brief Declares usecases::ReadStreamableList, the use case that walks and
/// holds the LegacyCloudStream streamable list for a user's region.
#pragma once

#include "src/core/catalog/adapters/cache/held_answers.hpp"
#include "src/core/catalog/model/list/raw_listing.hpp"
#include "src/core/catalog/usecases/fetch_progress.hpp"
#include "src/core/net/transport/http/client.hpp"

#include <libchiaking/detail/users/ps_user.hpp>
#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>

#include <optional>
#include <vector>

namespace libchiaking::catalog::usecases {

/// @brief Reads and holds a user's streamable list.
///
/// Borrows the HTTP provider, clocks, log and held answers, which must outlive
/// it. Safe to call from several threads at once: all shared state lives in
/// HeldAnswers. Not copyable.
class ReadStreamableList {
 public:
  /// @brief The token that abandons a call.
  using Stop = platform::StopToken;
  /// @brief A held streamable list.
  using ListPtr = HeldAnswers::ListPtr;

  /// @brief Sets up the use case.
  ///
  /// @param http Sends every request. Must outlive this object.
  /// @param clock The monotonic clock request deadlines are measured on. Must
  ///     outlive this object.
  /// @param wall_clock Stamps answers. Must outlive this object.
  /// @param log Receives log lines; may be null.
  /// @param held Where answers are held. Must outlive this object.
  ReadStreamableList(platform::HttpProvider& http,
                     const platform::ClockProvider& clock,
                     const platform::ClockProvider& wall_clock,
                     platform::DiagnosticsProvider* log,
                     HeldAnswers& held) noexcept;
  /// @brief Not copyable: it borrows shared state.
  ReadStreamableList(const ReadStreamableList&) = delete;
  /// @brief Not copyable: it borrows shared state.
  auto operator=(const ReadStreamableList&) -> ReadStreamableList& = delete;

  /// @brief The LegacyCloudStream streamable list for the user's region.
  ///
  /// A usable held list is served. Otherwise the account's LegacyCloudStream
  /// shelves are walked (falling back to the public store), the copy check and
  /// the PS5 store-app lists are added, and the result is filtered, held and
  /// returned. Callers for the same account, and then the same region, share
  /// one walk.
  ///
  /// @param user The user.
  /// @param stop Abandons the call.
  /// @param progress Told the activities and pages of a walk this caller
  ///     makes.
  /// @return The list; the sign-in failure; a cancellation;
  ///     Unanswered::Reason::unavailable when nothing was listed;
  ///     Unanswered::Reason::malformed when the rows cannot form a list.
  [[nodiscard]] auto list(users::PsUser& user, const Stop& stop,
                          const FetchProgress& progress = {})
      -> Expected<ListPtr, Unanswered>;

 private:
  /// @brief Builds the list from the account's own public store, read
  /// without signing in.
  ///
  /// The region the account's token states is learned first when none is
  /// known. The LegacyCloudStream shelves are walked through the account's
  /// session only when the region is still unknown or the anonymous walk found
  /// no titles, and only then is the access they show noted.
  ///
  /// @param user The user.
  /// @param stop Abandons the call.
  /// @param progress Told the walk's activities and pages.
  /// @return As list.
  [[nodiscard]] auto walked_list(users::PsUser& user, const Stop& stop,
                                 const FetchProgress& progress)
      -> Expected<ListPtr, Unanswered>;
  /// @brief Builds and holds the list from a shelf walk.
  ///
  /// The list keeps the walk's origin. When the walk failed the public store is
  /// walked instead, and that list is
  /// complete only when the region does not support LegacyCloudStream. The PS4
  /// copy check and the PS5 store-app lists are added.
  ///
  /// @param user The user.
  /// @param client Sends the requests.
  /// @param walked The shelf walk.
  /// @param progress Told the activities and pages.
  /// @return The list; the walk's sign-in failure; a cancellation;
  ///     Unanswered::Reason::unavailable (or the walk's failure) when nothing
  ///     was listed; Unanswered::Reason::malformed when the rows collide.
  [[nodiscard]] auto listed(users::PsUser& user, net::http::Client& client,
                            Expected<RawListing, Unanswered> walked,
                            const FetchProgress& progress)
      -> Expected<ListPtr, Unanswered>;
  /// @brief Marks PS4 rows whose streaming copy no plan covers as not
  /// streaming.
  ///
  /// @param user The user, for the audience.
  /// @param client Sends the requests.
  /// @param entries The rows; changed in place.
  /// @param pages Counts the batches asked.
  /// @return How far the check got, or std::nullopt when the audience is
  ///     unknown and nothing was asked.
  static auto mark_copies_no_plan_covers(const users::PsUser& user,
                                         net::http::Client& client,
                                         std::vector<ListedEntry>& entries,
                                         PageCount pages)
      -> std::optional<StreamableList::CopiesChecked>;

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
