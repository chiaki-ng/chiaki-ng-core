/// @file
/// @brief Declares usecases::LaunchChecks, the checks LegacyCloudStream and PS5
/// cloud streaming launches share: loading the games, rereading membership and
/// cloud streaming access, judging releases against trusted time, and
/// building the cleared launch.
#pragma once

#include "libchiaking/detail/catalog/catalog.hpp"
#include "src/core/catalog/usecases/check_progress.hpp"
#include "src/core/catalog/usecases/list_games.hpp"
#include "src/core/users/cloud_streaming.hpp"

#include <optional>

namespace libchiaking::catalog::usecases {

/// @brief The shared launch checks.
///
/// Borrows the provider, trusted clock, log and ListGames, which must outlive
/// it. Safe to call from several threads. Not copyable.
class LaunchChecks {
 public:
  /// @brief The token that abandons a call.
  using Stop = platform::StopToken;
  /// @brief A MyGames build with what it was built from.
  using View = ListGames::View;

  /// @brief Sets up the checks.
  ///
  /// @param http Sends the requests. Must outlive this object.
  /// @param clock The monotonic clock request deadlines are measured on. Must
  ///     outlive this object.
  /// @param trusted_clock Judges release dates; may be null.
  /// @param log Receives log lines; may be null.
  /// @param list Lists the user's games. Must outlive this object.
  LaunchChecks(platform::HttpProvider& http,
               const platform::ClockProvider& clock,
               TrustedClock* trusted_clock, platform::DiagnosticsProvider* log,
               ListGames& list) noexcept;
  /// @brief Not copyable: it borrows shared state.
  LaunchChecks(const LaunchChecks&) = delete;
  /// @brief Not copyable: it borrows shared state.
  auto operator=(const LaunchChecks&) -> LaunchChecks& = delete;

  /// @brief The held games, or the games loaded afresh when none are held.
  ///
  /// @param user The user.
  /// @param stop Abandons the load.
  /// @param progress Told when the library starts loading.
  /// @return The view; the load's failure.
  [[nodiscard]] auto held_or_loaded(users::PsUser& user, const Stop& stop,
                                    CheckProgress progress)
      -> Expected<View, Unanswered>;
  /// @brief The user's PS Plus tier, reread unless it already reaches Premium.
  ///
  /// @param user The user.
  /// @param stop Abandons the request.
  /// @return The tier read; the held tier when the read gave no tier or failed
  ///     without being cancelled; otherwise the failure, or
  ///     Unanswered::Reason::malformed when the read gave no tier and none was
  ///     held.
  [[nodiscard]] auto tier_at_launch(users::PsUser& user, const Stop& stop)
      -> Expected<users::Tier, Unanswered>;
  /// @brief Rereads the user's cloud streaming access.
  ///
  /// Blocks on the benefits request and, when access is refused, the
  /// subscription request that words the refusal.
  ///
  /// @param user The user.
  /// @param stop Abandons the requests.
  /// @return The access; unknown with its cause when it could not be read.
  [[nodiscard]] auto cloud_streaming_at_launch(users::PsUser& user,
                                               const Stop& stop)
      -> users::CloudStreaming;
  /// @brief The trusted time held now, without syncing.
  ///
  /// @return The time, or std::nullopt when there is no trusted clock or no
  ///     usable sync.
  auto held_now() const noexcept -> std::optional<TrustedClock::Time>;
  /// @brief Refuses a game not yet released, or one PS refused recently.
  ///
  /// @param game The game; its own release date is judged.
  /// @param stop Abandons a trusted time sync.
  /// @return As the three-argument overload.
  auto release_refusal(const MyGame& game, const Stop& stop)
      -> std::optional<LaunchRefusal>;
  /// @brief Refuses a release not yet reached, or a game PS refused recently.
  ///
  /// Syncs trusted time first when the release has a date; may block on NTP.
  ///
  /// @param game The game, for its remembered refusal.
  /// @param release The release date to judge.
  /// @param stop Abandons the sync.
  /// @return The release refusal (unannounced, not released, no trusted
  ///     clock), else LaunchRefusal::refused_by_service for a standing refusal,
  ///     else std::nullopt.
  auto release_refusal(const MyGame& game, ReleaseDate release,
                       const Stop& stop) -> std::optional<LaunchRefusal>;
  /// @brief As release_refusal, with the held trusted time only; never blocks.
  ///
  /// @param game The game.
  /// @param release The release date to judge.
  /// @return The refusal, or std::nullopt.
  auto held_release_refusal(const MyGame& game, ReleaseDate release) const
      -> std::optional<LaunchRefusal>;

  /// @brief The refusal cloud streaming access gives.
  ///
  /// @param user The user, for the tier in the refusal.
  /// @param streaming The access.
  /// @return std::nullopt when streaming is allowed; check unavailable with the
  ///     cause when access is unknown; LaunchRefusal::needs_premium otherwise.
  static auto refused_streaming(const users::PsUser& user,
                                const users::CloudStreaming& streaming)
      -> std::optional<LaunchVerdict>;
  /// @brief As refused_streaming, from the access held on the user.
  ///
  /// @param user The user.
  /// @return The refusal; check unavailable when no access is held.
  static auto held_streaming_refusal(users::PsUser& user)
      -> std::optional<LaunchVerdict>;

  /// @brief The verdict for a check that could not run.
  ///
  /// @param cause Why.
  /// @return Refused with LaunchRefusal::check_unavailable.
  static auto unavailable(Unanswered cause) -> LaunchVerdict;
  /// @brief The refusal the listing gives, ignoring release dates.
  ///
  /// @param game The game; null refuses as not listed.
  /// @return The refusal, or std::nullopt when the listing allows a launch or
  ///     only a release date holds it back.
  static auto standing_refusal(const MyGame* game)
      -> std::optional<LaunchRefusal>;
  /// @brief The cleared launch for a game.
  ///
  /// @param game The game.
  /// @param seen The games, for the store locale and origin.
  /// @param user The user, for the region when nothing else names a locale.
  /// @return The product, service, platform, launch identifier, owned
  ///     entitlement, store locale (the list's, else the category's, else the
  ///     region, else English US) and list origin.
  static auto clearance(const MyGame& game, const View& seen,
                        const users::PsUser& user) -> ClearedLaunch;

 private:
  /// @brief Whether a refusal only waits on a release date.
  ///
  /// @param kind The refusal kind.
  /// @return True for unannounced, not released and no trusted clock.
  static auto waits_for_release(LaunchRefusal::Kind kind) noexcept -> bool;
  /// @brief The release or remembered refusal at a trusted time.
  ///
  /// @param game The game.
  /// @param release The release date.
  /// @param trusted_now The trusted time, if any.
  /// @return The refusal, or std::nullopt.
  static auto refusal_at(const MyGame& game, ReleaseDate release,
                         std::optional<TrustedClock::Time> trusted_now)
      -> std::optional<LaunchRefusal>;

  /// @brief Sends requests; not owned.
  platform::HttpProvider& http_;
  /// @brief The monotonic clock request deadlines are measured on.
  const platform::ClockProvider& clock_;
  /// @brief Judges releases; may be null.
  TrustedClock* trusted_;
  /// @brief Receives log lines; may be null.
  platform::DiagnosticsProvider* log_;
  /// @brief Lists the user's games; not owned.
  ListGames& list_;
};

}  // namespace libchiaking::catalog::usecases
