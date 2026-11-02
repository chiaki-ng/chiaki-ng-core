#pragma once

#include "src/core/consoles/adapters/titles/console_library.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/consoles/account_rig.hpp"

#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace consoles_test {

class LibraryRig {
 public:
  using Asked =
      libchiaking::Expected<libchiaking::consoles::ConsoleLibrary::Held,
                            libchiaking::users::Unanswered>;
  using Reply = test_support::psn::Reply;
  using Titles = std::vector<libchiaking::consoles::InstalledTitle>;
  using Names = std::vector<std::string>;

  static constexpr std::string_view token = "fake-mobile-token-7f3a";
  static constexpr std::string_view renewed_token =
      "fake-mobile-token-renewed-11c0";
  static constexpr std::string_view other_token =
      "fake-mobile-token-other-52de";
  static constexpr std::string_view den = "a1b2c3d4e5f6";
  static constexpr std::string_view attic = "0a0b0c0d0e0f";
  static constexpr std::string_view party_session =
      "77777777-8888-4999-aaaa-bbbbbbbbbbbb";
  static constexpr std::string_view commands_url =
      "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/"
      "commands";

  static void pause(std::chrono::nanoseconds duration) {
    static const libchiaking::platform::SteadyClock steady;
    libchiaking::platform::Sleeper{test_support::process_threading(), steady}
        .sleep_for(duration);
  }

  static auto played_ms(std::int64_t ms)
      -> std::optional<libchiaking::consoles::ConsoleLibrary::Time> {
    if (ms <= 0) return std::nullopt;
    return libchiaking::consoles::ConsoleLibrary::Time{
        std::chrono::milliseconds{ms}};
  }

  static auto quoted(std::string_view text) -> std::string {
    return "\"" + std::string{text} + "\"";
  }

  static auto joined(const std::vector<std::string>& entries) -> std::string {
    std::string list;
    for (const auto& entry : entries) list += (list.empty() ? "" : ",") + entry;
    return list;
  }

  static auto installed(std::string_view np_title_id, std::string_view platform,
                        std::string_view extra = {}) -> std::string {
    return "{\"npTitleId\":" + quoted(np_title_id) +
           ",\"titleId\":\"x\",\"psPlatform\":" + quoted(platform) +
           ",\"storage\":\"embedded\",\"size\":123" + std::string{extra} + "}";
  }

  static auto accessed(std::string_view when) -> std::string {
    return ",\"lastAccessDateTime\":" + quoted(when);
  }

  static auto client(std::string_view duid, std::string_view name,
                     const std::vector<std::string>& titles = {})
      -> std::string {
    return "{\"duid\":" + quoted(duid) +
           ",\"platform\":\"PS5\",\"device\":{\"name\":" + quoted(name) +
           "},\"systemData\":{\"installedTitles\":{\"titles\":[" +
           joined(titles) + "]}}}";
  }

  static auto listed_console(std::string_view duid, std::string_view name,
                             const std::vector<std::string>& titles = {})
      -> std::string {
    return "{\"duid\":" + quoted(duid) +
           ",\"platform\":\"PS5\",\"device\":{\"name\":" + quoted(name) +
           ",\"enabledFeatures\":[\"remotePlay\"]},\"systemData\":{"
           "\"installedTitles\":{\"titles\":[" +
           joined(titles) + "]}}}";
  }

  static auto clients_page(const std::vector<std::string>& entries)
      -> std::string {
    return "{\"clients\":[" + joined(entries) + "]}";
  }

  static auto game(std::string_view id, std::string_view name,
                   std::string_view image = "https://img/a.png",
                   std::string_view category = "ps5_native_game",
                   std::string_view last = "2026-01-01T18:00:00Z")
      -> std::string {
    return "{\"titleId\":" + quoted(id) + ",\"name\":" + quoted(name) +
           ",\"imageUrl\":" + quoted(image) +
           ",\"category\":" + quoted(category) +
           ",\"lastPlayedDateTime\":" + quoted(last) + "}";
  }

  static auto gamelist(const std::vector<std::string>& titles) -> std::string {
    return "{\"titles\":[" + joined(titles) + "]}";
  }

