/// @file
/// @brief Declares usecases::ClearForLaunch, the use case that decides whether
/// a listed game may launch now, and that answers the ownership and
/// storefront questions a launch screen asks.
///
/// It routes each game to its service's clearance:
/// legacy_cloud_stream::Clearance for the old service, cloud_stream::Clearance
/// for PS5 cloud streaming.
#pragma once

#include "libchiaking/detail/catalog/catalog.hpp"
#include "src/core/catalog/adapters/cache/held_answers.hpp"
#include "src/core/catalog/usecases/check_progress.hpp"
#include "src/core/catalog/usecases/cloud_stream/clearance.hpp"
#include "src/core/catalog/usecases/launch_checks.hpp"
#include "src/core/catalog/usecases/legacy_cloud_stream/clearance.hpp"
#include "src/core/catalog/usecases/list_games.hpp"

namespace libchiaking::catalog::usecases {

/// @brief Clears games for launch, by quick pre-check or by the full checks.
///
/// Borrows the provider, clocks, log, held answers and ListGames, which must
/// outlive it. Safe to call from several threads. Not copyable.
class ClearForLaunch {
 public:
  /// @brief The token that abandons a call.
  using Stop = platform::StopToken;
  /// @brief A MyGames build with what it was built from.
  using View = ListGames::View;

  /// @brief Sets up the use case.
  ///
  /// @param http Sends the requests. Must outlive this object.
  /// @param clock The monotonic clock request deadlines are measured on.
  ///     Must outlive this object.
  /// @param wall_clock Stamps answers. Must outlive this object.
  /// @param trusted_clock Judges release dates; may be null, in which case
  ///     every release-gated game is refused. Must outlive this object when
  ///     given.
  /// @param log Receives log lines; may be null.
  /// @param held The held answers. Must outlive this object.
  /// @param list Lists the user's games. Must outlive this object.
  ClearForLaunch(platform::HttpProvider& http,
                 const platform::ClockProvider& clock,
                 const platform::ClockProvider& wall_clock,
                 TrustedClock* trusted_clock,
                 platform::DiagnosticsProvider* log, HeldAnswers& held,
                 ListGames& list);
  /// @brief Not copyable: it owns the clearances.
  ClearForLaunch(const ClearForLaunch&) = delete;
  /// @brief Not copyable: it owns the clearances.
  auto operator=(const ClearForLaunch&) -> ClearForLaunch& = delete;

  /// @brief The quick pre-check, from held answers only; never asks PS.
  ///
  /// It gives the same answer as clear for every held fact; missing facts
  /// refuse.
  ///
  /// @param user The user.
  /// @param product The product the launch asks for.
  /// @return The verdict; refused as check unavailable when the user is signed
  ///     out or expired.
  auto can_launch(users::PsUser& user, const ProductId& product)
      -> LaunchVerdict;
  /// @brief The full launch check.
  ///
  /// Loads the games, and when the game is hidden because the tier does not
  /// include it, rereads the membership (LegacyCloudStream) or cloud streaming
  /// access (PS5) and loads again; then runs the game's service clearance.
  /// Blocks for every request the checks need.
  ///
  /// @param user The user.
  /// @param product The product the launch asks for.
  /// @param stop Abandons the checks.
  /// @param progress Told each check as it starts.
  /// @return The verdict; refused with the failure when a check could not run,
  ///     and not listed when the game is not among the user's games.
  auto clear(users::PsUser& user, const ProductId& product, const Stop& stop,
             CheckProgress progress = {}) -> LaunchVerdict;
  /// @brief Asks whether the user owns a listed game.
  ///
  /// @param user The user.
  /// @param product The product.
  /// @param stop Abandons the call.
  /// @return The answer; the games' failure; Unanswered::Reason::not_listed
  ///     when the game is not listed; the recheck's failure.
  [[nodiscard]] auto ownership(users::PsUser& user, const ProductId& product,
                               const Stop& stop)
      -> Expected<OwnershipAnswer, Unanswered>;
  /// @brief What the store offers the user for a game.
  ///
  /// A LegacyCloudStream game, or an unlisted product that is not PS5, has no
  /// storefront answer.
  ///
  /// @param user The user.
  /// @param product The product.
  /// @param stop Abandons the call.
  /// @return The answer; the sign-in failure; the LegacyCloudStream refusal;
  /// the store's
  ///     failure.
  [[nodiscard]] auto storefront(users::PsUser& user, const ProductId& product,
                                const Stop& stop)
      -> Expected<StorefrontAnswer, Unanswered>;

 private:
  /// @brief Whether a game streams on the old service.
  ///
  /// @param game The game.
  /// @return True for a LegacyCloudStream game.
  static auto streams_on_legacy_cloud_stream(const MyGame& game) noexcept
      -> bool;

  /// @brief Lists the user's games; not owned.
  ListGames& list_;
  /// @brief The checks both services share.
  LaunchChecks checks_;
  /// @brief The LegacyCloudStream clearance.
  legacy_cloud_stream::Clearance legacy_cloud_stream_;
  /// @brief The PS5 cloud streaming clearance.
  cloud_stream::Clearance cloud_stream_;
};

}  // namespace libchiaking::catalog::usecases
