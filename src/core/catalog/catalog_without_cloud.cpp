#include "libchiaking/detail/catalog/catalog.hpp"

#include "src/core/catalog/adapters/cache/held_answers.hpp"
#include "src/core/catalog/model/unanswered.hpp"
#include "src/core/catalog/usecases/asker.hpp"
#include "src/core/catalog/usecases/inspect_caches.hpp"
#include "src/core/catalog/usecases/played_games.hpp"
#include "src/core/catalog/usecases/show_images.hpp"
#include "src/core/users/request_budget_core.hpp"

#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/storage/record_store.hpp>

#include <memory>
#include <optional>

namespace libchiaking::catalog {

class Catalog::Holdings {
 public:
  Holdings(platform::HttpProvider& http, platform::RecordStore& cache_location,
           platform::ThreadingProvider& threading,
           const platform::ClockProvider& clock,
           const platform::ClockProvider& wall_clock,
           TrustedClock* trusted_clock, users::RequestBudget& budget,
           platform::DiagnosticsProvider* diagnostics)
      : trusted_clock_(trusted_clock),
        held_(cache_location, threading, clock, wall_clock),
        pacer_(budget.core().pacer()),
        played_games_(http, clock, diagnostics, held_, pacer_),
        show_images_(http, threading, clock, diagnostics, held_),
        inspect_caches_(held_) {}

  template <class Reason>
  static auto left_out() -> CallFailure<Reason> {
    return CallFailure<Reason>{Reason::not_built};
  }

  static auto launch_left_out() -> LaunchVerdict {
    return LaunchVerdict::refused(
        LaunchRefusal::check_unavailable(left_out<LaunchCheckReason>()));
  }

  auto played_games() noexcept -> usecases::PlayedGames& {
    return played_games_;
  }
  auto show_images() noexcept -> usecases::ShowImages& { return show_images_; }
  auto inspect_caches() noexcept -> usecases::InspectCaches& {
    return inspect_caches_;
  }
  auto fetch_state(const users::PsUser& user) -> FetchState {
    return held_.fetch_board(user.account_id()).state();
  }
  auto trusted_time() const noexcept -> std::optional<TrustedClock::Time> {
    if (trusted_clock_ == nullptr) return std::nullopt;
    return trusted_clock_->now();
  }

  void start_over(users::PsUser& user) {
    held_.start_over(user.account_id(), usecases::Asker::region_of(user));
  }

  void forget(AccountId account) {
    held_.forget(account);
    pacer_.forget(account);
  }

 private:
  TrustedClock* trusted_clock_;
  HeldAnswers held_;
  users::RequestPacer& pacer_;
  usecases::PlayedGames played_games_;
  usecases::ShowImages show_images_;
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

auto Catalog::get_streamable_list(users::PsUser&, const platform::StopToken&)
    -> Expected<std::shared_ptr<const StreamableList>, StreamableListFailure> {
  return unexpected(Holdings::left_out<StreamableListReason>());
}

auto Catalog::library_of(users::PsUser&, const platform::StopToken&)
    -> Expected<std::shared_ptr<const Library>, LibraryFailure> {
  return unexpected(Holdings::left_out<LibraryReason>());
}

auto Catalog::process_library_and_list(users::PsUser&,
                                       const platform::StopToken&,
                                       const FetchObserver&)
    -> Expected<std::shared_ptr<const MyGames>, GamesFailure> {
  return unexpected(Holdings::left_out<GamesReason>());
}

auto Catalog::fetch_state(const users::PsUser& user) const -> FetchState {
  return holdings_->fetch_state(user);
}

auto Catalog::recently_played(users::PsUser&, const platform::StopToken&)
    -> Expected<std::vector<RecentlyPlayedGame>, RecentlyPlayedFailure> {
  return unexpected(Holdings::left_out<RecentlyPlayedReason>());
}

auto Catalog::played_games(users::PsUser& user, const platform::StopToken& stop)
    -> Expected<std::vector<PlayedGame>, PlayedGamesFailure> {
  return holdings_->played_games()
      .games(user, stop)
      .transform_error([](const Unanswered& why) {
        return why.as(PlayedGamesReason::malformed);
      });
}

auto Catalog::cloud_capture(users::PsUser&, std::string_view,
                            const platform::StopToken&)
    -> Expected<CloudCaptureFile, CloudCaptureFailure> {
  return unexpected(Holdings::left_out<CloudCaptureReason>());
}

auto Catalog::cloud_capture_download(users::PsUser&, const platform::StopToken&)
    -> Expected<bool, CaptureDownloadFailure> {
  return unexpected(Holdings::left_out<CaptureDownloadReason>());
}

auto Catalog::check_title_against_storefront(users::PsUser&, const ProductId&,
                                             const platform::StopToken&)
    -> Expected<StorefrontAnswer, StorefrontFailure> {
  return unexpected(Holdings::left_out<StorefrontReason>());
}

auto Catalog::recheck_ownership(users::PsUser&, const ProductId&,
                                const platform::StopToken&)
    -> Expected<OwnershipAnswer, OwnershipRecheckFailure> {
  return unexpected(Holdings::left_out<OwnershipRecheckReason>());
}

auto Catalog::can_launch(users::PsUser&, const ProductId&) const
    -> LaunchVerdict {
  return Holdings::launch_left_out();
}

auto Catalog::clear_for_launch(users::PsUser&, const ProductId&,
                               const platform::StopToken&,
                               const LaunchCheckObserver&) -> LaunchVerdict {
  return Holdings::launch_left_out();
}

void Catalog::remember_refusal(users::PsUser&, const ProductId&,
                               StreamRefusal::Kind) {}

auto Catalog::store_page_of(users::PsUser&, const ProductId&,
                            const platform::StopToken&)
    -> Expected<StorePage, StorePageFailure> {
  return unexpected(Holdings::left_out<StorePageReason>());
}

auto Catalog::details_of(users::PsUser&, const ProductId&,
                         const platform::StopToken&)
    -> Expected<GameDetails, ProductDetailsFailure> {
  return unexpected(Holdings::left_out<ProductDetailsReason>());
}

auto Catalog::details_of(users::PsUser&, const ConceptId&,
                         const platform::StopToken&)
    -> Expected<GameDetails, ConceptDetailsFailure> {
  return unexpected(Holdings::left_out<ConceptDetailsReason>());
}

auto Catalog::image_of(const ImageRef& image, const platform::StopToken& stop)
    -> Expected<std::vector<std::byte>, ImageFailure> {
  return holdings_->show_images()
      .image(image, stop)
      .transform_error([](const Unanswered& why) {
        return why.as(ImageReason::unavailable);
      });
}

auto Catalog::cloud_access_of(users::PsUser&, const platform::StopToken&)
    -> Expected<CloudAccess, CloudAccessFailure> {
  return unexpected(Holdings::left_out<CloudAccessReason>());
}

auto Catalog::explain(users::PsUser&, const ProductId& product) const
    -> Explanation {
  return Explanation::not_listed(product);
}

auto Catalog::held_trials(users::PsUser&) const -> std::vector<MyGame> {
  return {};
}

auto Catalog::trusted_time() const noexcept -> std::optional<Time> {
  return holdings_->trusted_time();
}

auto Catalog::how_fetched(users::PsUser&) const -> std::optional<FetchReport> {
  return std::nullopt;
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
