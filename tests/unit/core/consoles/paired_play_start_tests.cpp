#include "src/core/consoles/paired_play.hpp"
#include "tests/unit/core/consoles/adapters/pairing/account_directory.hpp"

#include <libchiaking/detail/consoles/account_console.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace consoles_test {
namespace {

using libchiaking::consoles::AccountConsole;
using libchiaking::consoles::DeviceUid;
using libchiaking::consoles::Firmware;
using libchiaking::consoles::Generation;
using libchiaking::consoles::PairedPlay;
using test_support::psn::Reply;
using Refusal = PairedPlay::Refusal;
using Listing = std::vector<AccountConsole>;

static_assert(!std::is_copy_constructible_v<PairedPlay::HomeTarget>);
static_assert(std::is_nothrow_move_constructible_v<PairedPlay::HomeTarget>);

class PairedPlayStart : public AccountDirectory {
 protected:
  static constexpr std::string_view attic = "a1b2c3d4e5f7";
  static constexpr std::string_view old = "112233445566";

  static auto listed(char digit, std::string name, bool remote_play = true,
                     Generation generation = Generation::ps5)
      -> AccountConsole {
    return AccountConsole::create(uid(digit), std::move(name), generation,
                                  remote_play)
        .value();
  }

  static auto clients(const std::vector<std::string>& entries) -> std::string {
    return page(entries);
  }
};

class PairedPlayAtHome : public PairedPlayStart {
 protected:
  std::shared_ptr<libchiaking::users::PsUser> player = account.home_user(40);
  std::shared_ptr<libchiaking::users::PsUser> stranger = account.home_user(41);
  std::shared_ptr<libchiaking::consoles::Console> console =
      account.paired(*player, den, "Den");
};

TEST_F(PairedPlayAtHome, NeedsAnAddress) {
  const auto nowhere = PairedPlay{*console, *player}.at_home();
  ASSERT_FALSE(nowhere);
  EXPECT_EQ(nowhere.error(), Refusal::no_address);
}

TEST_F(PairedPlayAtHome, NeedsThisUsersPairing) {
  ASSERT_TRUE(console->moved_to(Values::host("192.168.1.20")));
  const auto unpaired = PairedPlay{*console, *stranger}.at_home();
  ASSERT_FALSE(unpaired);
  EXPECT_EQ(unpaired.error(), Refusal::not_paired);
}

TEST_F(PairedPlayAtHome, CarriesTheAddressFirmwareAndKeys) {
  ASSERT_TRUE(console->moved_to(Values::host("192.168.1.20")));
  const auto target = PairedPlay{*console, *player}.at_home();
  ASSERT_TRUE(target);
  EXPECT_EQ(target.value().host.text(), "192.168.1.20");
  EXPECT_EQ(target.value().firmware, Firmware{Firmware::Family::ps5_1});
  const auto expected = Values::keys(1);
  EXPECT_TRUE(std::ranges::equal(target.value().keys.registration_key(),
                                 expected.registration_key()));
  EXPECT_TRUE(std::ranges::equal(target.value().keys.remote_play_key(),
                                 expected.remote_play_key()));
  EXPECT_EQ(target.value().keys.key_type(), expected.key_type());
}

TEST_F(PairedPlayAtHome, IsRefusedOnceTheConsoleIsForgotten) {
  ASSERT_TRUE(console->moved_to(Values::host("192.168.1.20")));
  ASSERT_TRUE(account.consoles.forget(*console));
  const auto forgotten = PairedPlay{*console, *player}.at_home();
  ASSERT_FALSE(forgotten);
  EXPECT_EQ(forgotten.error(), Refusal::forgotten);
}

class PairedPlayOverTheInternet : public PairedPlayStart {
 protected:
  std::shared_ptr<libchiaking::users::PsUser> home = account.home_user(40);
  std::shared_ptr<libchiaking::users::PsUser> player =
      account.user(7, store_token, remote_token);
};

TEST_F(PairedPlayOverTheInternet, NeedsAPsnSignIn) {
  const auto shared = account.paired(*home, "a1b2c3d4e5f8", "Shared");
  const auto offline = PairedPlay{*shared, *home}.over_the_internet();
  ASSERT_FALSE(offline);
  EXPECT_EQ(offline.error(), Refusal::needs_psn_sign_in);
  EXPECT_TRUE(account.http.seen.empty());
}

TEST_F(PairedPlayOverTheInternet, NeedsTheAccountIdentity) {
  const auto den_console = account.paired(*player, den, "Den");
  const auto unknown = PairedPlay{*den_console, *player}.over_the_internet();
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error(), Refusal::not_on_account);
}

