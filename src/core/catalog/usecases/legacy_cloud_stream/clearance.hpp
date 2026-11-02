/// @file
/// @brief Declares usecases::legacy_cloud_stream::Clearance, the launch checks
/// of a LegacyCloudStream (old service) game: the listing's standing refusal, a
/// Premium tier, and the release date.
#pragma once

#include "src/core/catalog/usecases/launch_checks.hpp"

#include <cstdint>
#include <optional>

namespace libchiaking::catalog::usecases::legacy_cloud_stream {

/// @brief Clears LegacyCloudStream games for launch.
///
/// Borrows the shared checks, which must outlive it.
class Clearance {
 public:
  /// @brief The token that abandons a call.
  using Stop = LaunchChecks::Stop;
  /// @brief A MyGames build with what it was built from.
  using View = LaunchChecks::View;

  /// @brief Uses the shared checks.
  ///
  /// @param checks The shared checks. Must outlive this object.
  explicit Clearance(LaunchChecks& checks) noexcept;

  /// @brief Runs the LegacyCloudStream checks in order.
  ///
  /// May block on the membership request and on a trusted time sync.
  ///
  /// @param user The user.
  /// @param seen The games, for the cleared launch's store.
  /// @param game The game.
  /// @param tier The tier already reread, or std::nullopt to read it now.
  /// @param stop Abandons the checks.
  /// @param progress Told each check as it starts.
  /// @return Cleared with the launch; refused with the listing's refusal,
  ///     LaunchRefusal::needs_premium below Premium, a release refusal, or
  ///     check unavailable when the tier could not be read.
  auto clear(users::PsUser& user, const View& seen, const MyGame& game,
             std::optional<users::Tier> tier, const Stop& stop,
             CheckProgress progress = {}) -> LaunchVerdict;
  /// @brief Whether the user owns a LegacyCloudStream game, from the listing.
  ///
  /// @param game The game.
  /// @return Owned or not owned; Unanswered::Reason::ownership_unknown when the
  ///     library could not be read.
  [[nodiscard]] static auto ownership(const MyGame& game)
      -> Expected<OwnershipAnswer, Unanswered>;
  /// @brief LegacyCloudStream games have no storefront answer.
  ///
  /// @return Always Unanswered::Reason::rejected.
  [[nodiscard]] static auto storefront()
      -> Expected<StorefrontAnswer, Unanswered>;

 private:
  /// @brief The checks, in the order they run.
  enum class Stage : std::uint8_t {
    /// @brief The listing's refusal, ignoring release dates.
    standing,
    /// @brief The user's tier reaches Premium.
    tier,
    /// @brief The release date has passed and no PS refusal stands.
    release
  };

  /// @brief The shared checks; not owned.
  LaunchChecks& checks_;
};

}  // namespace libchiaking::catalog::usecases::legacy_cloud_stream
