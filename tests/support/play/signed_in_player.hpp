#pragma once

#include "src/core/psn/auth/credentials.hpp"
#include "tests/support/live_providers.hpp"
#include "tests/support/setup.hpp"
#include "tests/support/users/signed_in.hpp"

#include <libchiaking/testing/in_memory_credential_store.hpp>
#include <libchiaking/users.hpp>
#include <libchiaking/users_types.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

namespace test_support::play {

class SignedInPlayer {
 public:
  static constexpr std::string_view npsso = "SYNTHETIC_TOKEN";
  static constexpr std::string_view device =
      "0000000700410080000102030405060708090a0b0c0d0e0f";
  static constexpr std::string_view remote_play_bearer = "SYNTHETIC_TEST_TOKEN";
  static constexpr std::uint64_t account = 7;
  static constexpr std::uint64_t remote_play_account = 0x0807060504030201;

  SignedInPlayer() : SignedInPlayer(saved()) {}
  explicit SignedInPlayer(libchiaking::users::SavedSignIn saved)
      : user(signed_in.as(std::move(saved))) {}
  SignedInPlayer(libchiaking::platform::HttpProvider& http,
                 const libchiaking::platform::ClockProvider& clock,
                 const libchiaking::platform::ClockProvider& wall_clock,
                 libchiaking::users::SavedSignIn saved)
      : signed_in(http, clock, wall_clock),
        user(signed_in.as(std::move(saved))) {}

  static auto saved(std::string_view npsso_text = npsso,
                    std::string_view device_text = device,
                    libchiaking::AccountId as = libchiaking::AccountId{account})
      -> libchiaking::users::SavedSignIn {
    libchiaking::users::SavedSignIn saved{
        as, "player", libchiaking::psn::auth::Npsso{npsso_text}};
    saved.device = libchiaking::users::DeviceId::parse(device_text);
    Setup::require(saved.device.has_value(), "the device id parses");
    return saved;
  }

  static auto saved_for_remote_play(
      std::string_view bearer = remote_play_bearer,
      libchiaking::AccountId as = libchiaking::AccountId{remote_play_account})
      -> libchiaking::users::SavedSignIn {
    auto held = saved(npsso, device, as);
    held.remote_play.emplace(
        libchiaking::psn::auth::BearerToken{bearer},
        std::chrono::system_clock::time_point{
            std::chrono::microseconds{static_cast<std::int64_t>(
                LiveProviders::wall().now_microseconds())}} +
            std::chrono::hours{24});
    return held;
  }

  static auto fresh(
      libchiaking::users::SavedSignIn saved = SignedInPlayer::saved())
      -> std::shared_ptr<libchiaking::users::PsUser> {
    auto owner = std::make_shared<SignedInPlayer>(std::move(saved));
    return {owner, owner->user.get()};
  }

  static auto fresh_for_remote_play(
      std::string_view bearer = remote_play_bearer,
      libchiaking::AccountId as = libchiaking::AccountId{remote_play_account})
      -> std::shared_ptr<libchiaking::users::PsUser> {
    return fresh(saved_for_remote_play(bearer, as));
  }

  static auto admissible(libchiaking::users::SavedSignIn saved) -> bool {
    users::SignedIn fresh;
    Setup::require(static_cast<bool>(fresh.credentials.save(saved)),
                   "the credentials save");
    const auto returned = fresh.users.welcome_back();
    Setup::require(static_cast<bool>(returned), "the users come back");
    return returned.value().rejected == 0 && returned.value().users.size() == 1;
  }

  users::SignedIn signed_in;
  std::shared_ptr<libchiaking::users::PsUser> user;
};

}  // namespace test_support::play