TEST_F(PairedPlayOverTheInternet, UsesALinkedIdentity) {
  const auto linked = account.paired(*player, attic, "Attic",
                                     Firmware::Family::ps5_1, uid('b'));
  const auto known = PairedPlay{*linked, *player}.over_the_internet();
  ASSERT_TRUE(known);
  EXPECT_EQ(known.value().device_uid, uid('b'));
  EXPECT_EQ(known.value().firmware, Firmware::newest(Generation::ps5));
  EXPECT_TRUE(account.http.seen.empty());
}

TEST_F(PairedPlayOverTheInternet, UsesTheMainPs4Identity) {
  const auto ps4 =
      account.paired(*player, old, "Old", Firmware::Family::ps4_10);
  const auto main = PairedPlay{*ps4, *player}.over_the_internet();
  ASSERT_TRUE(main);
  EXPECT_EQ(main.value().device_uid, DeviceUid::main_ps4());
  EXPECT_EQ(main.value().firmware, Firmware::newest(Generation::ps4));
  EXPECT_TRUE(account.http.seen.empty());
}

class PairedPlayByName : public PairedPlayStart {
 protected:
  std::shared_ptr<libchiaking::users::PsUser> player =
      account.user(7, store_token, remote_token);
  std::shared_ptr<libchiaking::users::PsUser> other = account.user(
      8, "fake-mobile-token-other-66b1", "fake-remote-token-other-1c2d");
  std::shared_ptr<libchiaking::consoles::Console> console =
      account.paired(*other, den, "Den");
  PairedPlay unpaired{*console, *player};
};

TEST_F(PairedPlayByName, FindsAnUnlinkedConsoleOnTheAccount) {
  const auto found = unpaired.over_the_internet(
      Listing{listed('a', "den"), listed('c', "Attic")});
  ASSERT_TRUE(found);
  EXPECT_EQ(found.value().device_uid, uid('a'));
  EXPECT_TRUE(account.http.seen.empty());
}

TEST_F(PairedPlayByName, RefusesAnAmbiguousName) {
  const auto ambiguous = unpaired.over_the_internet(
      Listing{listed('a', "Den"), listed('c', "Den")});
  ASSERT_FALSE(ambiguous);
  EXPECT_EQ(ambiguous.error(), Refusal::not_on_account);
}

TEST_F(PairedPlayByName, RefusesAConsoleWithRemotePlayOff) {
  const auto off = unpaired.over_the_internet(
      Listing{listed('a', "Den", false), listed('c', "Attic")});
  ASSERT_FALSE(off);
  EXPECT_EQ(off.error(), Refusal::remote_play_disabled);
}

TEST_F(PairedPlayByName, RefusesAConsoleOfAnotherGeneration) {
  const auto mismatched = unpaired.over_the_internet(
      Listing{listed('a', "Den", true, Generation::ps4)});
  ASSERT_FALSE(mismatched);
  EXPECT_EQ(mismatched.error(), Refusal::not_on_account);
  EXPECT_TRUE(account.http.seen.empty());
}

class PairedPlayAccountAsked : public PairedPlayStart {
 protected:
  std::shared_ptr<libchiaking::users::PsUser> player =
      account.user(7, store_token, remote_token);
};

TEST_F(PairedPlayAccountAsked, ConfirmsALinkedConsole) {
  const auto linked = account.paired(*player, attic, "Attic",
                                     Firmware::Family::ps5_1, uid('b'));
  account.http.on(
      "GET", std::string{consoles_url},
      {Reply{200, clients({client(std::string(64, 'b'), "Attic")})}});
  const auto held =
      PairedPlay::found_on_account(account.consoles, *linked, *player, {});
  ASSERT_TRUE(held);
  EXPECT_EQ(held.value().device_uid, uid('b'));
  EXPECT_EQ(account.http.seen.size(), 1U);
}

