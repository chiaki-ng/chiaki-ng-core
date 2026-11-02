#include "src/core/catalog/adapters/cache/flight_question.hpp"

#include <format>

namespace libchiaking::catalog::cache {

auto FlightQuestion::by(AccountId account) -> FlightQuestion {
  return FlightQuestion{std::format("account/{}", account.value())};
}

auto FlightQuestion::by(AccountId account, const ProductId& product)
    -> FlightQuestion {
  return FlightQuestion{
      std::format("account/{}/{}", account.value(), product.text())};
}

auto FlightQuestion::listed_in(const StoreLocale& region) -> FlightQuestion {
  return FlightQuestion{std::format("region/{}", region.lowercase())};
}

auto FlightQuestion::included_in(const StoreLocale& locale, int age)
    -> FlightQuestion {
  return FlightQuestion{std::format("{}/age-{}", locale.lowercase(), age)};
}

auto FlightQuestion::for_sale_in(const StoreLocale& locale, int age)
    -> FlightQuestion {
  return FlightQuestion{
      std::format("for-sale/{}/age-{}", locale.lowercase(), age)};
}

auto FlightQuestion::sold_as(const StoreLocale& locale, int age,
                             const ProductId& product) -> FlightQuestion {
  return FlightQuestion{std::format("details/{}/{}/product/{}",
                                    locale.lowercase(), age, product.text())};
}

auto FlightQuestion::grouped_as(const StoreLocale& locale,
                                const ConceptId& grouped_under)
    -> FlightQuestion {
  return FlightQuestion{std::format("details/{}/concept/{}", locale.lowercase(),
                                    grouped_under.text())};
}

auto FlightQuestion::showing(const ImageRef& image) -> FlightQuestion {
  return FlightQuestion{image.url()};
}

}  // namespace libchiaking::catalog::cache
