#include "src/core/consoles/adapters/pairing/account_consoles.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/consoles/adapters/pairing/account_directory.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace consoles_test {
namespace {

using libchiaking::consoles::AccountConsole;
using libchiaking::consoles::AccountConsoles;
using libchiaking::consoles::DeviceUid;
using libchiaking::consoles::Generation;
using libchiaking::users::Unanswered;
using test_support::psn::Reply;
using Reason = Unanswered::Reason;

static_assert(AccountConsoles::page_size == 10);
static_assert(AccountConsole::name_limit == 512);

class AccountConsolesListing : public AccountDirectory {
 protected:
  auto failure_for(Reply reply) -> std::optional<Unanswered> {
    const auto user = account.user(7, store_token, remote_token);
    account.http.on("GET", std::string{consoles_url}, {std::move(reply)});
    const auto listed =
        account.consoles.consoles_on_account(*user, Generation::ps5, {});
    EXPECT_FALSE(account.log.contains(remote_token));
    EXPECT_FALSE(account.log.contains(renewed_remote_token));
    EXPECT_FALSE(listed);
    if (listed) return std::nullopt;
    return listed.error();
  }

  auto full_first_page() const -> std::vector<std::string> {
    std::vector<std::string> full;
    for (char digit = '1'; digit <= '9'; ++digit)
      full.push_back(client(std::string(63, '0') + digit,
                            std::string{"Console "} + digit));
    full.push_back(R"({"duid":"zz","device":{"name":"Broken"}})");
    return full;
  }
};

TEST_F(AccountConsolesListing, IsExactlyTheMainConsoleForAPs4) {
  const auto user = account.user(7, store_token, remote_token);
  const auto listed =
      account.consoles.consoles_on_account(*user, Generation::ps4, {});
  ASSERT_TRUE(listed);
  ASSERT_EQ(listed.value().size(), 1U);
  const auto& main = listed.value().front();
  EXPECT_EQ(main, AccountConsole::main_ps4());
  EXPECT_TRUE(main.is_main_ps4());
  EXPECT_EQ(main.device_uid(), DeviceUid::main_ps4());
  EXPECT_EQ(main.generation(), Generation::ps4);
  EXPECT_EQ(main.name(), AccountConsole::main_ps4_name);
  EXPECT_TRUE(main.remote_play_enabled());
  EXPECT_TRUE(account.http.seen.empty());
}

TEST_F(AccountConsolesListing, AnswersAHomeUserSignedOutWithoutARequest) {
  const auto home = account.home_user(40);
  for (const auto generation : {Generation::ps4, Generation::ps5}) {
    const auto listed =
        account.consoles.consoles_on_account(*home, generation, {});
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().reason, Reason::signed_out);
  }
  EXPECT_TRUE(account.http.seen.empty());
}

TEST_F(AccountConsolesListing, KeepsOnlyAddressableConsoles) {
  const std::string first(64, 'a');
  const std::string second = std::string(62, '0') + "ff";
  const auto user = account.user(7, store_token, remote_token);
  account.http.on(
      "GET", std::string{consoles_url},
      {Reply{200, page({client(first, "Living room"),
                        R"({"duid":"zz","device":{"name":"Broken"}})",
                        client(second, "Bedroom", R"(["other"])")})}});
  AccountConsoles consoles{account.http, account.steady, &account.log};
  const auto listed = consoles.of(*user, Generation::ps5, {});
  ASSERT_TRUE(listed);
  ASSERT_EQ(listed.value().consoles.size(), 2U);
  EXPECT_EQ(listed.value().skipped, 1U);
  const auto& living = listed.value().consoles[0];
  EXPECT_EQ(living.name(), "Living room");
  EXPECT_TRUE(living.remote_play_enabled());
  EXPECT_EQ(living.device_uid().bytes()[0], std::byte{0xaa});
  EXPECT_EQ(living.device_uid().bytes()[31], std::byte{0xaa});
  const auto& bedroom = listed.value().consoles[1];
  EXPECT_EQ(bedroom.name(), "Bedroom");
  EXPECT_FALSE(bedroom.remote_play_enabled());
  EXPECT_EQ(bedroom.device_uid().bytes()[0], std::byte{0});
  EXPECT_EQ(bedroom.device_uid().bytes()[31], std::byte{0xff});
  ASSERT_EQ(account.http.seen.size(), 1U);
  EXPECT_NE(account.http.seen.front().url.find("limit=10&offset=0"),
            std::string::npos);
  EXPECT_EQ(account.http.seen.front().header("Authorization"),
            "Bearer " + std::string{remote_token});
  EXPECT_FALSE(account.log.contains(remote_token));
}