TEST_F(PairedPlayAccountAsked, LearnsAndKeepsAskingForEveryPs5Start) {
  account.http.on(
      "GET", std::string{consoles_url},
      {Reply{200, clients({client(std::string(64, 'a'), "Den"),
                           client(std::string(64, 'c'), "Elsewhere")})}});
  const auto console = account.paired(*player, den, "Den");
  const auto found =
      PairedPlay::found_on_account(account.consoles, *console, *player, {});
  ASSERT_TRUE(found);
  EXPECT_EQ(found.value().device_uid, uid('a'));
  EXPECT_EQ(account.facts.device_uid_of(*console, *player), uid('a'));
  const auto requests = account.http.seen.size();
  const auto again =
      PairedPlay::found_on_account(account.consoles, *console, *player, {});
  ASSERT_TRUE(again);
  EXPECT_EQ(again.value().device_uid, uid('a'));
  EXPECT_EQ(account.http.seen.size(), requests + 1);
}

TEST_F(PairedPlayAccountAsked, ReportsAnAccountThatDoesNotAnswer) {
  const auto lonely = account.paired(*player, den, "Den");
  account.http.on("GET", std::string{consoles_url}, {Reply{500, "{}"}});
  const auto unanswered =
      PairedPlay::found_on_account(account.consoles, *lonely, *player, {});
  ASSERT_FALSE(unanswered);
  EXPECT_EQ(unanswered.error(), Refusal::account_unanswered);
}

class PairedPlaySavedIdentity : public PairedPlayStart {
 protected:
  std::shared_ptr<libchiaking::users::PsUser> player =
      account.user(7, store_token, remote_token);
  std::shared_ptr<libchiaking::consoles::Console> console =
      account.paired(*player, den, "Den", Firmware::Family::ps5_1, uid('b'));
  PairedPlay paired{*console, *player};
};

TEST_F(PairedPlaySavedIdentity, IsUsedWhileListed) {
  EXPECT_EQ(paired.over_the_internet(Listing{listed('b', "Renamed")})
                .value()
                .device_uid,
            uid('b'));
}

TEST_F(PairedPlaySavedIdentity, IsRefusedWhenRemotePlayIsOff) {
  const auto off = paired.over_the_internet(Listing{listed('b', "Den", false)});
  ASSERT_FALSE(off);
  EXPECT_EQ(off.error(), Refusal::remote_play_disabled);
}

TEST_F(PairedPlaySavedIdentity, GivesWayToTheNameWhenNoLongerListed) {
  EXPECT_EQ(paired
                .over_the_internet(
                    Listing{listed('a', "Den"), listed('c', "Elsewhere")})
                .value()
                .device_uid,
            uid('a'));
}

TEST_F(PairedPlaySavedIdentity, IsNotOnTheAccountWithoutAClearMatch) {
  const Listing unrelated{listed('c', "Elsewhere")};
  const Listing ambiguous{listed('a', "Den"), listed('c', "Den")};
  const Listing duplicate_id{listed('b', "Den"), listed('b', "Other")};
  const Listing empty;
  for (const auto& entries : {unrelated, ambiguous, duplicate_id, empty}) {
    const auto missing = paired.over_the_internet(entries);
    ASSERT_FALSE(missing) << entries.size();
    EXPECT_EQ(missing.error(), Refusal::not_on_account) << entries.size();
  }
}

TEST_F(PairedPlaySavedIdentity, FollowsTheCurrentListingWithoutSavingIt) {
  account.http.on("GET", std::string{consoles_url},
                  {Reply{200, clients({client(std::string(64, 'a'), "Den")})},
                   Reply{503, "{}"}});
  const auto fresh =
      PairedPlay::found_on_account(account.consoles, *console, *player, {});
  ASSERT_TRUE(fresh);
  EXPECT_EQ(fresh.value().device_uid, uid('a'));
  EXPECT_EQ(account.facts.device_uid_of(*console, *player), uid('b'));
  const auto unavailable =
      PairedPlay::found_on_account(account.consoles, *console, *player, {});
  ASSERT_FALSE(unavailable);
  EXPECT_EQ(unavailable.error(), Refusal::account_unanswered);
  EXPECT_EQ(account.http.seen.size(), 2U);
}

TEST_F(PairedPlaySavedIdentity, NeverAsksTheAccountForAPs4) {
  const auto ps4 =
      account.paired(*player, old, "Old", Firmware::Family::ps4_10);
  EXPECT_TRUE(
      PairedPlay::found_on_account(account.consoles, *ps4, *player, {}));
  EXPECT_TRUE(account.http.seen.empty());
}

}  // namespace
}  // namespace consoles_test
