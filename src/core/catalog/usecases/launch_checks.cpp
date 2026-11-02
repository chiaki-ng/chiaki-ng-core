#include "src/core/catalog/usecases/launch_checks.hpp"

#include "src/core/net/transport/http/client.hpp"
#include "src/core/users/ports/catalog_calls.hpp"

#include <utility>

namespace libchiaking::catalog::usecases {

LaunchChecks::LaunchChecks(platform::HttpProvider& http,
                           const platform::ClockProvider& clock,
                           TrustedClock* trusted_clock,
                           platform::DiagnosticsProvider* log,
                           ListGames& list) noexcept
    : http_(http),
      clock_(clock),
      trusted_(trusted_clock),
      log_(log),
      list_(list) {}

auto LaunchChecks::held_or_loaded(users::PsUser& user, const Stop& stop,
                                  CheckProgress progress)
    -> Expected<View, Unanswered> {
  if (auto held = list_.held_view(user)) return std::move(*held);
  progress.started(LaunchCheck::loading_library);
  return list_.view(user, stop);
}

auto LaunchChecks::tier_at_launch(users::PsUser& user, const Stop& stop)
    -> Expected<users::Tier, Unanswered> {
  const auto held = user.membership().tier();
  if (held && *held >= streaming_tier) return *held;
  net::http::Client client{http_, clock_, stop, log_};
  const auto asked = users::CatalogCalls{user}.answered_membership(
      client, held ? streaming_tier : users::Tier::none);
  if (asked && asked.value().membership.tier())
    return *asked.value().membership.tier();
  if (held &&
      (asked || asked.error().reason != users::Unanswered::Reason::cancelled))
    return *held;
  return unexpected(asked ? Unanswered{Unanswered::Reason::malformed}
                          : Unanswered{asked.error()});
}

auto LaunchChecks::cloud_streaming_at_launch(users::PsUser& user,
                                             const Stop& stop)
    -> users::CloudStreaming {
  net::http::Client client{http_, clock_, stop, log_};
  return users::CatalogCalls{user}.cloud_streaming(
      client, users::CloudStreamingCache::Freshness::read_afresh);
}

auto LaunchChecks::refused_streaming(const users::PsUser& user,
                                     const users::CloudStreaming& streaming)
    -> std::optional<LaunchVerdict> {
  if (streaming.is_allowed()) return std::nullopt;
  if (const auto cause = streaming.cause())
    return unavailable(Unanswered{*cause});
  return LaunchVerdict::refused(
      LaunchRefusal::needs_premium(user.membership().tier()));
}

auto LaunchChecks::held_streaming_refusal(users::PsUser& user)
    -> std::optional<LaunchVerdict> {
  const auto held = users::CatalogCalls{user}.held_cloud_streaming();
  if (!held) return unavailable(Unanswered{Unanswered::Reason::unavailable});
  return refused_streaming(user, *held);
}

auto LaunchChecks::held_now() const noexcept
    -> std::optional<TrustedClock::Time> {
  return trusted_ != nullptr ? trusted_->now() : std::nullopt;
}

auto LaunchChecks::release_refusal(const MyGame& game, const Stop& stop)
    -> std::optional<LaunchRefusal> {
  return release_refusal(game, game.release(), stop);
}

auto LaunchChecks::release_refusal(const MyGame& game, ReleaseDate release,
                                   const Stop& stop)
    -> std::optional<LaunchRefusal> {
  const auto trusted_now = release.time() && trusted_ != nullptr
                               ? trusted_->refresh(stop, {})
                               : held_now();
  return refusal_at(game, release, trusted_now);
}

auto LaunchChecks::held_release_refusal(const MyGame& game,
                                        ReleaseDate release) const
    -> std::optional<LaunchRefusal> {
  return refusal_at(game, release, held_now());
}

auto LaunchChecks::refusal_at(const MyGame& game, ReleaseDate release,
                              std::optional<TrustedClock::Time> trusted_now)
    -> std::optional<LaunchRefusal> {
  if (auto unreleased = LaunchRefusal::of_release(release, trusted_now))
    return unreleased;
  if (const auto& remembered = game.remembered_refusal())
    return LaunchRefusal::refused_by_service(remembered->refused_at());
  return std::nullopt;
}

auto LaunchChecks::unavailable(Unanswered cause) -> LaunchVerdict {
  return LaunchVerdict::refused(LaunchRefusal::check_unavailable(
      cause.as(LaunchCheckReason::unavailable)));
}

auto LaunchChecks::standing_refusal(const MyGame* game)
    -> std::optional<LaunchRefusal> {
  const auto verdict = LaunchVerdict::of(game, std::nullopt);
  if (verdict.is_allowed() || waits_for_release(verdict.refusal()->kind()))
    return std::nullopt;
  return *verdict.refusal();
}

auto LaunchChecks::waits_for_release(LaunchRefusal::Kind kind) noexcept
    -> bool {
  return kind == LaunchRefusal::Kind::unannounced ||
         kind == LaunchRefusal::Kind::not_released ||
         kind == LaunchRefusal::Kind::no_trusted_clock;
}

auto LaunchChecks::clearance(const MyGame& game, const View& seen,
                             const users::PsUser& user) -> ClearedLaunch {
  const auto* list = seen.list ? seen.list.value().get() : nullptr;
  const auto* category = seen.category ? seen.category.value().get() : nullptr;
  auto store = list != nullptr ? list->settled_locale()
               : category != nullptr
                   ? category->locale()
                   : Asker::region_of(user).value_or(StoreLocale::english_us());
  return ClearedLaunch{
      .product = game.product_id(),
      .service = game.title().service(),
      .platform = game.title().platform(),
      .identifier = game.launch_identifier(),
      .held_as = game.ownership() ? std::optional{game.ownership()->id()}
                                  : std::nullopt,
      .store = std::move(store),
      .origin = list != nullptr ? list->origin() : ListOrigin::account_service,
  };
}

}  // namespace libchiaking::catalog::usecases