TEST(AccountConsolesTranslation, SkipsWhatCannotBeAnAccountConsole) {
  const std::string first(64, 'a');
  const auto uid = DeviceUid::parse(first).value();
  libchiaking::psn::rest::ClientsPage ps;
  ps.consoles.push_back({first, "bad\nname", {}, uid.bytes(), true});
  ps.consoles.push_back(
      {std::string(64, 'A'), "Main", {}, DeviceUid::main_ps4().bytes(), true});
  ps.consoles.push_back({"zz", "Unaddressable", {}, std::nullopt, true});
  ps.consoles.push_back({first, "Kept", {}, uid.bytes(), false});
  ps.received = 4;
  ps.unaddressable = 1;
  const auto translated = AccountConsoles::translated(ps);
  EXPECT_EQ(translated.skipped, 3U);
  ASSERT_EQ(translated.consoles.size(), 1U);
  EXPECT_EQ(translated.consoles.front().name(), "Kept");
  EXPECT_FALSE(translated.consoles.front().remote_play_enabled());
}

TEST_F(AccountConsolesListing, ReadsEveryPageOfTheDirectory) {
  const auto user = account.user(7, store_token, remote_token);
  account.http.on("GET", std::string{consoles_url},
                  {Reply{200, page(full_first_page())}});
  account.http.on(
      "GET", std::string{second_page_url},
      {Reply{200, page({client(std::string(64, 'a'), "Attic"),
                        R"({"duid":"yy","device":{"name":"Broken too"}})"})}});
  AccountConsoles consoles{account.http, account.steady, &account.log};
  const auto listed = consoles.of(*user, Generation::ps5, {});
  ASSERT_TRUE(listed);
  ASSERT_EQ(account.http.seen.size(), 2U);
  ASSERT_EQ(listed.value().consoles.size(), 10U);
  EXPECT_EQ(listed.value().skipped, 2U);
  EXPECT_EQ(listed.value().consoles.front().name(), "Console 1");
  EXPECT_EQ(listed.value().consoles[8].name(), "Console 9");
  EXPECT_EQ(listed.value().consoles.back().name(), "Attic");
  EXPECT_EQ(listed.value().consoles.back().device_uid(), uid('a'));
  EXPECT_EQ(account.http.seen[1].url, second_page_url);
}

TEST_F(AccountConsolesListing, FailsWhenALaterPageFails) {
  const auto user = account.user(7, store_token, remote_token);
  account.http.on("GET", std::string{consoles_url},
                  {Reply{200, page(full_first_page())}});
  account.http.on("GET", std::string{second_page_url}, {Reply{429, "{}"}});
  AccountConsoles consoles{account.http, account.steady, &account.log};
  const auto listed = consoles.of(*user, Generation::ps5, {});
  ASSERT_FALSE(listed);
  EXPECT_EQ(listed.error().reason, Reason::rate_limited);
  EXPECT_GE(account.http.seen.size(), 2U);
}

TEST_F(AccountConsolesListing, TypesARefusedTokenAsMissingAccess) {
  account.renews_remote_play_access(renewed_remote_token);
  const auto refused = failure_for(Reply{401, "{}"});
  ASSERT_TRUE(refused);
  EXPECT_EQ(refused->reason, Reason::remote_play_access_missing);
  EXPECT_EQ(refused->http_status, 401);
}

TEST_F(AccountConsolesListing, TypesARateLimit) {
  const auto failure = failure_for(Reply{429, "{}"});
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->reason, Reason::rate_limited);
}

TEST_F(AccountConsolesListing, TypesAMalformedReply) {
  const auto failure = failure_for(Reply{200, "{}"});
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->reason, Reason::malformed);
}

TEST_F(AccountConsolesListing, TypesATransportFailureAsUnavailable) {
  const auto failure = failure_for(Reply::transport());
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->reason, Reason::unavailable);
}

TEST_F(AccountConsolesListing, NeedsRemotePlayAccessBeforeAnyRequest) {
  const auto without = account.user(9, store_token);
  const auto missing =
      account.consoles.consoles_on_account(*without, Generation::ps5, {});
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().reason, Reason::remote_play_access_missing);
  EXPECT_TRUE(account.http.seen.empty());
}

TEST_F(AccountConsolesListing, IsCancelledBeforeAnyRequest) {
  const auto user = account.user(7, store_token, remote_token);
  const auto stop = std::make_shared<test_support::StopState>();
  stop->stopped = true;
  const auto cancelled = account.consoles.consoles_on_account(
      *user, Generation::ps5, libchiaking::platform::StopToken{stop});
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().reason, Reason::cancelled);
  EXPECT_TRUE(account.http.seen.empty());
}

}  // namespace
}  // namespace consoles_test
