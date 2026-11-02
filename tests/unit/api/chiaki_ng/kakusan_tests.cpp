#include "tests/unit/api/chiaki_ng/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace chiaki_ng_test {
namespace {

using ChiakiNgKakusan = ChiakiNgTest;

#if LIBCHIAKING_TESTS_HAVE_KAKUSAN
class SharedEvents final : public kakusan::SharedSessionObserver {
 public:
  explicit SharedEvents(platform::ThreadingProvider& threading)
      : mutex_(threading) {}

  void on_event(kakusan::SharedSessionEvent) override {
    const platform::Lock lock{mutex_};
    ++seen_;
  }

 private:
  platform::Mutex mutex_;
  std::size_t seen_{};
};

TEST_F(ChiakiNgKakusan, IsWiredOnceWithTheDeviceStoreAndStunServers) {
  Household without{false};
  const auto unavailable = without.chiaki.kakusan();
  ASSERT_FALSE(unavailable);
  EXPECT_EQ(unavailable.error(),
            kakusan::Kakusan::Unavailable::device_store_unavailable);

  Household home;
  const auto shared = home.chiaki.kakusan();
  ASSERT_TRUE(shared);
  const auto again = home.chiaki.kakusan();
  ASSERT_TRUE(again);
  EXPECT_EQ(again.value().this_device().key(),
            shared.value().this_device().key());
  const std::vector<std::string> servers{"stun.example.test:3478"};
  EXPECT_TRUE(home.chiaki.use_stun_servers(servers));
  const std::vector<std::string> too_long{
      std::string(kakusan::Kakusan::longest_stun_server + 1, 's')};
  const auto invalid = home.chiaki.use_stun_servers(too_long);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(),
            kakusan::Kakusan::Unavailable::invalid_stun_servers);
  EXPECT_FALSE(shared.value().read("RCP1:x"));

  const auto added = home.chiaki.users().add_home_user(AccountId{home_account},
                                                       "Living Room Player");
  ASSERT_TRUE(added);
  play::testing::ScriptedLink link{home.threading, home.steady};
  link.connect();
  const auto started = home.chiaki.play().start_scripted(
      added.value(), Where::home, link, {}, home.media, home.events);
  ASSERT_TRUE(started);
  home.events.until_state(SessionState::playing);
  SharedEvents observer{home.threading};
  auto root = home.chiaki.kakusan();
  ASSERT_TRUE(root);
  auto sharing = root.value().share(
      started.value()->share(), *kakusan::SharingRules::create(4, 1), observer);
  ASSERT_TRUE(sharing);
  const auto twice = root.value().share(
      started.value()->share(), *kakusan::SharingRules::create(4, 1), observer);
  ASSERT_FALSE(twice);
  EXPECT_EQ(twice.error().kind(), kakusan::Refusal::Kind::busy);
  sharing.value()->stop_sharing();
  home.end(*started.value());
}
#else
TEST_F(ChiakiNgKakusan, IsReportedUnbuiltAndTheRestStillWorks) {
  using Unavailable = kakusan::Kakusan::Unavailable;
  for (const bool with_devices : {false, true}) {
    Household home{with_devices};
    const auto shared = home.chiaki.kakusan();
    ASSERT_FALSE(shared);
    EXPECT_EQ(shared.error(), Unavailable::not_built);
    const std::vector<std::string> servers{"stun.example.test:3478"};
    const auto changed = home.chiaki.use_stun_servers(servers);
    ASSERT_FALSE(changed);
    EXPECT_EQ(changed.error(), Unavailable::not_built);
    EXPECT_EQ(kakusan::Kakusan::name(Unavailable::not_built), "NOT_BUILT");
    EXPECT_EQ(home.chiaki.play().live(), nullptr);
  }
}
#endif

}  // namespace
}  // namespace chiaki_ng_test
