#include "src/core/catalog/usecases/list_games.hpp"

#include "src/core/catalog/adapters/ps/old_service_shelves.hpp"
#include "src/core/catalog/adapters/ps/ownership_records.hpp"
#include "src/core/catalog/adapters/ps/ps5_library.hpp"
#include "src/core/catalog/adapters/ps/ps5_store_lists.hpp"
#include "src/core/catalog/adapters/ps/public_store_walk.hpp"
#include "src/core/catalog/adapters/ps/streaming_category.hpp"
#include "src/core/catalog/adapters/ps/streaming_copies.hpp"
#include "src/core/catalog/model/library/owned_record.hpp"
#include "src/core/catalog/model/library/ps5_entitlement.hpp"
#include "src/core/catalog/model/my_games/my_games_assembly.hpp"
#include "src/core/catalog/usecases/cloud_stream/check_exclusion.hpp"
#include "src/core/users/ports/catalog_calls.hpp"

#include <libchiaking/platform/diagnostics/provider.hpp>

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::catalog::usecases {

ListGames::ListGames(platform::HttpProvider& http,
                     const platform::ClockProvider& clock,
                     const platform::ClockProvider& wall_clock,
                     TrustedClock* trusted_clock,
                     platform::DiagnosticsProvider* log, HeldAnswers& held)
    : http_(http),
      clock_(clock),
      wall_(wall_clock),
      trusted_(trusted_clock),
      log_(log),
      held_(held),
      libraries_(http, clock, wall_clock, log, held),
      lists_(http, clock, wall_clock, log, held),
      categories_(http, clock, wall_clock, log, held) {}

auto ListGames::streamable_list(users::PsUser& user, const Stop& stop)
    -> Expected<ListPtr, Unanswered> {
  return lists_.list(user, stop);
}

auto ListGames::library(users::PsUser& user, const Stop& stop)
    -> Expected<LibraryPtr, Unanswered> {
  return libraries_.library(user, stop);
}

auto ListGames::library_afresh(users::PsUser& user, const Stop& stop)
    -> Expected<LibraryPtr, Unanswered> {
  return libraries_.afresh(user, stop);
}

auto ListGames::games(users::PsUser& user, const Stop& stop,
                      const FetchObserver& observer)
    -> Expected<GamesPtr, Unanswered> {
  auto seen = view(user, stop, observer);
  if (!seen) return unexpected(seen.error());
  return std::move(seen.value().games);
}

auto ListGames::view(users::PsUser& user, const Stop& stop,
                     const FetchObserver& observer)
    -> Expected<View, Unanswered> {
  const auto account = user.account_id();
  auto& board = held_.fetch_board(account);
  const auto watching = board.watch(observer);
  const auto wanted = board.epoch();
  const auto question = HeldAnswers::Question::by(account);
  for (;;) {
    auto fetched = held_.in_flight().fetches.answer(
        question.text(), stop,
        [&]() -> Expected<HeldAnswers::Fetched, Unanswered> {
          const auto begun = board.begin(watching);
          if (begun && begun->starts_over)
            held_.drop_answers(account, Asker::region_of(user));
          auto view =
              led(user, stop,
                  FetchProgress{board, begun ? std::optional{begun->attempt}
                                             : std::nullopt});
          if (!view && view.error().is_cancelled())
            return unexpected(view.error());
          return HeldAnswers::Fetched{begun ? begun->epoch : board.epoch(),
                                      begun ? begun->attempt : 0,
                                      std::move(view)};
        });
    if (!fetched) return unexpected(fetched.error());
    if (fetched.value().epoch >= wanted) {
      board.catch_up(watching, fetched.value().attempt);
      return std::move(fetched.value().view);
    }
    if (stop.stop_requested()) return unexpected(Unanswered::cancelled());
    board.follow_again(watching);
  }
}

auto ListGames::fetch_state(const users::PsUser& user) -> FetchState {
  return held_.fetch_board(user.account_id()).state();
}

auto ListGames::led(users::PsUser& user, const Stop& stop,
                    const FetchProgress& progress)
    -> Expected<View, Unanswered> {
  auto reply = built(user, stop, progress);
  if (reply)
    progress.finished();
  else
    progress.failed(reply.error());
  return reply;
}

auto ListGames::built(users::PsUser& user, const Stop& stop,
                      const FetchProgress& progress)
    -> Expected<View, Unanswered> {
  using enum FetchActivity;
  if (auto refused = Asker::turned_away(user)) return unexpected(*refused);
  progress.ended(StepOutcome::held);
  const auto account = user.account_id();
  const auto region = Asker::region_of(user);
  const bool list_held = region && held_.fresh_list(*region);
  progress.began(FetchStep::cloud_list,
                 list_held ? std::optional{reusing_held_answer} : std::nullopt);
  auto list = lists_.list(user, stop, progress);
  if (!list && Asker::ends_the_load(list.error()))
    return unexpected(list.error());
  progress.ended(list_held ? StepOutcome::held
                 : list    ? StepOutcome::fetched
                           : StepOutcome::degraded);
  const bool library_held = libraries_.whole_fresh(account) != nullptr;
  progress.began(
      FetchStep::owned_library,
      library_held ? std::optional{reusing_held_answer} : std::nullopt);
  auto owned = libraries_.owned_by(user, stop, progress);
  if (!owned) return unexpected(owned.error());
  report_library(owned.value(), library_held, progress);
  const auto demand =
      held_.starting_over(account) ? Demand::start_over : Demand::when_stale;
  auto category = category_step<catalog::cloud_stream::PremiumCategory>(
      user, FetchStep::premium_category, demand, progress,
      [&](const Asker::Audience& audience) {
        return held_.held_included(audience.locale, audience.age);
      },
      [&] { return categories_.included(user, demand, stop, progress); });
  if (!category && category.error().is_cancelled())
    return unexpected(category.error());
  auto sold = category_step<catalog::cloud_stream::ForSaleCategory>(
      user, FetchStep::for_sale_category, demand, progress,
      [&](const Asker::Audience& audience) {
        return held_.held_for_sale(audience.locale, audience.age);
      },
      [&] { return categories_.for_sale(user, demand, stop, progress); });
  if (!sold && sold.error().is_cancelled()) return unexpected(sold.error());
  if (const auto asked = streaming_for_tiles(user, stop, progress); !asked)
    return unexpected(asked.error());
  held_.started_afresh(account);
  if (has_nothing_to_show(list, category, owned.value()))
    return unexpected(list.error());
  progress.began(FetchStep::matching, matching_games);
  auto games = matched(user, list ? list.value() : nullptr, owned.value(),
                       category ? category.value() : nullptr,
                       sold ? sold.value() : nullptr);
  progress.ended(StepOutcome::fetched);
  if (trusted_ != nullptr && awaits_a_release(*games)) {
    progress.began(FetchStep::trusted_time, syncing_clock);
    const auto synced = trusted_->refresh(
        stop, [&progress](std::string_view host) { progress.asking(host); });
    progress.ended(synced ? StepOutcome::fetched : StepOutcome::degraded);
  } else {
    progress.skipped(FetchStep::trusted_time);
  }
  if (shows_a_ps5_game(*games)) {
    const auto held_exclusions = held_.held_exclusions();
    const bool fresh =
        held_exclusions && held_exclusions->is_fresh_at(held_.now());
    progress.began(FetchStep::exclusions,
                   fresh ? reusing_held_answer : reading_exclusions);
    const auto kept =
        cloud_stream::CheckExclusion{http_, clock_, wall_, log_, held_}
            .keep_current(stop);
    if (!kept && kept.error().is_cancelled()) return unexpected(kept.error());
    progress.ended(fresh  ? StepOutcome::held
                   : kept ? StepOutcome::fetched
                          : StepOutcome::degraded);
  } else {
    progress.skipped(FetchStep::exclusions);
  }
  return View{std::move(games), std::move(list), std::move(category)};
}

void ListGames::report_library(const HeldAnswers::Owned& owned, bool held,
                               const FetchProgress& progress) {
  const bool whole =
      owned.state == LibraryState::fetched &&
      (!owned.ps5 || owned.ps5->state() == LibraryState::fetched);
  progress.ended(held    ? StepOutcome::held
                 : whole ? StepOutcome::fetched
                         : StepOutcome::degraded);
}

template <class Category, class Held, class Read>
auto ListGames::category_step(const users::PsUser& user, FetchStep step,
                              Demand demand, const FetchProgress& progress,
                              const Held& held, const Read& read)
    -> Expected<std::shared_ptr<const Category>, Unanswered> {
  const auto audience = Asker::audience_of(user);
  if (!audience) {
    progress.skipped(step);
    return read();
  }
  const bool asks = held(*audience).record->next_step(held_.now(), demand) ==
                    cache::category::Step::fetch;
  progress.began(step, asks
                           ? std::nullopt
                           : std::optional{FetchActivity::reusing_held_answer});
  auto answer = read();
  if (!answer && answer.error().is_cancelled()) return answer;
  progress.ended(!asks    ? StepOutcome::held
                 : answer ? StepOutcome::fetched
                          : StepOutcome::degraded);
  return answer;
}

auto ListGames::has_nothing_to_show(
    const Expected<ListPtr, Unanswered>& list,
    const Expected<CategoryPtr, Unanswered>& category,
    const HeldAnswers::Owned& owned) -> bool {
  const bool ps5_read =
      owned.ps5 && owned.ps5->state() != LibraryState::unavailable;
  return !list && !(category && category.value()) && !ps5_read;
}

auto ListGames::streaming_for_tiles(users::PsUser& user, const Stop& stop,
                                    const FetchProgress& progress)
    -> Expected<void, Unanswered> {
  const bool held =
      users::CatalogCalls{user}.held_cloud_streaming().has_value();
  progress.began(FetchStep::streaming_access,
                 held ? FetchActivity::reusing_held_answer
                      : FetchActivity::reading_subscriptions);
  net::http::Client client{http_, clock_, stop, log_};
  const auto verdict = users::CatalogCalls{user}.cloud_streaming(
      client, users::CloudStreamingCache::Freshness::held_will_do);
  const auto cause = verdict.cause();
  if (cause && cause->reason == users::Unanswered::Reason::cancelled)
    return unexpected(Unanswered{*cause});
  progress.ended(held    ? StepOutcome::held
                 : cause ? StepOutcome::degraded
                         : StepOutcome::fetched);
  return {};
}

auto ListGames::awaits_a_release(const MyGames& games) -> bool {
  return std::ranges::any_of(games.visible(), [](const MyGame& game) {
    return game.release().time().has_value();
  });
}

auto ListGames::shows_a_ps5_game(const MyGames& games) -> bool {
  return std::ranges::any_of(games.visible(), [](const MyGame& game) {
    return game.title().service() == ServiceGeneration::ps5_cloud;
  });
}

auto ListGames::held_view(users::PsUser& user) -> std::optional<View> {
  const auto region = Asker::region_of(user);
  auto list = region ? held_.held_list(*region) : nullptr;
  auto owned = held_.held_by(user.account_id());
  auto category = category_held_for(user);
  if (!owned || (!list && !category && !owned->ps5)) return std::nullopt;
  auto games = matched(user, list, *owned, category, for_sale_held_for(user));
  return View{std::move(games),
              list ? Expected<ListPtr, Unanswered>{std::move(list)}
                   : unexpected(Unanswered{Unanswered::Reason::unavailable}),
              std::move(category)};
}

void ListGames::note_trial_offer(users::PsUser& user, const ProductId& product,
                                 bool offered) {
  if (Asker::turned_away(user)) return;
  const auto account = user.account_id();
  const auto seen = held_view(user);
  const auto* game = seen ? seen->games->find(product) : nullptr;
  const auto& listed = game != nullptr ? game->product_id() : product;
  if (offered || held_.trials().is_used(account, listed)) {
    (void)held_.trials().note(account, listed, offered);
    return;
  }
  const bool labels = game != nullptr &&
                      game->play_path() == PlayPath::store_only &&
                      game->trial_offer() == TrialOffer::play_first_trial;
  if (labels) (void)held_.trials().note(account, listed, false);
}

void ListGames::remember_refusal(users::PsUser& user, const ProductId& product,
                                 StreamRefusal::Kind kind) {
  if (Asker::turned_away(user)) return;
  const auto account = user.account_id();
  const auto seen = held_view(user);
  (void)held_.refusals().remember(
      account, product, kind,
      {user.membership().tier(), seen ? seen->games->holding_of(product)
                                      : StreamRefusal::Holding::unknown()});
  if (kind == StreamRefusal::Kind::no_game_for_entitlement)
    held_.forget_library(account);
}

auto ListGames::how_fetched(users::PsUser& user) -> std::optional<FetchReport> {
  const auto region = Asker::region_of(user);
  const auto list = region ? held_.held_list(*region) : nullptr;
  const auto owned = held_.held_by(user.account_id());
  FetchReport report;
  if (list) {
    report.legacy_cloud_stream.origin = list->origin();
    report.legacy_cloud_stream.complete = list->is_complete();
    report.legacy_cloud_stream.fetched_at = list->fetched_at();
    report.legacy_cloud_stream.titles =
        list->count_on(ServiceGeneration::old_service);
    report.legacy_cloud_stream.copies_checked = list->streaming_copies();
    report.legacy_cloud_stream.without_a_covered_copy =
        static_cast<std::size_t>(
            std::ranges::count_if(list->titles(), [](const auto& entry) {
              return entry.title.service() == ServiceGeneration::old_service &&
                     entry.title.streamability() ==
                         ListStreamability::marked_unstreamable;
            }));
  }
  report.cloud_stream = ps5_report(user, owned);
  if (!list && !owned && !report.cloud_stream.premium_category &&
      !report.cloud_stream.for_sale_category && !report.cloud_stream.exclusions)
    return std::nullopt;
  if (owned) {
    report.library = owned->state;
    report.library_fetched_at = owned->titles->fetched_at();
    report.library_records = owned->titles->titles().size();
  }
  report.tier = user.membership().tier();
  return report;
}

auto ListGames::ps5_report(users::PsUser& user,
                           const std::optional<HeldAnswers::Owned>& owned)
    -> FetchReport::CloudStream {
  FetchReport::CloudStream report;
  if (const auto audience = Asker::audience_of(user)) {
    report.audience = FetchReport::Audience{
        audience->locale.country(), audience->locale.language(), audience->age};
    if (const auto category =
            held_.held_included(audience->locale, audience->age).category) {
      const auto& scan = category->scan();
      report.premium_category = FetchReport::PremiumCategory{
          .fetched_at = category->fetched_at(),
          .titles = category->included_count(),
          .products_read = scan.products_read,
          .server_total = scan.server_total,
          .complete = scan.complete,
      };
    }
    if (const auto for_sale =
            held_.held_for_sale(audience->locale, audience->age).category) {
      const auto& scan = for_sale->scan();
      report.for_sale_category = FetchReport::ForSaleCategory{
          .fetched_at = for_sale->fetched_at(),
          .titles = for_sale->for_sale_count(),
          .products_read = scan.products_read,
          .server_total = scan.server_total,
          .complete = scan.complete,
      };
    }
  }
  if (owned && owned->ps5)
    report.owned = FetchReport::OwnedGames{
        .state = owned->ps5->state(),
        .records_read = owned->ps5->scan().records_read,
        .revision_restarts = owned->ps5->scan().revision_restarts,
        .fetched_at = owned->ps5->fetched_at()};
  else if (owned)
    report.owned =
        FetchReport::OwnedGames{.state = owned->titles->ps5_ownership()};
  const auto on_ps5 = [](const MyGame& game) {
    return game.title().service() == ServiceGeneration::ps5_cloud;
  };
  if (const auto seen = owned ? held_view(user) : std::nullopt)
    report.titles = static_cast<std::size_t>(
        std::ranges::count_if(seen->games->visible(), on_ps5) +
        std::ranges::count_if(seen->games->hidden(), on_ps5));
  if (trusted_ != nullptr) report.trusted_time = trusted_->last_sync();
  if (const auto exclusions = held_.held_exclusions()) {
    const auto now = held_.now();
    report.exclusions =
        FetchReport::Exclusions{.fetched_at = exclusions->fetched_at(),
                                .age = now - exclusions->fetched_at(),
                                .fresh = exclusions->is_fresh_at(now),
                                .titles = exclusions->excluded().size()};
  }
  return report;
}

auto ListGames::category_held_for(const users::PsUser& user) -> CategoryPtr {
  return categories_.included_held_for(user);
}

auto ListGames::for_sale_held_for(const users::PsUser& user) -> ForSalePtr {
  return categories_.for_sale_held_for(user);
}

auto ListGames::matched(users::PsUser& user, const ListPtr& list,
                        const HeldAnswers::Owned& owned,
                        const CategoryPtr& category, const ForSalePtr& for_sale)
    -> GamesPtr {
  const auto account = user.account_id();
  const auto membership = user.membership();
  const auto tier = membership.tier();
  auto standing =
      tier ? held_.refusals().standing(account, tier)
           : held_.refusals().standing_while_membership_is_unknown(account);
  const auto remembered =
      standing ? std::move(standing.value()) : RefusalMemory::Standing{};
  MyGames::Inputs inputs{.library_fetched_at = owned.titles->fetched_at()};
  if (list) inputs.list_fetched_at = list->fetched_at();
  if (category) inputs.included_fetched_at = category->fetched_at();
  auto noted = held_.trials().used(account);
  const auto used = noted ? std::move(noted.value()) : TrialMemory::Used{};
  inputs.tier = tier;
  inputs.refusals_revision = remembered.revision;
  inputs.used_trials_revision = used.revision;
  inputs.library = owned.state;
  inputs.ps5_library = owned.ps5 ? owned.ps5->state()
                       : owned.state == LibraryState::fetched
                           ? owned.titles->ps5_ownership()
                           : LibraryState::unavailable;
  HeldAnswers::MatchedFrom sources{
      list,
      owned.titles,
      owned.ps5,
      category,
      users::CatalogCalls{user}.held_cloud_streaming(),
      for_sale};
  if (auto before = held_.matched_before(account, sources, inputs))
    return before;
  auto games = std::make_shared<const MyGames>(MyGamesAssembly::games_of(
      {.library = *owned.titles,
       .library_state = owned.state,
       .list = list.get(),
       .ps5 = owned.ps5.get(),
       .category = category.get(),
       .for_sale = for_sale.get(),
       .region = Asker::region_of(user),
       .used_trials = used.products,
       .used_trials_revision = used.revision},
      membership, sources.streaming, remembered.refusals, remembered.revision,
      held_.now()));
  if (!remembered.refusals.empty())
    (void)held_.refusals().forget_where_ownership_changed(
        account, [&games](const ProductId& refused) {
          return games->holding_of(refused);
        });
  held_.keep_matched(account, games, std::move(sources));
  return games;
}

auto ListGames::cloud_access(users::PsUser& user, const Stop& stop)
    -> Expected<CloudAccess, Unanswered> {
  if (auto refused = Asker::turned_away(user)) return unexpected(*refused);
  return HeldAnswers::answer(
      held_.in_flight().access, HeldAnswers::Question::by(user.account_id()),
      stop, [&] { return held_.fresh_access(user); },
      [&]() -> Expected<CloudAccess, Unanswered> {
        net::http::Client client{http_, clock_, stop, log_};
        ps::OldServiceShelves shelves{user, client, wall_};
        const auto walked = shelves.walk();
        if (!walked && walked.error().is_cancelled())
          return unexpected(walked.error());
        auto access = shelves.access_shown_by(walked);
        held_.note_access(user, access);
        return access;
      });
}

}  // namespace libchiaking::catalog::usecases
