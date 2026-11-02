#include "src/core/catalog/usecases/read_categories.hpp"

#include "src/core/catalog/adapters/ps/streaming_category.hpp"
#include "src/core/catalog/usecases/asker.hpp"

#include <string>
#include <utility>

namespace libchiaking::catalog::usecases {

ReadCategories::ReadCategories(platform::HttpProvider& http,
                               const platform::ClockProvider& clock,
                               const platform::ClockProvider& wall_clock,
                               platform::DiagnosticsProvider* log,
                               HeldAnswers& held) noexcept
    : http_(http), clock_(clock), wall_(wall_clock), log_(log), held_(held) {}

auto ReadCategories::included_held_for(const users::PsUser& user)
    -> CategoryPtr {
  const auto audience = Asker::audience_of(user);
  return audience
             ? held_.held_included(audience->locale, audience->age).category
             : nullptr;
}

auto ReadCategories::for_sale_held_for(const users::PsUser& user)
    -> ForSalePtr {
  const auto audience = Asker::audience_of(user);
  return audience
             ? held_.held_for_sale(audience->locale, audience->age).category
             : nullptr;
}

auto ReadCategories::included(const users::PsUser& user, Demand demand,
                              const Stop& stop, const FetchProgress& progress)
    -> Expected<CategoryPtr, Unanswered> {
  const auto audience = Asker::audience_of(user);
  if (!audience) {
    warn_of_an_unknown_audience("premium streaming category");
    return CategoryPtr{};
  }
  const auto& [locale, age] = *audience;
  return held_or_fetched(
      held_.in_flight().included,
      HeldAnswers::Question::included_in(locale, age), demand, stop,
      [&] { return held_.held_included(locale, age); },
      [&](net::http::Client& client, PageCount pages) {
        return ps::StreamingCategory{client, wall_}.included(locale, age,
                                                             std::move(pages));
      },
      [&](HeldAnswers::Included learned) {
        return held_.keep(locale, age, std::move(learned));
      },
      progress);
}

auto ReadCategories::for_sale(const users::PsUser& user, Demand demand,
                              const Stop& stop, const FetchProgress& progress)
    -> Expected<ForSalePtr, Unanswered> {
  const auto audience = Asker::audience_of(user);
  if (!audience) {
    warn_of_an_unknown_audience("store streaming category");
    return ForSalePtr{};
  }
  const auto& [locale, age] = *audience;
  return held_or_fetched(
      held_.in_flight().for_sale,
      HeldAnswers::Question::for_sale_in(locale, age), demand, stop,
      [&] { return held_.held_for_sale(locale, age); },
      [&](net::http::Client& client, PageCount pages) {
        return ps::StreamingCategory{client, wall_}.for_sale(locale, age,
                                                             std::move(pages));
      },
      [&](HeldAnswers::ForSale learned) {
        return held_.keep(locale, age, std::move(learned));
      },
      progress);
}

template <class Category, class Held, class Fetch, class Keep>
auto ReadCategories::held_or_fetched(
    concurrency::SharedFlight<std::shared_ptr<const Category>, Unanswered>&
        flight,
    const HeldAnswers::Question& question, Demand demand, const Stop& stop,
    const Held& held, const Fetch& fetch, const Keep& keep,
    const FetchProgress& progress)
    -> Expected<std::shared_ptr<const Category>, Unanswered> {
  using Kept = std::shared_ptr<const Category>;
  return flight.answer(
      question.text(), stop, [&]() -> Expected<Kept, Unanswered> {
        auto known = held();
        if (known.record->next_step(held_.now(), demand) !=
            cache::category::Step::fetch)
          return known.category;
        net::http::Client client{http_, clock_, stop, log_};
        progress.doing(FetchActivity::reading_category_pages);
        auto category = fetch(client, progress.pages());
        if (!category && category.error().is_cancelled())
          return unexpected(category.error());
        const auto now = held_.now();
        auto kept = keep(HeldAnswers::held_from(
            category ? known.record->with_answer(category.value(), now)
                     : known.record->with_failure(now)));
        if (!category && !kept) return unexpected(category.error());
        return kept;
      });
}

void ReadCategories::warn_of_an_unknown_audience(
    std::string_view category) const {
  if (log_ == nullptr) return;
  const auto line =
      "[CLOUD_STREAM] the account's country, language or age is not known; "
      "the " +
      std::string{category} + " is not asked for";
  log_->write({platform::LogSeverity::warning, line});
}

}  // namespace libchiaking::catalog::usecases
