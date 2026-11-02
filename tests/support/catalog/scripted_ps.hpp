#pragma once

#include "libchiaking/testing/service_builder.hpp"
#include "src/core/catalog/adapters/cache/cache.hpp"
#include "src/core/catalog/adapters/cache/held_answers.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/users/adapters/account_claims.hpp"
#include "src/core/utils/text/base64.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/setup.hpp"
#include "tests/support/users/signed_in.hpp"

#include <libchiaking/catalog.hpp>
#include <libchiaking/catalog_types.hpp>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/testing/in_memory_record_store.hpp>
#include <libchiaking/users.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace test_support::catalog {

class ScriptedPs final : public libchiaking::platform::HttpProvider {
 public:
  struct Header {
    std::string name;
    std::string value;
  };

  struct Reply {
    Reply(int code, std::string text = {}, std::vector<Header> extra = {},
          bool error = false)
        : status(code),
          body(std::move(text)),
          headers(std::move(extra)),
          transport_error(error) {}

    int status;
    std::string body;
    std::vector<Header> headers;
    bool transport_error;
  };

  struct Seen {
    std::string method;
    std::string url;
    std::vector<Header> headers;
  };

  static auto location(std::string_view value) -> std::vector<Header> {
    return {{"Location", std::string{value}}};
  }

  void on(std::string method, std::string fragment,
          std::vector<Reply> replies) {
    const libchiaking::platform::Lock lock{mutex_};
    for (auto& rule : rules_) {
      if (rule.method != method || rule.fragment != fragment) continue;
      rule.replies.assign(replies.begin(), replies.end());
      return;
    }
    rules_.push_back({std::move(method),
                      std::move(fragment),
                      {replies.begin(), replies.end()}});
  }

  void before_everything(std::string method, std::string fragment,
                         std::vector<Reply> replies) {
    const libchiaking::platform::Lock lock{mutex_};
    rules_.push_front({std::move(method),
                       std::move(fragment),
                       {replies.begin(), replies.end()}});
  }

  void hold(std::string fragment) {
    const libchiaking::platform::Lock lock{mutex_};
    held_fragment_ = std::move(fragment);
    released_ = false;
    holding_ = 0;
  }

  void wait_until_held() {
    libchiaking::platform::Lock lock{mutex_};
    changed_.wait(lock, [this] { return holding_ > 0; });
  }

  void release() {
    const libchiaking::platform::Lock lock{mutex_};
    released_ = true;
    held_fragment_.clear();
    changed_.notify_all();
  }

  auto perform(const libchiaking::platform::HttpRequest& request,
               const libchiaking::platform::StopToken& stop)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    libchiaking::platform::Lock lock{mutex_};
    const std::string method =
        libchiaking::platform::http_method_name(request.method);
    const std::string url{request.url.view()};
    std::vector<Header> headers;
    for (const auto& header : request.headers)
      headers.push_back({header.name, std::string{header.value.view()}});
    seen_.push_back({method, url, std::move(headers)});
    if (!held_fragment_.empty() &&
        url.find(held_fragment_) != std::string::npos) {
      ++holding_;
      changed_.notify_all();
      changed_.wait(lock, [this] { return released_; });
    }
    if (stop.stop_requested())
      return libchiaking::unexpected(
          libchiaking::platform::HttpError::cancelled);
    for (auto& rule : rules_) {
      if (rule.method != method || url.find(rule.fragment) == std::string::npos)
        continue;
      Reply reply = rule.replies.front();
      if (rule.replies.size() > 1) rule.replies.pop_front();
      if (reply.transport_error)
        return libchiaking::unexpected(
            libchiaking::platform::HttpError::disconnected);
      libchiaking::platform::HttpResponse response;
      response.status_code = static_cast<std::uint16_t>(reply.status);
      for (const auto& header : reply.headers)
        response.headers.push_back(
            {header.name, libchiaking::SecretString{header.value}});
      response.body.assign(std::string_view{reply.body});
      return response;
    }
    return libchiaking::unexpected(
        libchiaking::platform::HttpError::disconnected);
  }

  auto count(std::string_view fragment) const -> int {
    const libchiaking::platform::Lock lock{mutex_};
    return static_cast<int>(
        std::ranges::count_if(seen_, [fragment](const Seen& request) {
          return request.url.find(fragment) != std::string::npos;
        }));
  }

  auto requests() const -> std::size_t {
    const libchiaking::platform::Lock lock{mutex_};
    return seen_.size();
  }

  auto first_asking(std::string_view fragment) const
      -> std::optional<std::size_t> {
    const libchiaking::platform::Lock lock{mutex_};
    for (std::size_t at = 0; at < seen_.size(); ++at)
      if (seen_[at].url.find(fragment) != std::string::npos) return at;
    return std::nullopt;
  }

  auto seen() const -> std::vector<Seen> {
    const libchiaking::platform::Lock lock{mutex_};
    return seen_;
  }

 private:
  struct Rule {
    std::string method;
    std::string fragment;
    std::deque<Reply> replies;
  };

  libchiaking::platform::DefaultThreadingProvider threading_;
  libchiaking::platform::SteadyClock steady_;
  mutable libchiaking::platform::Mutex mutex_{threading_};
  libchiaking::platform::ConditionVariable changed_{threading_, steady_};
  std::deque<Rule> rules_;
  std::vector<Seen> seen_;
  std::string held_fragment_;
  bool released_{true};
  int holding_{};
};

