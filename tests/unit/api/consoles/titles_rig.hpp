#pragma once

#include "tests/support/api/fakes.hpp"
#include "tests/support/consoles/paired_consoles.hpp"
#include "tests/support/crypto/refusing.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"

#include "libchiaking/consoles.hpp"
#include "libchiaking/platform/net/default.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "libchiaking/testing/in_memory_console_store.hpp"
#include "libchiaking/testing/in_memory_credential_store.hpp"
#include "libchiaking/testing/service_builder.hpp"
#include "libchiaking/users.hpp"
#include "src/core/psn/auth/credentials.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace api_consoles_test {

class ScriptedHttp final : public libchiaking::platform::HttpProvider {
 public:
  struct Sent {
    std::string method;
    std::string url;
    std::string authorization;
    std::string body;
  };

  void on(std::string method, std::string url, std::uint16_t status,
          std::string body) {
    rules_.push_back(
        {std::move(method), std::move(url), status, std::move(body)});
  }

  [[nodiscard]] auto perform(const libchiaking::platform::HttpRequest& request,
                             const libchiaking::platform::StopToken& stop)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    Sent sent{libchiaking::platform::http_method_name(request.method),
              std::string{request.url.view()},
              {},
              std::string{request.body.view()}};
    for (const auto& header : request.headers)
      if (header.name == "Authorization")
        sent.authorization = std::string{header.value.view()};
    seen.push_back(sent);
    if (stop.stop_requested())
      return libchiaking::unexpected(
          libchiaking::platform::HttpError::cancelled);
    for (const auto& rule : rules_) {
      if (rule.method != sent.method || rule.url != sent.url) continue;
      libchiaking::platform::HttpResponse response;
      response.status_code = rule.status;
      response.body.assign(rule.body);
      return response;
    }
    return libchiaking::unexpected(
        libchiaking::platform::HttpError::disconnected);
  }

  std::vector<Sent> seen;

 private:
  struct Rule {
    std::string method;
    std::string url;
    std::uint16_t status{};
    std::string body;
  };

  std::vector<Rule> rules_;
};

class TitlesRig {
 public:
  static constexpr std::string_view token = "fake-console-token-55ee";

  auto signed_in(std::uint64_t account)
      -> std::shared_ptr<libchiaking::users::PsUser> {
    libchiaking::users::SavedSignIn saved{
        libchiaking::AccountId{account}, "player",
        libchiaking::psn::auth::Npsso{"fake-npsso-18d3"}};
    saved.store.emplace(
        libchiaking::psn::auth::AppToken{token},
        std::chrono::system_clock::time_point{
            std::chrono::microseconds{wall.now_microseconds()}} +
            std::chrono::hours{1});
    EXPECT_TRUE(credentials.save(saved));
    EXPECT_TRUE(everyone.welcome_back());
    auto user = everyone.find(libchiaking::AccountId{account});
    if (!user) throw std::logic_error{"the signed-in user was not welcomed"};
    return user;
  }

  auto home(std::uint64_t account)
      -> std::shared_ptr<libchiaking::users::PsUser> {
    return everyone
        .add_home_user(libchiaking::AccountId{account}, "Home Player")
        .value();
  }

  auto paired(std::string_view id,
              libchiaking::consoles::Firmware::Family family,
              std::uint64_t account)
      -> std::shared_ptr<libchiaking::consoles::Console> {
    const auto hardware_id =
        libchiaking::consoles::HardwareId::parse(id).value();
    std::array<std::byte,
               libchiaking::consoles::PairingKeys::registration_key_size>
        registration{};
    std::array<std::byte,
               libchiaking::consoles::PairingKeys::remote_play_key_size>
        remote_play{};
    registration.fill(std::byte{1});
    remote_play.fill(std::byte{2});
    auto keys =
        libchiaking::consoles::PairingKeys::create(registration, remote_play, 2)
            .value();
    const libchiaking::consoles::Firmware firmware{family};
    store.seed(libchiaking::consoles::SavedConsole{
        hardware_id, firmware,
        libchiaking::consoles::ConsoleName::of(firmware.generation()),
        std::nullopt});
    store.seed(libchiaking::consoles::SavedPairing{
        libchiaking::AccountId{account}, hardware_id, std::move(keys),
        std::nullopt});
    EXPECT_TRUE(known.remember_saved());
    auto console = known.find(hardware_id);
    if (!console) throw std::logic_error{"the paired console was not known"};
    return console;
  }

  static auto title(std::string_view title_id, std::string_view start_id)
      -> std::optional<libchiaking::consoles::InstalledTitle> {
    return libchiaking::consoles::InstalledTitle::create(
        {std::string{title_id}, std::string{start_id}, "Game",
         "https://img/g.png", libchiaking::consoles::Generation::ps5,
         std::nullopt, false});
  }

  auto never_logged_the_token() const -> ::testing::AssertionResult {
    for (const auto& line : log.messages)
      if (line.find(token) != std::string::npos)
        return ::testing::AssertionFailure()
               << "the token was logged: " << line;
    return ::testing::AssertionSuccess();
  }

  libchiaking::platform::DefaultThreadingProvider& threading{
      test_support::process_threading()};
  libchiaking::platform::SteadyClock steady;
  libchiaking::platform::SystemClock wall;
  ScriptedHttp http;
  test_support::FakeLog log;
  libchiaking::testing::InMemoryCredentialStore credentials{threading};
  libchiaking::platform::SystemSecureRandom user_random;
  libchiaking::users::SignedInUsers everyone{
      libchiaking::testing::ServiceBuilder::build<
          libchiaking::users::SignedInUsers>(http, credentials, threading,
                                             steady, wall, user_random)};
  libchiaking::platform::DefaultNetworkProvider network{threading, steady};
  test_support::NoCrypto crypto;
  libchiaking::platform::SystemSecureRandom random;
  libchiaking::testing::InMemoryConsoleStore store{threading};
  libchiaking::consoles::KnownConsoles known{
      libchiaking::testing::ServiceBuilder::build<
          libchiaking::consoles::KnownConsoles>(network, http, crypto, random,
                                                store, threading, steady,
                                                nullptr, &log)};
};

class ConsoleTitlesTest : public ::testing::Test {
 protected:
  static constexpr std::string_view living_room = "a1b2c3d4e5f6";
  static constexpr std::string_view old_console = "112233445566";
  static constexpr std::string_view commands_url =
      "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/"
      "commands";

  TitlesRig rig;
};

}  // namespace api_consoles_test
