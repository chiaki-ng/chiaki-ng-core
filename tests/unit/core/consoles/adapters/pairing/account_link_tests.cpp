#include "src/core/consoles/adapters/pairing/account_link.hpp"
#include "tests/unit/core/consoles/adapters/pairing/account_directory.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace consoles_test {
namespace {

using libchiaking::consoles::AccountLink;
using libchiaking::consoles::Console;
using libchiaking::consoles::ConsoleName;
using libchiaking::consoles::Firmware;
using libchiaking::consoles::Generation;
using test_support::psn::Reply;
using Listed = AccountLink::Listed;
using Rule = AccountLink::Rule;

class AccountLinkLearned : public AccountDirectory {
 protected:
  void SetUp() override {
    EXPECT_EQ(living->name(), "PS5");
    EXPECT_FALSE(account.facts.device_uid_of(*living, *user));
    pairing_saves = account.store.pairing_saves;
    account.http.on(
        "GET", std::string{consoles_url},
        {Reply{200, page({client(std::string(64, 'a'), "Living Room")})}});
  }

  void list_the_account() {
    const auto listed =
        account.consoles.consoles_on_account(*user, Generation::ps5, {});
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed.value().size(), 1U);
  }

  std::shared_ptr<libchiaking::users::PsUser> user =
      account.user(7, store_token, remote_token);
  std::shared_ptr<Console> living = account.paired(*user, den, std::nullopt);
  std::shared_ptr<Console> old = account.paired(
      *user, "112233445566", std::nullopt, Firmware::Family::ps4_10);
  std::size_t pairing_saves{};
};

TEST_F(AccountLinkLearned, ListsTheAccountsConsole) {
  const auto listed =
      account.consoles.consoles_on_account(*user, Generation::ps5, {});
  ASSERT_TRUE(listed);
  ASSERT_EQ(listed.value().size(), 1U);
  EXPECT_EQ(account.http.seen.size(), 1U);
  const auto& first = listed.value().front();
  EXPECT_EQ(first.device_uid(), uid('a'));
  EXPECT_EQ(first.name(), "Living Room");
  EXPECT_EQ(first.generation(), Generation::ps5);
  EXPECT_TRUE(first.remote_play_enabled());
  EXPECT_FALSE(first.is_main_ps4());
  ASSERT_FALSE(account.http.seen.empty());
  EXPECT_EQ(account.http.seen.front().header("Authorization"),
            "Bearer " + std::string{remote_token});
}

TEST_F(AccountLinkLearned, SavesTheLearnedUidWithThePairing) {
  ASSERT_NO_FATAL_FAILURE(list_the_account());
  EXPECT_EQ(account.facts.device_uid_of(*living, *user), uid('a'));
  EXPECT_EQ(account.store.pairing_saves, pairing_saves + 1);
  const auto stored =
      account.store.pairing(user->account_id(), living->hardware_id());
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->device_uid, uid('a'));
}

TEST_F(AccountLinkLearned, NamesTheConsoleAfterTheAccount) {
  ASSERT_NO_FATAL_FAILURE(list_the_account());
  EXPECT_EQ(living->name(), "Living Room");
  EXPECT_EQ(living->name_source(), ConsoleName::Source::account);
  const auto kept = account.store.console(living->hardware_id());
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept->name.text(), "Living Room");
  EXPECT_EQ(kept->name.source(), ConsoleName::Source::account);
  EXPECT_EQ(old->name(), "PS4");
  EXPECT_FALSE(account.facts.device_uid_of(*old, *user));
}

TEST_F(AccountLinkLearned, RanksTheAccountNameBelowADiscoveredOne) {
  ASSERT_NO_FATAL_FAILURE(list_the_account());
  account.facts.sighted(
      {living->hardware_id(), Firmware{Firmware::Family::ps5_1},
       Values::name("PS5-042", ConsoleName::Source::discovered),
       Values::host("192.168.1.9"), false, std::nullopt});
  EXPECT_EQ(living->name(), "PS5-042");
  const auto again =
      account.consoles.consoles_on_account(*user, Generation::ps5, {});
  ASSERT_TRUE(again);
  EXPECT_EQ(account.http.seen.size(), 2U);
  EXPECT_EQ(living->name(), "PS5-042");
  EXPECT_EQ(living->name_source(), ConsoleName::Source::discovered);
  EXPECT_EQ(account.store.pairing_saves, pairing_saves + 1);
}

TEST_F(AccountLinkLearned, LinksByTheStoredUidOnlyForItsOwner) {
  ASSERT_NO_FATAL_FAILURE(list_the_account());
  AccountLink link{account.consoles};
  const std::array ps{Listed{"Renamed By PS", uid('a')},
                      Listed{"Elsewhere", uid('c')}};
  const auto links = link.linked(*user, ps);
  ASSERT_EQ(links.size(), 1U);
  EXPECT_EQ(links.front().console, living);
  EXPECT_EQ(links.front().listed, 0U);
  EXPECT_EQ(links.front().rule, Rule::stored_device_uid);
  const auto stranger = account.user(8, "fake-mobile-token-other-66b1");
  EXPECT_TRUE(link.linked(*stranger, ps).empty());
}

}  // namespace
}  // namespace consoles_test