  static auto filler_games(int start, int count) -> std::vector<std::string> {
    std::vector<std::string> fillers;
    for (int index = start; index < start + count; ++index)
      fillers.push_back(game("FILL" + std::to_string(index) + "_00",
                             "Filler " + std::to_string(index)));
    return fillers;
  }

  static auto ten_clients(int start) -> std::vector<std::string> {
    std::vector<std::string> listed;
    for (int index = start; index < start + 10; ++index)
      listed.push_back(client("duid-" + std::to_string(index),
                              "PS5-" + std::to_string(index)));
    return listed;
  }

  static auto clients_url(std::size_t offset) -> std::string {
    return "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/"
           "users/me/"
           "clients?platform=PS5&includeFields=device,systemData&limit=10&"
           "offset=" +
           std::to_string(offset);
  }

  static auto played_titles_url(std::size_t offset) -> std::string {
    return "https://m.np.playstation.com/api/gamelist/v2/users/me/"
           "titles?limit=50&offset=" +
           std::to_string(offset);
  }

  static auto uid(char digit) -> std::string { return std::string(64, digit); }

  static auto at_ms(std::int64_t milliseconds)
      -> std::optional<std::chrono::system_clock::time_point> {
    return std::chrono::system_clock::time_point{
        std::chrono::milliseconds{milliseconds}};
  }

  static auto title(
      std::string_view title_id, std::string_view start_id,
      std::string_view name, std::string_view cover,
      libchiaking::consoles::Generation platform,
      std::optional<std::chrono::system_clock::time_point> last_played,
      bool recent) -> libchiaking::consoles::InstalledTitle {
    using libchiaking::consoles::InstalledTitle;
    return InstalledTitle::create(
               {std::string{title_id}, std::string{start_id}, std::string{name},
                std::string{cover}, platform,
                last_played
                    ? std::optional{InstalledTitle::LastUse{
                          *last_played,
                          InstalledTitle::LastUse::Known::on_the_account}}
                    : std::nullopt,
                recent})
        .value();
  }

  static auto shelf(const Asked& asked, std::string_view duid)
      -> const libchiaking::consoles::ConsoleLibrary::Shelf* {
    if (!asked) {
      ADD_FAILURE() << "the library has no answer";
      return nullptr;
    }
    for (const auto& console : asked.value()->consoles)
      if (console.duid == duid) return &console;
    return nullptr;
  }

  static auto names(const Titles& titles) -> Names {
    Names all;
    for (const auto& entry : titles) all.push_back(entry.name());
    return all;
  }

  static auto titles_on(
      const std::vector<libchiaking::consoles::RecentlyPlayed>& recent,
      const std::shared_ptr<libchiaking::consoles::Console>& console)
      -> std::optional<Titles> {
    const auto found = std::ranges::find(
        recent, console, &libchiaking::consoles::RecentlyPlayed::console);
    if (found == recent.end()) return std::nullopt;
    return found->titles;
  }

  auto library(std::size_t maximum_response_bytes =
                   libchiaking::net::http::default_maximum_response_bytes)
      -> libchiaking::consoles::ConsoleLibrary {
    return libchiaking::consoles::ConsoleLibrary{
        account.consoles, account.http, account.threading,
        account.steady,   &account.log, maximum_response_bytes};
  }

  auto run(std::shared_ptr<test_support::StopState> stop = nullptr) -> Asked {
    auto asking = library();
    return asking.asked(*user, stop ? libchiaking::platform::StopToken{stop}
                                    : libchiaking::platform::StopToken{});
  }

  void clients(std::size_t offset, std::vector<Reply> replies) {
    http.on("GET", clients_url(offset), std::move(replies));
  }

  void games(std::size_t offset, std::vector<Reply> replies) {
    http.on("GET", played_titles_url(offset), std::move(replies));
  }

  auto leaks_no_token() const -> testing::AssertionResult {
    if (log.contains(token))
      return testing::AssertionFailure() << "the log holds the token";
    if (log.contains(renewed_token))
      return testing::AssertionFailure() << "the log holds the renewed token";
    return testing::AssertionSuccess();
  }

  Account account;
  test_support::psn::FakeHttp& http{account.http};
  test_support::FakeLog& log{account.log};
  std::shared_ptr<libchiaking::users::PsUser> user{account.user(7, token)};
};

}  // namespace consoles_test
