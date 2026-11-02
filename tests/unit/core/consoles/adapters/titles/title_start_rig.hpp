#pragma once

#include "src/core/consoles/adapters/titles/console_library.hpp"
#include "src/core/consoles/adapters/titles/title_start.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/support/waiter.hpp"
#include "tests/unit/core/consoles/account_rig.hpp"
#include "tests/support/consoles/start_states.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace consoles_test {

class StartHarness {
 public:
  using Reply = test_support::psn::Reply;
  using States = std::vector<libchiaking::consoles::StartState>;

  static constexpr std::string_view token = "fake-mobile-token-91bd";
  static constexpr std::string_view renewed_token =
      "fake-mobile-token-renewed-3c7e";
  static constexpr std::string_view command_id =
      "3f2a9c1e-0000-4000-8000-00000000abcd";
  static constexpr std::string_view commands_url =
      "https://web.np.playstation.com/api/"
      "cloudAssistedNavigation/v2/users/me/commands";

  static auto poll_url() -> std::string {
    return std::string{commands_url} +
           "?platform=PS5&commandIds=" + std::string{command_id} +
           "&includeFields=actions,commandDetail&commandStatus=executing,"
           "completed";
  }

  static auto accepted() -> Reply {
    return {202, "{\"commandId\":\"" + std::string{command_id} + "\"}"};
  }

  static auto status(std::string_view command_status,
                     std::string_view action_type = {}) -> Reply {
    std::string actions;
    if (!action_type.empty())
      actions = ",\"actions\":[{\"action\":{\"actionType\":\"" +
                std::string{action_type} +
                "\",\"actionState\":\"processing\"}}]";
    return {200, "{\"commands\":[{\"commandId\":\"" + std::string{command_id} +
                     "\",\"commandStatus\":\"" + std::string{command_status} +
                     "\",\"commandResult\":\"n/a\"" + actions + "}]}"};
  }

  static auto with_header(Reply reply, std::string value) -> Reply {
    reply.headers.push_back({"X-PSN-CAN-Frequency-Control", std::move(value)});
    return reply;
  }

  static auto installed(std::string_view title_id)
      -> std::optional<libchiaking::consoles::InstalledTitle> {
    return libchiaking::consoles::InstalledTitle::create(
        {std::string{title_id},
         libchiaking::consoles::ConsoleLibrary::start_id(title_id), "Game",
         "https://img/a.png", libchiaking::consoles::Generation::ps5,
         std::nullopt, false});
  }

  auto run() -> libchiaking::Expected<libchiaking::consoles::StartState,
                                      libchiaking::consoles::StartFailure> {
    waiter.stop_state = stop.get();
    const auto title = installed(title_id).value();
    libchiaking::consoles::TitleStart::Pace pace;
    if (timeout_ms != 0) pace.deadline = std::chrono::milliseconds{timeout_ms};
    if (poll_interval_ms != 0)
      pace.poll = std::chrono::milliseconds{poll_interval_ms};
    test_support::consoles::StartStates watched;
    auto result =
        libchiaking::consoles::TitleStart{http, waiter, clock, &log, pace}.run(
            *user, duid, title, &watched,
            libchiaking::platform::StopToken{stop});
    progress = watched.states;
    EXPECT_FALSE(log.contains(token));
    EXPECT_FALSE(log.contains(renewed_token));
    return result;
  }

  auto polls() const -> int { return http.count("commandIds="); }

  void answers_post(Reply reply) {
    http.on("POST", std::string{commands_url}, {std::move(reply)});
  }

  void answers_polls(std::vector<Reply> replies) {
    http.on("GET", poll_url(), std::move(replies));
  }

  test_support::FakeClock clock;
  Account account{&clock};
  test_support::psn::FakeHttp& http{account.http};
  test_support::FakeLog& log{account.log};
  test_support::FakeWaiter waiter{&clock};
  std::shared_ptr<libchiaking::users::PsUser> user{account.user(7, token)};
  std::shared_ptr<test_support::StopState> stop{
      std::make_shared<test_support::StopState>()};
  States progress;
  std::uint32_t timeout_ms{};
  std::uint32_t poll_interval_ms{};
  std::string_view title_id{"PPSA01234_00"};
  std::string_view duid{"F1E2D3C4B5A6"};
};

}  // namespace consoles_test
