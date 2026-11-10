#include "libchiaking/detail/catalog/catalog.hpp"

#include "src/core/catalog/adapters/cache/held_answers.hpp"
#include "src/core/catalog/adapters/ps/store_details.hpp"
#include "src/core/catalog/model/unanswered.hpp"
#include "src/core/catalog/usecases/clear_for_launch.hpp"
#include "src/core/catalog/usecases/inspect_caches.hpp"
#include "src/core/catalog/usecases/list_games.hpp"
#include "src/core/catalog/usecases/played_games.hpp"
#include "src/core/catalog/usecases/recently_played.hpp"
#include "src/core/catalog/usecases/show_details.hpp"
#include "src/core/catalog/usecases/show_images.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/users/ports/catalog_calls.hpp"
#include "src/core/users/request_budget_core.hpp"

#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/storage/record_store.hpp>

#include <memory>
#include <optional>
#include <utility>

namespace libchiaking::catalog {

class Catalog::Holdings {
 public:
  using Stop = platform::StopToken;

  Holdings(platform::HttpProvider& http, platform::RecordStore& cache_location,
           platform::ThreadingProvider& threading,
           const platform::ClockProvider& clock,
           const platform::ClockProvider& wall_clock,
           TrustedClock* trusted_clock, users::RequestBudget& budget,
           platform::DiagnosticsProvider* diagnostics)
      : http_(http),
        clock_(clock),
        trusted_clock_(trusted_clock),
        log_(diagnostics),
        held_(cache_location, threading, clock, wall_clock),
        pacer_(budget.core().pacer()),
        list_games_(http, clock, wall_clock, trusted_clock, diagnostics, held_),
        clear_for_launch_(http, clock, wall_clock, trusted_clock, diagnostics,
                          held_, list_games_),
        show_details_(http, threading, clock, wall_clock, diagnostics, held_,
                      list_games_),
        show_images_(http, threading, clock, diagnostics, held_),
        recently_played_(http, clock, diagnostics, list_games_),
        played_games_(http, clock, diagnostics, held_, pacer_),
        inspect_caches_(held_) {}

  auto list_games() noexcept -> usecases::ListGames& { return list_games_; }
  auto clear_for_launch() noexcept -> usecases::ClearForLaunch& {
    return clear_for_launch_;
  }
  auto show_details() noexcept -> usecases::ShowDetails& {
    return show_details_;
  }
  auto show_images() noexcept -> usecases::ShowImages& { return show_images_; }
  auto recently_played() noexcept -> usecases::RecentlyPlayed& {
    return recently_played_;
  }
  auto played_games() noexcept -> usecases::PlayedGames& {
    return played_games_;
  }
  auto inspect_caches() noexcept -> usecases::InspectCaches& {
    return inspect_caches_;
  }
  auto http() noexcept -> platform::HttpProvider& { return http_; }
  auto clock() noexcept -> const platform::ClockProvider& { return clock_; }
  auto log() noexcept -> platform::DiagnosticsProvider* { return log_; }
  auto trusted_time() const noexcept -> std::optional<TrustedClock::Time> {
    if (trusted_clock_ == nullptr) return std::nullopt;
    return trusted_clock_->now();
  }

  void remember_refusal(users::PsUser& user, const ProductId& product,
                        StreamRefusal::Kind kind) {
    list_games_.remember_refusal(user, product, kind);
  }

  [[nodiscard]] auto store_page(users::PsUser& user, const ProductId& product,
                                const Stop& stop)
      -> Expected<StorePage, Unanswered> {
    if (auto refused = usecases::Asker::turned_away(user))
      return unexpected(*refused);
    const auto seen = list_games_.held_view(user);
    const auto list = seen && seen->list ? seen->list.value() : nullptr;
    const auto* listed = list ? list->find(product) : nullptr;
    const auto locale = list ? list->settled_locale()
                             : usecases::Asker::region_of(user).value_or(
                                   StoreLocale::english_us());
    net::http::Client client{http_, clock_, stop, log_};
    return ps::StorePages{client}.page_of(product, locale,
                                          listed != nullptr
                                              ? listed->title.concept_page()
                                              : std::optional<StorePage>{});
  }

  auto explain(users::PsUser& user, const ProductId& product) -> Explanation {
    if (usecases::Asker::turned_away(user))
      return Explanation::not_listed(product);
    const auto seen = list_games_.held_view(user);
    return seen ? seen->games->explain(product)
                : Explanation::not_listed(product);
  }

  auto held_trials(users::PsUser& user) -> std::vector<MyGame> {
    if (usecases::Asker::turned_away(user)) return {};
    const auto seen = list_games_.held_view(user);
    if (!seen) return {};
    std::vector<MyGame> trials;
    for (const auto& game : seen->games->visible()) {
      const auto& held = game.ownership();
      if (game.title().service() == ServiceGeneration::ps5_cloud && held &&
          held->held_as() == HeldAs::trial_access && !held->trial_ended())
        trials.push_back(game);
    }
    return trials;
  }

