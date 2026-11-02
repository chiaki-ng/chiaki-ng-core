#include "src/core/consoles/paired_play.hpp"
#include "tests/unit/core/consoles/adapters/pairing/account_directory.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace consoles_test {
namespace {

using libchiaking::consoles::Console;
using libchiaking::consoles::ConsoleName;
using libchiaking::consoles::Firmware;
using libchiaking::consoles::PairedConsole;
using libchiaking::consoles::PairedPlay;

class PairedPlayReports : public AccountDirectory {
 protected:
  auto pairing_with(std::uint8_t seed) -> PairedConsole {
    return PairedConsole{console->hardware_id(),
                         Firmware{Firmware::Family::ps5_1}, std::nullopt,
                         Values::keys(seed)};
  }

  std::shared_ptr<libchiaking::users::PsUser> player =
      account.user(7, store_token, remote_token);
  std::shared_ptr<Console> console = account.paired(*player, den, std::nullopt);
  PairedPlay::Reports reports{account.consoles, console, player};
};

TEST_F(PairedPlayReports, NameTheConsoleFromThePairing) {
  EXPECT_EQ(console->name(), "PS5");
  reports.named("Living Room");
  EXPECT_EQ(console->name(), "Living Room");
  EXPECT_EQ(console->name_source(), ConsoleName::Source::pairing);
}

TEST_F(PairedPlayReports, NeverOverrideARename) {
  reports.named("Living Room");
  ASSERT_TRUE(console->rename("Mine"));
  reports.named("Living Room");
  EXPECT_EQ(console->name(), "Mine");
  reports.named(std::string(300, 'n'));
  EXPECT_EQ(console->name(), "Mine");
}

TEST_F(PairedPlayReports, KeepAFreshPairing) {
  const auto kept = reports.paired(pairing_with(9), uid('d'));
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept.value(), console);
  const auto fresh = account.facts.keys_of(*console, *player);
  ASSERT_TRUE(fresh);
  EXPECT_TRUE(std::ranges::equal(fresh->registration_key(),
                                 Values::keys(9).registration_key()));
  EXPECT_EQ(account.facts.device_uid_of(*console, *player), uid('d'));
  const auto stored =
      account.store.pairing(player->account_id(), console->hardware_id());
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->device_uid, uid('d'));
  EXPECT_TRUE(std::ranges::equal(stored->keys.remote_play_key(),
                                 Values::keys(9).remote_play_key()));
}

TEST_F(PairedPlayReports, FailWhenAFreshPairingCannotBeSaved) {
  account.store.refuse_pairing_saves = true;
  EXPECT_FALSE(reports.paired(pairing_with(3), uid('d')));
}

class PairedPlayPs4Recognition : public AccountDirectory {
 protected:
  static constexpr std::string_view old = "112233445566";

  static auto reached(std::string_view id,
                      std::optional<std::string_view> nickname)
      -> PairedConsole {
    std::optional<ConsoleName> named;
    if (nickname) named = Values::name(*nickname, ConsoleName::Source::pairing);
    return PairedConsole{Values::hardware_id(id),
                         Firmware{Firmware::Family::ps4_10}, std::move(named),
                         Values::keys(5)};
  }

  void SetUp() override {
    static_cast<void>(account.paired(*player, "112233445577", "Bedroom",
                                     Firmware::Family::ps4_10));
    static_cast<void>(account.paired(*other, "112233445588", "Lounge",
                                     Firmware::Family::ps4_10));
  }

  std::shared_ptr<libchiaking::users::PsUser> player =
      account.user(7, store_token, remote_token);
  std::shared_ptr<libchiaking::users::PsUser> other = account.user(
      8, "fake-mobile-token-other-66b1", "fake-remote-token-other-1c2d");
  std::shared_ptr<Console> asked =
      account.paired(*player, old, "Old", Firmware::Family::ps4_10);
  std::shared_ptr<Console> ps5 = account.paired(*player, den, "Den");
  PairedPlay::Reports reports{account.consoles, asked, player};
};

TEST_F(PairedPlayPs4Recognition, AcceptsAPs4ThePlayerPaired) {
  EXPECT_TRUE(reports.recognises(reached(old, "Old")));
  EXPECT_TRUE(reports.recognises(reached("112233445577", std::nullopt)));
  EXPECT_TRUE(reports.recognises(reached("1122334455aa", "Bedroom")));
  EXPECT_TRUE(account.http.seen.empty());
}

TEST_F(PairedPlayPs4Recognition, RefusesAnyOtherConsole) {
  EXPECT_FALSE(reports.recognises(reached("112233445588", "Lounge")));
  EXPECT_FALSE(reports.recognises(reached("1122334455aa", "Den")));
  EXPECT_FALSE(reports.recognises(reached("1122334455aa", std::nullopt)));
}

TEST_F(PairedPlayPs4Recognition, AcceptsAnyPs4ForAPs5Session) {
  const PairedPlay::Reports to_ps5{account.consoles, ps5, player};
  EXPECT_TRUE(to_ps5.recognises(reached("1122334455aa", std::nullopt)));
  EXPECT_TRUE(account.http.seen.empty());
}

}  // namespace
}  // namespace consoles_test
