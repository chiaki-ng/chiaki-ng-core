#include "src/core/catalog/usecases/legacy_cloud_stream/clearance.hpp"

#include <utility>

namespace libchiaking::catalog::usecases::legacy_cloud_stream {

Clearance::Clearance(LaunchChecks& checks) noexcept : checks_(checks) {}

auto Clearance::clear(users::PsUser& user, const View& seen, const MyGame& game,
                      std::optional<users::Tier> tier, const Stop& stop,
                      CheckProgress progress) -> LaunchVerdict {
  for (auto stage = Stage::standing;;) {
    switch (stage) {
      case Stage::standing:
        if (auto standing = LaunchChecks::standing_refusal(&game))
          return LaunchVerdict::refused(std::move(*standing));
        stage = Stage::tier;
        break;
      case Stage::tier:
        if (!tier) {
          progress.started(LaunchCheck::membership);
          const auto asked = checks_.tier_at_launch(user, stop);
          if (!asked) return LaunchChecks::unavailable(asked.error());
          tier = asked.value();
        }
        if (*tier < streaming_tier)
          return LaunchVerdict::refused(LaunchRefusal::needs_premium(tier));
        stage = Stage::release;
        break;
      case Stage::release:
        progress.started(LaunchCheck::release);
        if (auto unreleased = checks_.release_refusal(game, stop))
          return LaunchVerdict::refused(std::move(*unreleased));
        return LaunchVerdict::cleared(
            LaunchChecks::clearance(game, seen, user));
    }
  }
}

auto Clearance::ownership(const MyGame& game)
    -> Expected<OwnershipAnswer, Unanswered> {
  if (game.ownership_state() == OwnershipState::unknown)
    return unexpected(Unanswered{Unanswered::Reason::ownership_unknown});
  return game.is_owned() ? OwnershipAnswer::owned(game)
                         : OwnershipAnswer::not_owned();
}

auto Clearance::storefront() -> Expected<StorefrontAnswer, Unanswered> {
  return unexpected(Unanswered{Unanswered::Reason::rejected});
}

}  // namespace libchiaking::catalog::usecases::legacy_cloud_stream