  auto how_fetched(users::PsUser& user) -> std::optional<FetchReport> {
    if (usecases::Asker::turned_away(user)) return std::nullopt;
    return list_games_.how_fetched(user);
  }

  void start_over(users::PsUser& user) {
    held_.start_over(user.account_id(), usecases::Asker::region_of(user));
  }

  void forget(AccountId account) {
    held_.forget(account);
    pacer_.forget(account);
  }

 private:
  platform::HttpProvider& http_;
  const platform::ClockProvider& clock_;
  TrustedClock* trusted_clock_;
  platform::DiagnosticsProvider* log_;
  HeldAnswers held_;
  users::RequestPacer& pacer_;
  usecases::ListGames list_games_;
  usecases::ClearForLaunch clear_for_launch_;
  usecases::ShowDetails show_details_;
  usecases::ShowImages show_images_;
  usecases::RecentlyPlayed recently_played_;
  usecases::PlayedGames played_games_;
  usecases::InspectCaches inspect_caches_;
};

Catalog::Catalog(platform::HttpProvider& http,
                 platform::RecordStore& cache_location,
                 platform::ThreadingProvider& threading,
                 const platform::ClockProvider& clock,
                 const platform::ClockProvider& wall_clock,
                 TrustedClock& trusted_clock, users::RequestBudget& budget,
                 platform::DiagnosticsProvider* diagnostics)
    : holdings_(std::make_unique<Holdings>(http, cache_location, threading,
                                           clock, wall_clock, &trusted_clock,
                                           budget, diagnostics)) {}

Catalog::Catalog(platform::HttpProvider& http,
                 platform::RecordStore& cache_location,
                 platform::ThreadingProvider& threading,
                 const platform::ClockProvider& clock,
                 const platform::ClockProvider& wall_clock,
                 users::RequestBudget& budget,
                 platform::DiagnosticsProvider* diagnostics)
    : holdings_(std::make_unique<Holdings>(http, cache_location, threading,
                                           clock, wall_clock, nullptr, budget,
                                           diagnostics)) {}

Catalog::~Catalog() = default;

auto Catalog::get_streamable_list(users::PsUser& user,
                                  const platform::StopToken& stop)
    -> Expected<std::shared_ptr<const StreamableList>, StreamableListFailure> {
  return holdings_->list_games()
      .streamable_list(user, stop)
      .transform_error([](const Unanswered& why) {
        return why.as(StreamableListReason::unavailable);
      });
}

auto Catalog::library_of(users::PsUser& user, const platform::StopToken& stop)
    -> Expected<std::shared_ptr<const Library>, LibraryFailure> {
  return holdings_->list_games()
      .library(user, stop)
      .transform_error([](const Unanswered& why) {
        return why.as(LibraryReason::malformed);
      });
}

auto Catalog::process_library_and_list(users::PsUser& user,
                                       const platform::StopToken& stop,
                                       const FetchObserver& observer)
    -> Expected<std::shared_ptr<const MyGames>, GamesFailure> {
  return holdings_->list_games()
      .games(user, stop, observer)
      .transform_error([](const Unanswered& why) {
        return why.as(GamesReason::unavailable);
      });
}

auto Catalog::fetch_state(const users::PsUser& user) const -> FetchState {
  return holdings_->list_games().fetch_state(user);
}

auto Catalog::recently_played(users::PsUser& user,
                              const platform::StopToken& stop)
    -> Expected<std::vector<RecentlyPlayedGame>, RecentlyPlayedFailure> {
  return holdings_->recently_played()
      .games(user, stop)
      .transform_error([](const Unanswered& why) {
        return why.as(RecentlyPlayedReason::unavailable);
      });
}

auto Catalog::played_games(users::PsUser& user, const platform::StopToken& stop)
    -> Expected<std::vector<PlayedGame>, PlayedGamesFailure> {
  return holdings_->played_games()
      .games(user, stop)
      .transform_error([](const Unanswered& why) {
        return why.as(PlayedGamesReason::malformed);
      });
}

auto Catalog::cloud_capture(users::PsUser& user, std::string_view gallery_id,
                            const platform::StopToken& stop)
    -> Expected<CloudCaptureFile, CloudCaptureFailure> {
  if (auto refused = usecases::Asker::turned_away(user))
    return unexpected(refused->as(CloudCaptureReason::user));
  net::http::Client client{holdings_->http(), holdings_->clock(), stop,
                           holdings_->log()};
  auto found = users::CatalogCalls{user}.gallery_capture(client, gallery_id);
  if (!found)
    return unexpected(Unanswered{found.error()}.as(CloudCaptureReason::user));
  return CloudCaptureFile{std::move(found->download_url),
                          std::move(found->file_type), found->file_size};
}

auto Catalog::cloud_capture_download(users::PsUser& user,
                                     const platform::StopToken& stop)
    -> Expected<bool, CaptureDownloadFailure> {
  if (auto refused = usecases::Asker::turned_away(user))
    return unexpected(refused->as(CaptureDownloadReason::user));
  net::http::Client client{holdings_->http(), holdings_->clock(), stop,
                           holdings_->log()};
  auto setting = users::CatalogCalls{user}.cloud_capture_download(client);
  if (!setting)
    return unexpected(
        Unanswered{setting.error()}.as(CaptureDownloadReason::user));
  return *setting;
}

auto Catalog::check_title_against_storefront(users::PsUser& user,
                                             const ProductId& product,
                                             const platform::StopToken& stop)
    -> Expected<StorefrontAnswer, StorefrontFailure> {
  return holdings_->clear_for_launch()
      .storefront(user, product, stop)
      .transform_error([](const Unanswered& why) {
        return why.as(StorefrontReason::unavailable);
      });
}

auto Catalog::recheck_ownership(users::PsUser& user, const ProductId& product,
                                const platform::StopToken& stop)
    -> Expected<OwnershipAnswer, OwnershipRecheckFailure> {
  return holdings_->clear_for_launch()
      .ownership(user, product, stop)
      .transform_error([](const Unanswered& why) {
        return why.as(OwnershipRecheckReason::unavailable);
      });
}

auto Catalog::can_launch(users::PsUser& user, const ProductId& product) const
    -> LaunchVerdict {
  return holdings_->clear_for_launch().can_launch(user, product);
}

auto Catalog::clear_for_launch(users::PsUser& user, const ProductId& product,
                               const platform::StopToken& stop,
                               const LaunchCheckObserver& observer)
    -> LaunchVerdict {
  return holdings_->clear_for_launch().clear(user, product, stop,
                                             usecases::CheckProgress{observer});
}

void Catalog::remember_refusal(users::PsUser& user, const ProductId& product,
                               StreamRefusal::Kind kind) {
  holdings_->remember_refusal(user, product, kind);
}

auto Catalog::store_page_of(users::PsUser& user, const ProductId& product,
                            const platform::StopToken& stop)
    -> Expected<StorePage, StorePageFailure> {
  return holdings_->store_page(user, product, stop)
      .transform_error([](const Unanswered& why) {
        return why.as(StorePageReason::not_listed);
      });
}

auto Catalog::details_of(users::PsUser& user, const ProductId& product,
                         const platform::StopToken& stop)
    -> Expected<GameDetails, ProductDetailsFailure> {
  return holdings_->show_details()
      .details(user, product, stop)
      .transform_error([](const Unanswered& why) {
        return why.as(ProductDetailsReason::unavailable);
      });
}

auto Catalog::details_of(users::PsUser& user, const ConceptId& grouped_under,
                         const platform::StopToken& stop)
    -> Expected<GameDetails, ConceptDetailsFailure> {
  return holdings_->show_details()
      .details(user, grouped_under, stop)
      .transform_error([](const Unanswered& why) {
        return why.as(ConceptDetailsReason::unavailable);
      });
}

auto Catalog::image_of(const ImageRef& image, const platform::StopToken& stop)
    -> Expected<std::vector<std::byte>, ImageFailure> {
  return holdings_->show_images()
      .image(image, stop)
      .transform_error([](const Unanswered& why) {
        return why.as(ImageReason::unavailable);
      });
}

auto Catalog::cloud_access_of(users::PsUser& user,
                              const platform::StopToken& stop)
    -> Expected<CloudAccess, CloudAccessFailure> {
  return holdings_->list_games()
      .cloud_access(user, stop)
      .transform_error([](const Unanswered& why) {
        return why.as(CloudAccessReason::user);
      });
}

auto Catalog::explain(users::PsUser& user, const ProductId& product) const
    -> Explanation {
  return holdings_->explain(user, product);
}

auto Catalog::held_trials(users::PsUser& user) const -> std::vector<MyGame> {
  return holdings_->held_trials(user);
}

auto Catalog::trusted_time() const noexcept -> std::optional<Time> {
  return holdings_->trusted_time();
}

auto Catalog::how_fetched(users::PsUser& user) const
    -> std::optional<FetchReport> {
  return holdings_->how_fetched(user);
}

void Catalog::start_over(users::PsUser& user) { holdings_->start_over(user); }

void Catalog::forget(AccountId account) { holdings_->forget(account); }

auto Catalog::caches() -> Expected<std::vector<CacheSummary>, CachesFailure> {
  return holdings_->inspect_caches().summaries().transform_error(
      [](const Unanswered& why) { return why.as(CachesReason::storage); });
}

auto Catalog::cache_entries(CacheId cache)
    -> Expected<std::vector<CacheEntry>, CacheEntriesFailure> {
  return holdings_->inspect_caches().entries(cache).transform_error(
      [](const Unanswered& why) {
        return why.as(CacheEntriesReason::storage);
      });
}

auto Catalog::clear_cache(CacheId cache) -> Expected<void, ClearCacheFailure> {
  return holdings_->inspect_caches().clear(cache).transform_error(
      [](const Unanswered& why) { return why.as(ClearCacheReason::storage); });
}

auto Catalog::clear_caches() -> Expected<void, ClearCachesFailure> {
  return holdings_->inspect_caches().clear_all().transform_error(
      [](const Unanswered& why) { return why.as(ClearCachesReason::storage); });
}

}  // namespace libchiaking::catalog
