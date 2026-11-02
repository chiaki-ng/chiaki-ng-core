#pragma once

#include "src/core/catalog/adapters/ps/old_service_shelves.hpp"
#include "src/core/catalog/adapters/ps/ps5_store_lists.hpp"
#include "src/core/catalog/adapters/ps/public_store_walk.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/catalog/adapters/ps/rig.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace catalog_test {

class StoreWalkRig : public PsRig {
 public:
  using Walked = libchiaking::Expected<libchiaking::catalog::RawListing,
                                       libchiaking::catalog::Unanswered>;

  static constexpr std::string_view ps5_game =
      R"({"productId":"EP-PPSA10001_00-GAME","name":"Comet","conceptId":1,"device":["PS5"],"streamingSupported":true})";

  static auto store_list(std::string_view game) -> std::string {
    return "[{\"games\":[" + std::string{game} + "]}]";
  }

  static auto listed_times(const libchiaking::catalog::RawListing& listing,
                           std::string_view product) -> std::ptrdiff_t {
    return std::ranges::count_if(
        listing.entries(),
        [product](const libchiaking::catalog::ListedEntry& entry) {
          return entry.product == product;
        });
  }

  void session_opens(std::string_view country = "GB",
                     std::string_view language = "en") {
    http.on("GET", "response_type=code",
            {{302, "",
              location("https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/"
                       "grc-response.html?code=CODE1")}});
    http.on("POST", "/user/session",
            {{200,
              R"({"header":{"status_code":"0x0000"},"data":{"country":")" +
                  std::string{country} + R"(","language":")" +
                  std::string{language} + R"("}})",
              {{"Set-Cookie", "JSESSIONID=JS1; Path=/"}}}});
  }

  void old_service_ok(
      std::string_view shelf_body =
          R"({"links":[{"id":"EP-CUSA00001_00-LEGACY_CLOUD_STREAM","name":"Classic"}]})") {
    session_opens();
    http.on(
        "GET", "/user/stores",
        {{200,
          R"({"header":{"status_code":"0x0000"},"data":{"base_url":"https://psnow.playstation.com/store/container/GB/en/19/ROOT"}})"}});
    http.on(
        "GET", "ROOT?size=100",
        {{200,
          R"({"links":[{"name":"A - B","url":"https://psnow.playstation.com/store/cat/AB"},{"name":"Featured","url":"https://x/f"}]})"}});
    http.on("GET", "cat/AB?start=0&size=500", {{200, std::string{shelf_body}}});
    http.on("GET", "x/f?start=0&size=500", {{200, R"({"links":[]})"}});
  }

  void region_unsupported(std::string_view country = "GB",
                          std::string_view language = "en") {
    session_opens(country, language);
    http.on("GET", "/user/stores", {{404, "{}"}});
  }

  void store_lists_ok(bool all_ps5_ok = true) {
    http.on("GET", "categoryList=all-ps5-list",
            {{all_ps5_ok ? 200 : 500, store_list(ps5_game)}});
    http.on("GET", "imagic/gameslist", {{200, "[]"}});
  }

  auto walked(libchiaking::users::PsUser& user) -> Walked {
    return libchiaking::catalog::ps::OldServiceShelves{user, client, wall,
                                                       1024 * 1024}
        .walk();
  }

  auto availability_shown(libchiaking::users::PsUser& user,
                          const Walked& result)
      -> libchiaking::catalog::CloudAccess::Availability {
    return libchiaking::catalog::ps::OldServiceShelves{user, client, wall}
        .access_shown_by(result)
        .availability();
  }

  auto fallback(std::string_view region) -> Walked {
    return libchiaking::catalog::ps::PublicStoreWalk{client, wall}.walk(
        locale(region));
  }

  auto ps5_lists(std::string_view region) -> Walked {
    return libchiaking::catalog::ps::Ps5StoreLists{client, wall}.fetch(
        locale(region));
  }
};

}  // namespace catalog_test
