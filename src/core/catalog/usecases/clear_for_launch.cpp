#include "src/core/catalog/usecases/clear_for_launch.hpp"

#include <optional>
#include <utility>

namespace libchiaking::catalog::usecases {

ClearForLaunch::ClearForLaunch(platform::HttpProvider& http,
                               const platform::ClockProvider& clock,
                               const platform::ClockProvider& wall_clock,
                               TrustedClock* trusted_clock,
                               platform::DiagnosticsProvider* log,
                               HeldAnswers& held, ListGames& list)
    : list_(list),
      checks_(http, clock, trusted_clock, log, list),
      legacy_cloud_stream_(checks_),
      cloud_stream_(http, clock, wall_clock, log, held, list, checks_) {}

auto ClearForLaunch::can_launch(users::PsUser& user, const ProductId& product)
    -> LaunchVerdict {
  if (auto refused = Asker::turned_away(user))
    return LaunchVerdict::refused(
        LaunchRefusal::check_unavailable(refused->as(LaunchCheckReason::user)));
  const auto seen = list_.held_view(user);
  const auto* game = seen ? seen->games->find(product) : nullptr;
  if (game != nullptr && !streams_on_legacy_cloud_stream(*game))
    return cloud_stream_.can_launch(user, *seen, product);
  auto verdict = LaunchVerdict::before_launch(game, checks_.held_now());
  if (!verdict.is_allowed()) return verdict;
  return LaunchVerdict::cleared(LaunchChecks::clearance(*game, *seen, user));
}

auto ClearForLaunch::clear(users::PsUser& user, const ProductId& product,
                           const Stop& stop, CheckProgress progress)
    -> LaunchVerdict {
  progress.started(LaunchCheck::loading_library);
  auto loaded = list_.view(user, stop);
  if (!loaded) return LaunchChecks::unavailable(loaded.error());
  auto seen = loaded.value();
  const auto* game = seen.games->find(product);
  std::optional<users::Tier> tier;
  std::optional<users::CloudStreaming> streaming;
  const auto standing = LaunchChecks::standing_refusal(game);
  if (standing &&
      standing->hide_reason() == HideReason::tier_does_not_include) {
    if (streams_on_legacy_cloud_stream(*game)) {
      progress.started(LaunchCheck::membership);
      const auto asked = checks_.tier_at_launch(user, stop);
      if (!asked) return LaunchChecks::unavailable(asked.error());
      tier = asked.value();
    } else {
      progress.started(LaunchCheck::cloud_streaming);
      streaming = checks_.cloud_streaming_at_launch(user, stop);
      if (auto refused = LaunchChecks::refused_streaming(user, *streaming))
        return std::move(*refused);
    }
    auto again = checks_.held_or_loaded(user, stop, progress);
    if (!again) return LaunchChecks::unavailable(again.error());
    seen = std::move(again).value();
    game = seen.games->find(product);
  }
  if (game == nullptr)
    return LaunchVerdict::refused(LaunchRefusal::not_listed());
  if (streams_on_legacy_cloud_stream(*game))
    return legacy_cloud_stream_.clear(user, seen, *game, tier, stop, progress);
  return cloud_stream_.clear(user, std::move(seen), product,
                             std::move(streaming), stop, progress);
}

auto ClearForLaunch::ownership(users::PsUser& user, const ProductId& product,
                               const Stop& stop)
    -> Expected<OwnershipAnswer, Unanswered> {
  auto seen = list_.view(user, stop);
  if (!seen) return unexpected(seen.error());
  const auto* game = seen.value().games->find(product);
  if (game == nullptr)
    return unexpected(Unanswered{Unanswered::Reason::not_listed});
  if (streams_on_legacy_cloud_stream(*game))
    return legacy_cloud_stream::Clearance::ownership(*game);
  return cloud_stream_.ownership(user, seen.value(), product, stop);
}

auto ClearForLaunch::storefront(users::PsUser& user, const ProductId& product,
                                const Stop& stop)
    -> Expected<StorefrontAnswer, Unanswered> {
  if (auto refused = Asker::turned_away(user)) return unexpected(*refused);
  const auto seen = list_.held_view(user);
  const auto* game = seen ? seen->games->find(product) : nullptr;
  const bool on_legacy_cloud_stream =
      game != nullptr
          ? streams_on_legacy_cloud_stream(*game)
          : product.platform().value_or(Platform::ps5) != Platform::ps5;
  if (on_legacy_cloud_stream)
    return legacy_cloud_stream::Clearance::storefront();
  const auto asked = game != nullptr ? game->product_id() : product;
  std::optional<ConceptId> grouped_under;
  if (game != nullptr) {
    grouped_under = game->title().concept_id();
    if (!grouped_under && game->ownership())
      grouped_under = game->ownership()->concept_id();
  }
  auto answer = cloud_stream_.storefront(user, asked, grouped_under, stop);
  if (answer && answer.value().answered_product())
    list_.note_trial_offer(user, asked, answer.value().offers_trial());
  return answer;
}

auto ClearForLaunch::streams_on_legacy_cloud_stream(const MyGame& game) noexcept
    -> bool {
  return game.title().service() == ServiceGeneration::old_service;
}

}  // namespace libchiaking::catalog::usecases
