#include "src/core/catalog/usecases/grouped_details.hpp"

#include "src/core/catalog/adapters/ps/store_details.hpp"
#include "src/core/catalog/usecases/asker.hpp"

#include <utility>

namespace libchiaking::catalog::usecases {

GroupedDetails::GroupedDetails(platform::HttpProvider& http,
                               platform::ThreadingProvider& threading,
                               const platform::ClockProvider& clock,
                               const platform::ClockProvider& wall_clock,
                               platform::DiagnosticsProvider* log,
                               HeldAnswers& held)
    : http_(http),
      threading_(threading),
      clock_(clock),
      wall_(wall_clock),
      log_(log),
      held_(held) {}

auto GroupedDetails::details(const StoreLocale& locale,
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

auto GroupedDetails::details(const users::PsUser& user,
                             const ConceptId& grouped_under, const Stop& stop)
    -> Expected<GameDetails, Unanswered> {
  const auto region = Asker::region_of(user);
  const auto listed = region ? held_.held_list(*region) : nullptr;
  return details(listed ? listed->settled_locale()
                        : region.value_or(StoreLocale::english_us()),
                 grouped_under, stop);
}

}  // namespace libchiaking::catalog::usecases
