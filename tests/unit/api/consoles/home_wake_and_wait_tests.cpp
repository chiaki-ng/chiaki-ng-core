#include "tests/unit/api/consoles/home_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace api_consoles_test {
namespace {

using libchiaking::AccountId;
using libchiaking::consoles::Console;
using libchiaking::consoles::Generation;
using libchiaking::consoles::HomeNetwork;
using libchiaking::consoles::Presence;
using libchiaking::consoles::WakeRefusal;
using libchiaking::platform::IpAddress;
using libchiaking::platform::StopSource;
using libchiaking::testing::FakeConsole;

static_assert(Console::find_deadline == std::chrono::seconds{15});
static_assert(Console::ready_deadline == std::chrono::seconds{25});

class ConsoleWakeAndWait : public HomeConsolesTest {
 protected:
  void serve_every_read(FakeConsole& fake) {
    home.clock.on_read = [this, &fake] {
      fake.serve();
      home.clock.now.fetch_add(100'000);
    };
  }

  std::shared_ptr<libchiaking::users::PsUser> user =
      home.home_user(7, "home_player");
  StopSource stop;
};

TEST_F(ConsoleWakeAndWait, WakesAConsoleInRestModeAndAwaitsIt) {
  FakeConsole fake{home.network, {.name = "Living room"}};
  fake.rest();
  fake.wake_for(living_room_credential);
  const auto console = home.paired(living_room, Generation::ps5, "127.0.0.1",
                                   AccountId{7}, "3a5f9c1e");
  serve_every_read(fake);
  const auto woken = console->wake_and_wait(*user, stop.token());
  EXPECT_TRUE(woken);
  EXPECT_EQ(console->presence(), Presence::ready);
  EXPECT_EQ(console->name(), "Living room");
  ASSERT_EQ(fake.wakeups().size(), 1U);
  EXPECT_FALSE(fake.in_rest_mode());
  EXPECT_NE(fake.wakeups().front().find("user-credential:979344414\n"),
            std::string::npos);

  const auto again = console->wake_and_wait(*user, stop.token());
  EXPECT_TRUE(again);
  EXPECT_EQ(fake.wakeups().size(), 1U);
  EXPECT_EQ(home.http.requests, 0);
}

TEST_F(ConsoleWakeAndWait, FindsAConsoleWithoutASavedAddressFirst) {
  FakeConsole fake{home.network, {.name = "Living room"}};
  fake.rest();
  fake.wake_for(living_room_credential);
  const auto console =
      home.paired(living_room, Generation::ps5, {}, AccountId{7}, "3a5f9c1e");
  bool announced = false;
  home.clock.on_read = [&] {
    if (!std::exchange(announced, true)) fake.announce();
    fake.serve();
    home.clock.now.fetch_add(100'000);
  };
  EXPECT_TRUE(console->wake_and_wait(*user, stop.token()));
  EXPECT_EQ(console->presence(), Presence::ready);
  ASSERT_TRUE(console->last_address());
  EXPECT_EQ(console->last_address()->text(), "127.0.0.1");
  EXPECT_EQ(fake.wakeups().size(), 1U);
}

TEST_F(ConsoleWakeAndWait, RecoversADroppedWakeupByResending) {
  FakeConsole fake{home.network, {.name = "Living room"}};
  fake.rest();
  const auto console = home.paired(living_room, Generation::ps5, "127.0.0.1",
                                   AccountId{7}, "3a5f9c1e");
  home.clock.on_read = [&] {
    fake.serve();
    if (!fake.wakeups().empty()) fake.wake_for(living_room_credential);
    home.clock.now.fetch_add(1'000'000);
  };
  const auto woken = console->wake_and_wait(*user, stop.token());
  EXPECT_TRUE(woken);
  EXPECT_EQ(console->presence(), Presence::ready);
  EXPECT_EQ(console->name(), "Living room");
  EXPECT_GE(fake.wakeups().size(), 2U);
  EXPECT_FALSE(fake.in_rest_mode());
}

class ConsoleWakeAndWaitGivesUp : public ConsoleWakeAndWait {
 protected:
  void SetUp() override {
    console = home.paired(living_room, Generation::ps5, "127.0.0.1",
                          AccountId{7}, "3a5f9c1e");
  }

  std::shared_ptr<Console> console;
};

TEST_F(ConsoleWakeAndWaitGivesUp, WhenNoConsoleAnswersWithinTheFindDeadline) {
  home.clock.on_read = [&] { home.clock.now.fetch_add(1'000'000); };
  const auto begun = home.clock.now.load();
  const auto absent = console->wake_and_wait(*user, stop.token());
  ASSERT_FALSE(absent);
  EXPECT_EQ(absent.error(), WakeRefusal::not_found);
  EXPECT_GE(home.clock.now.load() - begun, 15'000'000U);
  EXPECT_EQ(console->presence(), Presence::not_nearby);
}

TEST_F(ConsoleWakeAndWaitGivesUp,
       WhenTheConsoleStaysInRestModePastTheReadyDeadline) {
  FakeConsole fake{home.network, {.name = "Living room"}};
  fake.rest();
  home.clock.on_read = [&] {
    fake.serve();
    home.clock.now.fetch_add(1'000'000);
  };
  const auto begun = home.clock.now.load();
  const auto asleep = console->wake_and_wait(*user, stop.token());
  ASSERT_FALSE(asleep);
  EXPECT_EQ(asleep.error(), WakeRefusal::still_in_rest_mode);
  EXPECT_GE(home.clock.now.load() - begun, 25'000'000U);
  EXPECT_GE(fake.wakeups().size(), 2U);
  EXPECT_EQ(console->presence(), Presence::rest_mode);
  const auto resent = fake.wakeups().size();

  const auto stranger = home.home_user(8, "guest_player");
  const auto not_theirs = console->wake_and_wait(*stranger, stop.token());
  ASSERT_FALSE(not_theirs);
  EXPECT_EQ(not_theirs.error(), WakeRefusal::not_paired);
  EXPECT_EQ(fake.wakeups().size(), resent);
  static_cast<void>(stop.request_stop());
  const auto cancelled = console->wake_and_wait(*user, stop.token());
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error(), WakeRefusal::cancelled);
  home.clock.on_read = nullptr;
}

TEST_F(ConsoleWakeAndWaitGivesUp, OnceTheConsoleIsForgotten) {
  EXPECT_TRUE(home.known.forget(*console));
  const auto forgotten = console->wake_and_wait(*user, stop.token());
  ASSERT_FALSE(forgotten);
  EXPECT_EQ(forgotten.error(), WakeRefusal::forgotten);
}

TEST_F(ConsoleWakeAndWait, OverANetworkThatCannotBroadcastAsksTheConsoleAlone) {
  const auto tunnel = std::make_shared<Tunnel>(false);
  FakeConsole fake{tunnel->memory, {.name = "Living room"}};
  fake.rest();
  fake.wake_for(living_room_credential);
  const auto console = home.paired(living_room, Generation::ps5, "127.0.0.1",
                                   AccountId{7}, "3a5f9c1e");
  serve_every_read(fake);
  {
    const HomeNetwork over{tunnel};
    EXPECT_TRUE(console->wake_and_wait(*user, stop.token(), over));
  }
  EXPECT_EQ(tunnel.use_count(), 1);
  EXPECT_EQ(console->presence(), Presence::ready);
  EXPECT_EQ(fake.wakeups().size(), 1U);
  EXPECT_FALSE(fake.in_rest_mode());
  EXPECT_TRUE(home.network.sent().empty());
  EXPECT_EQ(tunnel->destinations(),
            (std::vector<IpAddress>{IpAddress::loopback_ipv4()}));
  ASSERT_TRUE(console->last_address());
  EXPECT_EQ(console->last_address()->text(), "127.0.0.1");

  home.clock.on_read = nullptr;
  const auto nowhere = home.paired("0a0b0c0d0e10", Generation::ps5, {},
                                   AccountId{7}, "3a5f9c1e");
  const auto before = home.clock.now.load();
  const auto unaddressed =
      nowhere->wake_and_wait(*user, stop.token(), HomeNetwork{tunnel});
  ASSERT_FALSE(unaddressed);
  EXPECT_EQ(unaddressed.error(), WakeRefusal::no_address);
  EXPECT_EQ(home.clock.now.load(), before);
  EXPECT_EQ(tunnel.use_count(), 1);
}

TEST_F(ConsoleWakeAndWait, OverANetworkThatBroadcastsSearchesItToo) {
  const auto tunnel = std::make_shared<Tunnel>(true);
  FakeConsole fake{tunnel->memory, {.name = "Living room"}};
  fake.rest();
  fake.wake_for(living_room_credential);
  const auto console = home.paired(living_room, Generation::ps5, "127.0.0.1",
                                   AccountId{7}, "3a5f9c1e");
  serve_every_read(fake);
  EXPECT_TRUE(console->wake_and_wait(*user, stop.token(), HomeNetwork{tunnel}));
  EXPECT_EQ(fake.wakeups().size(), 1U);
  EXPECT_TRUE(home.network.sent().empty());
  const auto sent = tunnel->destinations();
  EXPECT_NE(std::ranges::find(sent, IpAddress::ipv4({192, 0, 2, 255})),
            sent.end());
  EXPECT_EQ(tunnel.use_count(), 1);
}

}  // namespace
}  // namespace api_consoles_test
