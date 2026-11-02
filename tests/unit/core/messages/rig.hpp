#pragma once

#include "libchiaking/testing/service_builder.hpp"
#include "src/core/psn/rest/gaming_lounge/lounge_groups.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/psn/fakes.hpp"
#include "tests/support/users/signed_in.hpp"

#include <libchiaking/messages.hpp>
#include <libchiaking/users.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace messages_test {

class Rig {
 public:
  using ChatMessage = libchiaking::messages::ChatMessage;
  using Messages = libchiaking::messages::Messages;
  using Reason = libchiaking::users::Unanswered::Reason;
  using Reply = test_support::psn::Reply;

  static constexpr std::string_view base =
      "https://m.np.playstation.com/api/gamingLoungeGroups";
  static constexpr std::string_view token_url =
      "https://ca.account.sony.com/api/authz/v3/oauth/token";
  static constexpr std::string_view app_token = "SYNTHETIC_APP_TOKEN_1";
  static constexpr std::string_view renewed_token = "SYNTHETIC_APP_TOKEN_2";
  static constexpr std::uint64_t wall_start_us = 1'900'000'000'000'000;
  static constexpr std::size_t page_size =
      libchiaking::psn::rest::LoungeGroups::groups_page_size;

  Rig() {
    wall.now_us = wall_start_us;
    libchiaking::users::SavedSignIn saved{libchiaking::AccountId{7}, "player",
                                          libchiaking::psn::auth::Npsso{"np"}};
    saved.store.emplace(
        libchiaking::psn::auth::AppToken{app_token},
        std::chrono::system_clock::time_point{
            std::chrono::microseconds{wall_start_us} + std::chrono::hours{72}});
    saved.store_refresh.emplace(
        libchiaking::psn::auth::AppRefreshToken{"refresh-7"});
    user = signed_in.as(std::move(saved));
  }

  static auto groups_url(std::string_view filter, std::size_t offset = 0)
      -> std::string {
    return std::string{base} +
           "/v1/members/me/groups?includeFields=groupName,groupIcon,members,"
           "mainThread,joinedTimestamp,modifiedTimestamp,totalGroupCount,"
           "isFavorite,existsNewArrival,notificationSetting&favoriteFilter=" +
           std::string{filter} + "&limit=200&offset=" + std::to_string(offset);
  }

  static auto group_entry(std::string_view id) -> std::string {
    return R"({"groupId":")" + std::string{id} +
           R"(","groupType":0,"mainThread":{"threadId":"t"}})";
  }

  static auto thread_url(std::string_view owner) -> std::string {
    return std::string{base} + std::string{owner} + "g-1/threads/t-1";
  }

  static auto messages_url() -> std::string {
    return thread_url("/v1/members/me/groups/") +
           "/messages?sharedTypes=profile,party&includeFields=sharedUrlInfo";
  }

  static auto send_url() -> std::string {
    return thread_url("/v1/groups/") + "/messages";
  }

  static auto at(std::int64_t milliseconds)
      -> libchiaking::messages::MessageTime {
    return libchiaking::messages::MessageTime{
        std::chrono::milliseconds{milliseconds}};
  }

  static auto bearer(std::string_view token) -> std::string {
    return "Bearer " + std::string{token};
  }

  void renews_store_access() {
    http.on("POST", std::string{token_url},
            {Reply{200, R"({"access_token":")" + std::string{renewed_token} +
                            R"(","expires_in":2000})"}});
  }

  test_support::FakeClock clock;
  test_support::FakeClock wall;
  test_support::psn::FakeHttp http;
  test_support::users::SignedIn signed_in{http, clock, wall};
  Messages messages{
      libchiaking::testing::ServiceBuilder::build<Messages>(http, clock, wall)};
  std::shared_ptr<libchiaking::users::PsUser> user;
};

}  // namespace messages_test
