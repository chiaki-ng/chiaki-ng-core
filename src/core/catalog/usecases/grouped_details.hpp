/// @file
/// @brief Declares GroupedDetails, which answers a game's details asked for
/// by the concept it is grouped under.
#pragma once

#include "libchiaking/detail/catalog/catalog.hpp"
#include "src/core/catalog/adapters/cache/held_answers.hpp"
#include "src/core/catalog/model/unanswered.hpp"

namespace libchiaking::catalog::usecases {

/// @brief Answers a game's details by concept from the public store.
///
/// Needs no cloud streaming: it reads only the anonymous store, shares one
/// request between callers asking the same question, and keeps whole answers
/// in the held answers.
class GroupedDetails {
 public:
  /// @brief Ends a read early when requested.
  using Stop = platform::StopToken;

  /// @brief Builds the use case over its providers.
  ///
  /// @param http Sends store requests; not owned.
  /// @param threading Starts the threads concept reads run on; not owned.
  /// @param clock The monotonic clock request deadlines are measured on; not
  /// owned.
  /// @param wall_clock The wall clock; not owned.
  /// @param log Receives log lines; may be null.
  /// @param held The held answers; not owned.
  GroupedDetails(platform::HttpProvider& http,
                 platform::ThreadingProvider& threading,
                 const platform::ClockProvider& clock,
                 const platform::ClockProvider& wall_clock,
                 platform::DiagnosticsProvider* log, HeldAnswers& held);
  GroupedDetails(const GroupedDetails&) = delete;
  auto operator=(const GroupedDetails&) -> GroupedDetails& = delete;

  /// @brief Answers the details of a concept in the user's store region.
  ///
  /// Blocks the caller while the store answers.
  ///
  /// @param user The user whose region picks the store locale.
  /// @param grouped_under The concept.
  /// @param stop Ends the read early.
  /// @return The details, or why the store left them unanswered.
  [[nodiscard]] auto details(const users::PsUser& user,
                             const ConceptId& grouped_under, const Stop& stop)
      -> Expected<GameDetails, Unanswered>;
  /// @brief Answers the details of a concept in one store locale.
  ///
  /// Gives a held answer when one is still fresh; otherwise blocks the caller
  /// while the store answers.
  ///
  /// @param locale The store locale.
  /// @param grouped_under The concept.
  /// @param stop Ends the read early.
  /// @return The details, or why the store left them unanswered.
  [[nodiscard]] auto details(const StoreLocale& locale,
                             const ConceptId& grouped_under, const Stop& stop)
      -> Expected<GameDetails, Unanswered>;

 private:
  /// @brief Sends requests; not owned.
  platform::HttpProvider& http_;
  /// @brief Starts the threads concept reads run on; not owned.
  platform::ThreadingProvider& threading_;
  /// @brief The monotonic clock request deadlines are measured on; not
  /// owned.
  const platform::ClockProvider& clock_;
  /// @brief The wall clock; not owned.
  const platform::ClockProvider& wall_;
  /// @brief Receives log lines; may be null.
  platform::DiagnosticsProvider* log_;
  /// @brief The held answers; not owned.
  HeldAnswers& held_;
};

}  // namespace libchiaking::catalog::usecases
