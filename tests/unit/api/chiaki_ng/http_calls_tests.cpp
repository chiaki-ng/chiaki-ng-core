#include "tests/unit/api/chiaki_ng/rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>

namespace chiaki_ng_test {
namespace {

using ChiakiNgHttpCalls = ChiakiNgTest;
using libchiaking::diagnostics::HttpArea;
using Kind = platform::HttpCredential;

TEST_F(ChiakiNgHttpCalls, AreRecordedWithTheirContextAndCredentials) {
  Household home;
  EXPECT_TRUE(home.credentials.save(SignedInPlayer::saved()));
  EXPECT_TRUE(home.credentials.save(SignedInPlayer::saved_for_remote_play()));
  home.save_pairing(AccountId{home_account}, living_room);
  EXPECT_TRUE(home.chiaki.users().welcome_back());
  EXPECT_TRUE(home.chiaki.consoles().remember_saved());
  const auto player =
      home.chiaki.users().find(AccountId{SignedInPlayer::account});
  const auto remote =
      home.chiaki.users().find(AccountId{SignedInPlayer::remote_play_account});
  const auto console =
      home.chiaki.consoles().find(Household::hardware(living_room));
  ASSERT_TRUE(player);
  ASSERT_TRUE(remote);
  ASSERT_TRUE(console);

  const auto started = home.chiaki.play().start_on_cloud(
      player, test_support::play::Title::product_id(cloud_title), {},
      test_support::play::LaunchCase::named("dc1"), home.media, home.events);
  ASSERT_TRUE(started);
  home.events.until("datacenter_chosen");
  home.end(*started.value());
  home.ps.consoles_on_account =
      test_support::play::ScriptedCloudPs::account_listing("aa", R"(["remotePlay"])");
  static_cast<void>(console->route_for(*remote));
  static_cast<void>(home.chiaki.catalog().process_library_and_list(*player));

  const auto snapshot = home.chiaki.http_calls().snapshot();
  const auto find = [&](std::string_view label) {
    return std::ranges::find_if(snapshot.calls, [label](const auto& call) {
      return call->label() == label;
    });
  };
  const auto listed = find("psn.cloud_assisted_navigation.list_clients");
  ASSERT_TRUE(listed != snapshot.calls.end());
  EXPECT_EQ((*listed)->area(), HttpArea::consoles);
  EXPECT_EQ((*listed)->credentials(), carried({Kind::account_token}));
  EXPECT_EQ((*listed)->credential_label(), "remote play bearer");
  for (const auto label :
       {"gk.start", "gk.authorize", "gk.lock", "gk.datacenters"}) {
    const auto call = find(label);
    ASSERT_TRUE(call != snapshot.calls.end()) << label;
    EXPECT_EQ((*call)->area(), HttpArea::play) << label;
    EXPECT_EQ((*call)->credentials(), carried({Kind::gk_session})) << label;
    EXPECT_EQ((*call)->base(), "https://cc.prod.gaikai.com") << label;
  }
  for (const auto label :
       {"psn.auth.authorize_check", "psn.auth.gk_client_code",
        "psn.auth.gk_server_code"}) {
    const auto call = find(label);
    ASSERT_TRUE(call != snapshot.calls.end()) << label;
    EXPECT_EQ((*call)->area(), HttpArea::play) << label;
    EXPECT_EQ((*call)->credentials(), carried({Kind::sign_in_cookie})) << label;
  }
  for (const auto label : {"gk.client_ids", "gk.config"}) {
    const auto call = find(label);
    ASSERT_TRUE(call != snapshot.calls.end()) << label;
    EXPECT_TRUE((*call)->credentials().anonymous()) << label;
  }
  EXPECT_TRUE(std::ranges::any_of(snapshot.calls, [](const auto& call) {
    return call->area() == HttpArea::catalog;
  }));
  EXPECT_EQ(snapshot.totals.all.requests, snapshot.calls.size());
  EXPECT_EQ(snapshot.totals.anonymous.requests, 2U);
  const auto before = home.chiaki.http_calls().revision();
  home.chiaki.http_calls().clear();
  EXPECT_TRUE(home.chiaki.http_calls().snapshot().calls.empty());
  EXPECT_NE(home.chiaki.http_calls().revision(), before);
}

}  // namespace
}  // namespace chiaki_ng_test