class ServerTime final : public libchiaking::catalog::TrustedClock {
 public:
  [[nodiscard]] auto now() const noexcept -> std::optional<Time> override {
    return current;
  }

  [[nodiscard]] auto refresh(const libchiaking::platform::StopToken&,
                             const ServerObserver& asking)
      -> std::optional<Time> override {
    ++refreshes;
    if (!current && asking)
      for (const auto& host : asking_hosts) asking(host);
    if (!current) current = synced;
    return current;
  }

  [[nodiscard]] auto last_sync() const -> std::optional<Sync> override {
    return last;
  }

  std::optional<Time> current{};
  std::optional<Time> synced{};
  std::optional<Sync> last{};
  std::vector<std::string> asking_hosts{};
  std::atomic<int> refreshes{};
};

class CatalogRig {
 public:
  using Time = std::chrono::sys_time<std::chrono::milliseconds>;
  using Reply = ScriptedPs::Reply;

  static constexpr std::int64_t wall_start_ms = 1'900'000'000'000;
  static constexpr std::string_view feature_config =
      "portal/np/portal-config.json";
  static constexpr std::string_view cloud_game_resolver =
      "api/catalog/v2/products/cloudgame";
  static constexpr std::string_view store_streaming_category =
      "categories/83999501-dbe5-413a-b265-6484e0a7626e?";
  static constexpr std::string_view device =
      "000000070041008012345678901234567890123456789012";

  CatalogRig() {
    wall.now_us = static_cast<std::uint64_t>(wall_start_ms) * 1000U;
    http.on("GET", std::string{feature_config},
            {{200, R"({"excludedTitleIds":[]})"}});
    http.on("GET", std::string{cloud_game_resolver},
            {{200, R"({"productInfo":[]})"}});
    http.on("GET", std::string{store_streaming_category},
            {{200, R"({"totalNumProducts":0,"products":[]})"}});
  }

  static auto token_carrying(std::string_view claims) -> std::string {
    const auto bytes = std::as_bytes(std::span{claims.data(), claims.size()});
    return "eyJhbGciOiJub25lIn0." +
           libchiaking::text::base64::encode(
               bytes, libchiaking::text::base64::Alphabet::url) +
           ".signature";
  }

  static auto product(std::string_view id) -> libchiaking::catalog::ProductId {
    return Setup::value(libchiaking::catalog::ProductId::parse(id),
                        "the product id parses");
  }

  void hold_exclusions() {
    libchiaking::catalog::cache::Cache cache{records, wall};
    Setup::require(
        static_cast<bool>(
            cache.write(libchiaking::catalog::cache::ExclusionListRecord{
                libchiaking::catalog::cloud_stream::ExclusionList{{}, now()}})),
        "the exclusion list is cached");
  }

  auto saved(std::uint64_t account) const -> libchiaking::users::SavedSignIn {
    libchiaking::users::SavedSignIn saved{
        libchiaking::AccountId{account}, "player",
        libchiaking::psn::auth::Npsso{"NPSSO"}};
    saved.store.emplace(
        libchiaking::psn::auth::AppToken{"app"},
        std::chrono::system_clock::time_point{
            std::chrono::milliseconds{wall_start_ms} + std::chrono::hours{72}});
    saved.device = libchiaking::users::DeviceId::parse(device);
    Setup::require(saved.device.has_value(), "the device id parses");
    saved.context =
        libchiaking::users::AccountClaims::read(token_carrying(account_claims));
    return saved;
  }

  auto user(std::uint64_t account = 1,
            std::optional<libchiaking::users::Region> region = {})
      -> std::shared_ptr<libchiaking::users::PsUser> {
    auto remembered = saved(account);
    remembered.region = std::move(region);
    return signed_in.as(std::move(remembered));
  }

  auto home_user(std::uint64_t account = 9)
      -> std::shared_ptr<libchiaking::users::PsUser> {
    return Setup::value(
        signed_in.users.add_home_user(libchiaking::AccountId{account},
                                      "Living Room Player"),
        "the home user is added");
  }

  auto now() const -> Time {
    return Time{std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::microseconds{wall.now_us})};
  }

  void advance(std::chrono::milliseconds by) {
    const auto microseconds = static_cast<std::uint64_t>(by.count()) * 1000U;
    wall.now_us += microseconds;
    clock.now_us += microseconds;
  }

  auto has_record(std::string_view name) -> bool {
    return std::ranges::any_of(records.names(), [name](const auto& held) {
      return held.text() == name;
    });
  }

  libchiaking::platform::DefaultThreadingProvider threading;
  test_support::FakeClock clock{0};
  test_support::FakeClock wall{0};
  ScriptedPs http;
  ServerTime server;
  libchiaking::testing::InMemoryRecordStore records{threading};
  test_support::users::SignedIn signed_in{http, clock, wall};
  libchiaking::users::RequestBudget budget{threading, clock};
  libchiaking::catalog::Catalog catalog{
      libchiaking::testing::ServiceBuilder::build<
          libchiaking::catalog::Catalog>(http, records, threading, clock, wall,
                                         server, budget)};
  std::string account_claims =
      R"({"age":34,"legal_country":"GB","locale":"en-GB","is_child":false})";
};

}  // namespace test_support::catalog
