#include "src/core/catalog/usecases/show_details.hpp"

#include "src/core/catalog/adapters/ps/store_details.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace libchiaking::catalog::usecases {

ShowDetails::ShowDetails(platform::HttpProvider& http,
                         platform::ThreadingProvider& threading,
                         const platform::ClockProvider& clock,
                         const platform::ClockProvider& wall_clock,
                         platform::DiagnosticsProvider* log, HeldAnswers& held,
                         ListGames& list_games)
    : http_(http),
      threading_(threading),
      clock_(clock),
      wall_(wall_clock),
      log_(log),
      held_(held),
      list_games_(list_games) {}

auto ShowDetails::concept_of(const ListGames::View& seen, const Library* owned,
                             const ProductId& product)
    -> std::optional<ConceptId> {
  const auto holds = [&product](const MyGame& game) {
    const auto& ownership = game.ownership();
    return ownership && (ownership->product_id() == product ||
                         ownership->id().as_product() == product);
  };
  const auto* game = seen.games->find(product);
  for (const auto shelf : {seen.games->visible(), seen.games->hidden()}) {
    if (game != nullptr) break;
    if (const auto held = std::ranges::find_if(shelf, holds);
        held != shelf.end())
      game = &*held;
  }
  if (game != nullptr && game->title().concept_id())
    return game->title().concept_id();
  const auto* list = seen.list ? seen.list.value().get() : nullptr;
  if (const auto* listed = list != nullptr ? list->find(product) : nullptr;
      listed != nullptr && listed->title.concept_id())
    return listed->title.concept_id();
  return owned != nullptr ? owned->concept_of_copy(product) : std::nullopt;
}

auto ShowDetails::locale_of(const users::PsUser& user,
                            const std::optional<ListGames::View>& seen)
    -> StoreLocale {
  if (seen && seen->list && seen->list.value())
    return seen->list.value()->settled_locale();
  return Asker::region_of(user).value_or(StoreLocale::english_us());
}

auto ShowDetails::sold_as(const StoreLocale& locale, int age,
                          const ProductId& product, const Stop& stop)
    -> Expected<GameDetails, Unanswered> {
  return HeldAnswers::answer(
      held_.in_flight().details,
      HeldAnswers::Question::sold_as(locale, age, product), stop,
      [&] { return held_.fresh_details(locale, product, std::nullopt); },
      [&]() -> Expected<GameDetails, Unanswered> {
        net::http::Client client{http_, clock_, stop, log_};
        auto found = ps::StoreDetails{client, threading_, wall_}.of_product(
            product, locale, age);
        if (found) held_.keep(found.value());
        return found;
      });
}

auto ShowDetails::grouped_as(const StoreLocale& locale,
                             const ConceptId& grouped_under, const Stop& stop)
    -> Expected<GameDetails, Unanswered> {
  return HeldAnswers::answer(
      held_.in_flight().details,
      HeldAnswers::Question::grouped_as(locale, grouped_under), stop,
      [&] { return held_.fresh_details(locale, std::nullopt, grouped_under); },
      [&]() -> Expected<GameDetails, Unanswered> {
        net::http::Client client{http_, clock_, stop, log_};
        auto found = ps::StoreDetails{client, threading_, wall_}.of_concept(
            grouped_under, locale);
        if (!found) return unexpected(found.error());
        if (found.value().whole) held_.keep(found.value().details);
        return std::move(found.value().details);
      });
}

auto ShowDetails::details(users::PsUser& user, const ProductId& product,
                          const Stop& stop)
    -> Expected<GameDetails, Unanswered> {
  if (auto refused = Asker::turned_away(user)) return unexpected(*refused);
  const auto seen = list_games_.held_view(user);
  const auto locale = locale_of(user, seen);
  const auto age = user.account_context().age();
  auto sold = age ? sold_as(locale, *age, product, stop)
                  : Expected<GameDetails, Unanswered>{
                        unexpected(Unanswered{Unanswered::Reason::not_listed})};
  if (sold || !seen || sold.error().reason() != Unanswered::Reason::not_listed)
    return sold;
  const auto owned = held_.held_by(user.account_id());
  const auto grouped_under =
      concept_of(*seen, owned ? owned->titles.get() : nullptr, product);
  if (!grouped_under) return sold;
  auto grouped = grouped_as(locale, *grouped_under, stop);
  if (grouped) grouped.value().product = product;
  return grouped;
}

auto ShowDetails::details(users::PsUser& user, const ConceptId& grouped_under,
                          const Stop& stop)
    -> Expected<GameDetails, Unanswered> {
  const auto region = Asker::region_of(user);
  const auto listed = region ? held_.held_list(*region) : nullptr;
  return grouped_as(listed ? listed->settled_locale()
                           : region.value_or(StoreLocale::english_us()),
                    grouped_under, stop);
}

}  // namespace libchiaking::catalog::usecases
