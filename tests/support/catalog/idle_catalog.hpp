#pragma once

#include "libchiaking/testing/service_builder.hpp"
#include "tests/support/setup.hpp"
#include "tests/support/users/signed_in.hpp"

#include <libchiaking/catalog.hpp>
#include <libchiaking/catalog_types.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/testing/in_memory_record_store.hpp>
#include <libchiaking/testing/manual_clock.hpp>

#include <string_view>
#include <utility>

namespace test_support::catalog {

class IdleCatalog {
 public:
  static constexpr std::string_view ps5_title =
      "EP0109-PPSA07033_00-FAKE0030TITLE000";
  static constexpr std::string_view old_service_title =
      "EP0000-CUSA12345_00-TITLE";

  IdleCatalog() = default;
  IdleCatalog(const IdleCatalog&) = delete;
  auto operator=(const IdleCatalog&) -> IdleCatalog& = delete;

  static auto shared() -> libchiaking::catalog::Catalog& {
    static IdleCatalog idle;
    return idle.catalog;
  }

  static auto title(std::string_view id = ps5_title)
      -> libchiaking::catalog::ProductId {
    return Setup::value(libchiaking::catalog::ProductId::parse(id),
                        "the product id parses");
  }

 private:
  users::Offline offline_;
  libchiaking::platform::DefaultThreadingProvider threading_;
  libchiaking::testing::InMemoryRecordStore records_{threading_};
  libchiaking::testing::ManualClock clock_;
  libchiaking::testing::ManualClock wall_;
  libchiaking::users::RequestBudget budget_{threading_, clock_};

 public:
  libchiaking::catalog::Catalog catalog{
      libchiaking::testing::ServiceBuilder::build<
          libchiaking::catalog::Catalog>(offline_, records_, threading_, clock_,
                                         wall_, budget_)};
};

}  // namespace test_support::catalog
