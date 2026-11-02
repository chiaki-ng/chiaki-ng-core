#pragma once

#include "src/core/trophies/trophies_core.hpp"
#include "src/core/utils/json/ps_fields.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/psn/fakes.hpp"
#include "tests/support/users/signed_in.hpp"

#include <libchiaking/testing/in_memory_record_store.hpp>
#include <libchiaking/trophies.hpp>
#include <libchiaking/users.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace trophies_test {

class Rig {
 public:
  using Records = libchiaking::testing::InMemoryRecordStore;
  using Reply = test_support::psn::Reply;

  static constexpr std::string_view base =
      "https://m.np.playstation.com/api/trophy/v1";
  static constexpr std::string_view token_url =
      "https://ca.account.sony.com/api/authz/v3/oauth/token";
  static constexpr std::string_view app_token = "SYNTHETIC_APP_TOKEN_1";
  static constexpr std::string_view renewed_token = "SYNTHETIC_APP_TOKEN_2";
  static constexpr std::uint64_t wall_start_us = 1'900'000'000'000'000;
  static constexpr std::string_view summary_body = R"({
  "accountId": "1234567890123456789",
  "trophyLevel": 312,
  "progress": 46,
  "tier": 4,
  "earnedTrophies": {"bronze": 2104, "silver": 611, "gold": 158, "platinum": 21}
})";

  Rig() : Rig(fresh_records()) {}

  explicit Rig(std::shared_ptr<Records> kept) : records(std::move(kept)) {
    wall.now_us = wall_start_us;
    core = std::make_unique<libchiaking::trophies::Trophies::Core>(
        http, *records, test_support::process_threading(), clock, wall, nullptr,
        waiter.factory(), budget);
    user = signed_in.as(saved());
  }

  static auto fresh_records() -> std::shared_ptr<Records> {
    return std::make_shared<Records>(test_support::process_threading());
  }

  static auto keeps(Records& store, std::string_view name) -> bool {
    return std::ranges::any_of(store.names(), [name](const auto& held) {
      return held.text() == name;
    });
  }

  static auto at(std::string_view text) -> libchiaking::trophies::Time {
    return libchiaking::json::ps::Instant::from_text(text).value().time();
  }

  static auto store_sign_in(std::string_view token)
      -> libchiaking::users::SavedSignIn {
    libchiaking::users::SavedSignIn saved{libchiaking::AccountId{7}, "player",
                                          libchiaking::psn::auth::Npsso{"np"}};
    saved.store.emplace(
        libchiaking::psn::auth::AppToken{token},
        std::chrono::system_clock::time_point{
            std::chrono::microseconds{wall_start_us} + std::chrono::hours{72}});
    return saved;
  }

  static auto summary_url() -> std::string {
    return std::string{base} + "/users/me/trophySummary";
  }

  static auto titles_url(std::size_t offset) -> std::string {
    return std::string{base} +
           "/users/me/trophyTitles?limit=100&offset=" + std::to_string(offset);
  }

  static auto set_url(std::string_view owner, std::string_view id,
                      std::string_view leaf, std::string_view service,
                      std::optional<std::size_t> offset = {}) -> std::string {
    std::string url = std::string{base} + std::string{owner} +
                      "/npCommunicationIds/" + std::string{id} +
                      std::string{leaf};
    if (!service.empty()) url += "?npServiceName=" + std::string{service};
    if (offset)
      url += std::string{service.empty() ? "?" : "&"} +
             "limit=100&offset=" + std::to_string(*offset);
    return url;
  }

  auto saved() const -> libchiaking::users::SavedSignIn {
    auto saved = store_sign_in(app_token);
    saved.store_refresh.emplace(
        libchiaking::psn::auth::AppRefreshToken{"refresh-7"});
    return saved;
  }

  auto read() -> libchiaking::trophies::usecases::ReadTrophies& {
    return core->read();
  }

  void renews_store_access() {
    http.on("POST", std::string{token_url},
            {Reply{200, R"({"access_token":")" + std::string{renewed_token} +
                            R"(","refresh_token":"next","expires_in":2000})"}});
  }

  void advance_wall(std::chrono::milliseconds by) {
    wall.now_us += static_cast<std::uint64_t>(by.count()) * 1000U;
  }

  auto requests_to(std::string_view url) const -> int {
    return http.requests_to(url);
  }

  auto keeps(std::string_view name) const -> bool {
    return keeps(*records, name);
  }

  test_support::FakeClock clock;
  test_support::FakeClock wall;
  test_support::psn::FakeHttp http;
  std::shared_ptr<Records> records;
  test_support::users::SignedIn signed_in{http, clock, wall};
  test_support::FakeWaiter waiter{&clock};
  libchiaking::users::RequestBudget budget{
      std::make_unique<libchiaking::users::RequestBudget::Core>(
          test_support::process_threading(), clock, waiter.factory())};
  std::unique_ptr<libchiaking::trophies::Trophies::Core> core;
  std::shared_ptr<libchiaking::users::PsUser> user;
};

}  // namespace trophies_test
