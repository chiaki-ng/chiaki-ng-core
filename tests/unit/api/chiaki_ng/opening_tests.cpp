#include "tests/unit/api/chiaki_ng/rig.hpp"

#include <gtest/gtest.h>

namespace chiaki_ng_test {
namespace {

using ChiakiNgOpening = ChiakiNgTest;

TEST_F(ChiakiNgOpening, NeedsHttpAndCrypto) {
  auto& threading = test_support::process_threading();
  fakes::InMemoryCredentialStore credentials{threading};
  fakes::InMemoryConsoleStore console_store{threading};
  fakes::InMemoryRecordStore records{threading};
  test_support::play::MemoryDatacenterStore datacenters;
  const ChiakiNg::Stores stores{credentials, console_store, datacenters,
                                nullptr, records};
  platform::DefaultPlatformFactory bare;
  const auto no_http = ChiakiNg::open(bare, stores);
  ASSERT_FALSE(no_http);
  EXPECT_EQ(no_http.error(), ChiakiNg::Missing::http);
  test_support::play::ScriptedCloudPs ps;
  platform::DefaultPlatformFactory no_crypto{
      platform::PlatformProviders{.http = &ps}};
  const auto unopened = ChiakiNg::open(no_crypto, stores);
  ASSERT_FALSE(unopened);
  EXPECT_EQ(unopened.error(), ChiakiNg::Missing::crypto);
}

TEST_F(ChiakiNgOpening, ReachesReturningUsersAndTheirConsoles) {
  Household home;
  EXPECT_TRUE(home.credentials.save(SignedInPlayer::saved_for_remote_play()));
  EXPECT_TRUE(home.credentials.save(
      users::SavedHomeUser{AccountId{home_account}, "Living Room Player"}));
  home.save_pairing(AccountId{home_account}, living_room);

  const auto returned = home.chiaki.users().welcome_back();
  ASSERT_TRUE(returned);
  EXPECT_EQ(returned.value().users.size(), 2U);
  EXPECT_EQ(returned.value().rejected, 0U);
  const auto remembered = home.chiaki.consoles().remember_saved();
  ASSERT_TRUE(remembered);
  EXPECT_EQ(remembered.value().consoles.size(), 1U);

  const auto home_user = home.chiaki.users().find(AccountId{home_account});
  const auto signed_in =
      home.chiaki.users().find(AccountId{SignedInPlayer::remote_play_account});
  ASSERT_TRUE(home_user);
  ASSERT_TRUE(signed_in);
  EXPECT_FALSE(home_user->has_psn_account());
  EXPECT_TRUE(signed_in->has_psn_account());
  const auto console =
      home.chiaki.consoles().find(Household::hardware(living_room));
  ASSERT_TRUE(console);
  EXPECT_TRUE(console->is_paired_with(*home_user));
  EXPECT_FALSE(console->is_paired_with(*signed_in));

  const auto refused = home.chiaki.users().sign_in(
      libchiaking::psn::auth::Npsso{"SYNTHETIC_TOKEN"},
      *users::DeviceId::parse(SignedInPlayer::device), "correlation");
  EXPECT_FALSE(refused);
  EXPECT_EQ(home.chiaki.users().everyone().size(), 2U);

  platform::StopSource stopped;
  EXPECT_TRUE(stopped.request_stop());
  auto pin = consoles::PairingPin::create(12345678);
  ASSERT_TRUE(pin);
  const auto unpaired = home.chiaki.consoles().pair_at_home(
      *signed_in, *console, std::move(*pin), stopped.token());
  ASSERT_FALSE(unpaired);
  EXPECT_EQ(unpaired.error().reason,
            consoles::PairingFailure::Reason::cancelled);
}

}  // namespace
}  // namespace chiaki_ng_test
