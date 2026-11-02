#pragma once

#include "libchiaking/platform/net/default.hpp"
#include "libchiaking/testing/service_builder.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "tests/support/crypto/refusing.hpp"
#include "tests/support/psn/fakes.hpp"
#include "tests/unit/core/consoles/known_consoles_rig.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace consoles_test {

class Account {
 public:
  static constexpr std::string_view store_token_url =
      "https://ca.account.sony.com/api/authz/v3/oauth/token";
  static constexpr std::string_view remote_play_token_url =
      "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token";

  explicit Account(test_support::FakeClock* clock = nullptr)
      : http(clock),
        signed_in(http),
        consoles(libchiaking::testing::ServiceBuilder::build<
                 libchiaking::consoles::KnownConsoles>(network, http, crypto,
                                                       random, store, threading,
                                                       steady, nullptr, &log)),
        facts(consoles) {}

  auto user(std::uint64_t account, std::string_view store_token,
            std::string_view remote_play_token = {})
      -> std::shared_ptr<libchiaking::users::PsUser> {
    const auto expires_at =
        std::chrono::system_clock::time_point{
            std::chrono::microseconds{wall.now_microseconds()}} +
        std::chrono::hours{1};
    libchiaking::users::SavedSignIn saved{
        libchiaking::AccountId{account}, "player",
        libchiaking::psn::auth::Npsso{"npsso-" + std::to_string(account)}};
    saved.store.emplace(libchiaking::psn::auth::AppToken{store_token},
                        expires_at);
    saved.store_refresh.emplace(libchiaking::psn::auth::AppRefreshToken{
        "refresh-" + std::to_string(account)});
    if (!remote_play_token.empty()) {
      saved.remote_play.emplace(
          libchiaking::psn::auth::BearerToken{remote_play_token}, expires_at);
      saved.remote_play_refresh.emplace(
          libchiaking::psn::auth::RemotePlayRefreshToken{
              "remote-refresh-" + std::to_string(account)});
    }
    return signed_in.as(std::move(saved));
  }

  auto home_user(std::uint64_t account)
      -> std::shared_ptr<libchiaking::users::PsUser> {
    return signed_in.users
        .add_home_user(libchiaking::AccountId{account}, "Home Player")
        .value();
  }

  auto paired(const libchiaking::users::PsUser& as, std::string_view id,
              std::optional<std::string_view> nickname,
              libchiaking::consoles::Firmware::Family family =
                  libchiaking::consoles::Firmware::Family::ps5_1,
              std::optional<libchiaking::consoles::DeviceUid> device_uid = {})
      -> std::shared_ptr<libchiaking::consoles::Console> {
    std::optional<libchiaking::consoles::ConsoleName> named;
    if (nickname)
      named = Values::name(*nickname,
                           libchiaking::consoles::ConsoleName::Source::pairing);
    return facts
        .paired(as,
                libchiaking::consoles::PairedConsole{
                    Values::hardware_id(id),
                    libchiaking::consoles::Firmware{family}, std::move(named),
                    Values::keys(1)},
                std::nullopt, device_uid)
        .value();
  }

  void renews_store_access(std::string_view token) {
    http.on("POST", std::string{store_token_url},
            {test_support::psn::Reply{
                200,
                R"({"access_token":")" + std::string{token} +
                    R"(","refresh_token":"next-refresh","expires_in":2000})"}});
  }

  void renews_remote_play_access(std::string_view token) {
    http.on(
        "POST", std::string{remote_play_token_url},
        {test_support::psn::Reply{
            200,
            R"({"access_token":")" + std::string{token} +
                R"(","refresh_token":"next-remote","expires_in":1000,"token_type":"Bearer"})"}});
  }

  libchiaking::platform::DefaultThreadingProvider threading;
  libchiaking::platform::SteadyClock steady;
  libchiaking::platform::SystemClock wall;
  test_support::psn::FakeHttp http;
  test_support::FakeLog log;
  test_support::users::SignedIn signed_in;
  libchiaking::platform::DefaultNetworkProvider network{threading, steady};
  test_support::NoCrypto crypto;
  libchiaking::platform::SystemSecureRandom random;
  libchiaking::testing::InMemoryConsoleStore store{threading};
  libchiaking::consoles::KnownConsoles consoles;
  libchiaking::consoles::ConsoleFacts facts;
};

}  // namespace consoles_test
