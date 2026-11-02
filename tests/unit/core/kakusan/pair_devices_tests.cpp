#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/pair_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace kakusan_test {
namespace {

class KakusanReinstall : public PairTest {};

TEST_F(KakusanReinstall, RecognisesWhatTheFirstInstallSaved) {
  libchiaking::kakusan::IdentityKey host_key;
  libchiaking::kakusan::IdentityKey guest_key;
  {
    PairStage stage{host_store, guest_store};
    ASSERT_NO_FATAL_FAILURE(stage.begin());
    host_key = stage.pair->host().this_device().key();
    guest_key = stage.pair->guest().this_device().key();
    ASSERT_NO_FATAL_FAILURE(stage.verified());
    EXPECT_TRUE(stage.share->remember(stage.guest_id, "Guest pad"));
    EXPECT_TRUE(stage.visit->remember_host("Host den"));
  }
  PairStage later{host_store, guest_store};
  ASSERT_NO_FATAL_FAILURE(later.begin());
  EXPECT_EQ(host_store.identity_saves(), 1U);
  EXPECT_EQ(guest_store.identity_saves(), 1U);
  EXPECT_EQ(later.pair->host().this_device().key(), host_key);
  EXPECT_EQ(later.pair->guest().this_device().key(), guest_key);
  ASSERT_EQ(later.pair->host().remembered().all().size(), 1U);
  ASSERT_FALSE(host_store.devices().empty());
  EXPECT_EQ(later.pair->host().remembered().all().front().fingerprint,
            host_store.devices().front().fingerprint);
  ASSERT_NO_FATAL_FAILURE(later.open());
  ASSERT_NO_FATAL_FAILURE(later.stream());
  ASSERT_NO_FATAL_FAILURE(later.invite());
  const auto read = later.pair->guest().read(later.invitation);
  ASSERT_TRUE(read);
  EXPECT_EQ(read.value().summary().recognition.kind,
            Recognition::Kind::recognised);
  ASSERT_NO_FATAL_FAILURE(later.visit_with(later.invitation));
  ASSERT_NO_FATAL_FAILURE(later.answer());
  const auto* identified =
      later.host.seen.last<SharedSession::PlayerIdentified>();
  ASSERT_NE(identified, nullptr);
  ASSERT_TRUE(identified->recognition.device);
  EXPECT_EQ(identified->recognition.device->nickname, "Guest pad");
}

class KakusanStunServers : public PairStageTest {};

TEST_F(KakusanStunServers, CanChangeWhileKakusanLives) {
  using Unavailable = libchiaking::kakusan::Kakusan::Unavailable;
  const std::vector<std::string> crowded(
      libchiaking::kakusan::Kakusan::most_stun_servers + 1, "192.0.2.10:3478");
  const std::vector<std::string> blank{""};
  const std::vector<std::string> long_name{
      std::string(libchiaking::kakusan::Kakusan::longest_stun_server + 1, 's')};
  for (const auto* servers : {&crowded, &blank, &long_name}) {
    const auto changed = stage.pair->host().use_stun_servers(*servers);
    ASSERT_FALSE(changed);
    EXPECT_EQ(changed.error(), Unavailable::invalid_stun_servers);
  }
  const std::vector<std::string> unreachable{"192.0.2.10:3478"};
  EXPECT_TRUE(stage.pair->host().use_stun_servers(unreachable));
  EXPECT_TRUE(stage.pair->guest().use_stun_servers(unreachable));
  ASSERT_NO_FATAL_FAILURE(stage.verified());
  ASSERT_NO_FATAL_FAILURE(stage.sit(seat(1)));
  const auto asked = stage.pair->network().sent();
  EXPECT_TRUE(std::ranges::any_of(
      asked, [](const auto& endpoint) { return endpoint.port == 3478; }));
  EXPECT_TRUE(stage.pair->host().use_stun_servers({}));
}

}  // namespace
}  // namespace kakusan_test
