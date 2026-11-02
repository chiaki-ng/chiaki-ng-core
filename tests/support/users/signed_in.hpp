#pragma once

#include "libchiaking/testing/service_builder.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/setup.hpp"

#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/random_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/testing/in_memory_credential_store.hpp>
#include <libchiaking/users.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace test_support::users {

class Offline final : public libchiaking::platform::HttpProvider {
 public:
  [[nodiscard]] auto perform(const libchiaking::platform::HttpRequest&,
                             const libchiaking::platform::StopToken&)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    ++requests;
    return libchiaking::unexpected(
        libchiaking::platform::HttpError::disconnected);
  }

  int requests{};
};

using SystemWall = libchiaking::platform::SystemClock;

class PatternedRandom final
    : public libchiaking::platform::SecureRandomProvider {
 public:
  [[nodiscard]] auto fill_bytes(std::span<std::byte> output)
      -> libchiaking::Expected<void,
                               libchiaking::platform::PlatformError> override {
    ++fills;
    if (failing)
      return libchiaking::unexpected(
          libchiaking::platform::PlatformError::system_failure);
    std::ranges::fill(output, std::byte{0xab});
    return {};
  }

  static constexpr std::string_view device =
      "0000000700410080abababababababababababababababab";

  int fills{};
  bool failing{};
};

class SignedIn {
 public:
  SignedIn()
      : users(libchiaking::testing::ServiceBuilder::build<
              libchiaking::users::SignedInUsers>(
            offline, credentials, threading, steady, wall, random)) {}
  explicit SignedIn(libchiaking::platform::HttpProvider& http)
      : users(libchiaking::testing::ServiceBuilder::build<
              libchiaking::users::SignedInUsers>(http, credentials, threading,
                                                 steady, wall, random)) {}
  SignedIn(libchiaking::platform::HttpProvider& http,
           const libchiaking::platform::ClockProvider& clock,
           const libchiaking::platform::ClockProvider& wall_clock)
      : users(libchiaking::testing::ServiceBuilder::build<
              libchiaking::users::SignedInUsers>(http, credentials, threading,
                                                 clock, wall_clock, random)) {}
  SignedIn(libchiaking::platform::HttpProvider& http,
           libchiaking::platform::ThreadingProvider& provider,
           const libchiaking::platform::ClockProvider& clock,
           const libchiaking::platform::ClockProvider& wall_clock)
      : threading(provider),
        users(libchiaking::testing::ServiceBuilder::build<
              libchiaking::users::SignedInUsers>(http, credentials, threading,
                                                 clock, wall_clock, random)) {}

  auto as(libchiaking::users::SavedSignIn saved)
      -> std::shared_ptr<libchiaking::users::PsUser> {
    const auto account = saved.account;
    Setup::require(static_cast<bool>(credentials.save(saved)),
                   "the credentials save");
    Setup::require(static_cast<bool>(users.welcome_back()),
                   "the users come back");
    auto user = users.find(account);
    Setup::require(user != nullptr, "the signed-in user is found");
    return user;
  }

  auto as(std::string_view npsso, std::uint64_t account = 1)
      -> std::shared_ptr<libchiaking::users::PsUser> {
    return as(libchiaking::users::SavedSignIn{
        libchiaking::AccountId{account}, "player",
        libchiaking::psn::auth::Npsso{npsso}});
  }

  auto as(std::string_view npsso, std::string_view store_token,
          std::chrono::system_clock::time_point expires_at,
          std::uint64_t account = 1)
      -> std::shared_ptr<libchiaking::users::PsUser> {
    libchiaking::users::SavedSignIn saved{libchiaking::AccountId{account},
                                          "player",
                                          libchiaking::psn::auth::Npsso{npsso}};
    saved.store.emplace(libchiaking::psn::auth::AppToken{store_token},
                        expires_at);
    return as(std::move(saved));
  }

  libchiaking::platform::ThreadingProvider& threading{
      test_support::process_threading()};
  libchiaking::platform::SteadyClock steady;
  SystemWall wall;
  Offline offline;
  PatternedRandom random;
  libchiaking::testing::InMemoryCredentialStore credentials{threading};
  libchiaking::users::SignedInUsers users;
};

}  // namespace test_support::users
