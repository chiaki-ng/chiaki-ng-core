#include "tests/unit/api/consoles/home_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace api_consoles_test {
namespace {

using libchiaking::consoles::Console;
using libchiaking::consoles::Generation;
using libchiaking::consoles::Presence;
using libchiaking::consoles::Unreachable;
using libchiaking::platform::IpAddress;
using libchiaking::platform::StopSource;
using libchiaking::testing::FakeConsole;

static_assert(Console::check_deadline == std::chrono::milliseconds{3000});
static_assert(Console::check_wait_step == std::chrono::milliseconds{150});

class ConsoleCheck : public HomeConsolesTest {};

TEST_F(ConsoleCheck, AsksTheConsoleItself) {
  FakeConsole ps4{home.network, loopback_at(FakeConsole::ps4_search_port)};
  FakeConsole ps5{home.network, loopback_at(FakeConsole::ps5_search_port)};
  const auto console = home.saved(living_room, Generation::ps5, "127.0.0.1");
  std::string ps4_search;
  std::string ps5_search;
  int reads = 0;
  home.clock.on_read = [&] {
    if (++reads != 2) return;
    ps4_search = ps4.receive();
    ps5_search = ps5.receive();
    ps5.reply(anonymous_reply);
    ps5.reply(ps4_reply);
    ps5.reply(ps5_reply);
  };
  const auto resting = console->check();
  ASSERT_TRUE(resting);
  EXPECT_EQ(resting.value(), Presence::rest_mode);
  EXPECT_EQ(ps4_search,
            std::string{"SRCH * HTTP/1.1\ndevice-discovery-protocol-version:"
                        "00020020\n"} +
                '\0');
  EXPECT_EQ(ps5_search,
            std::string{"SRCH * HTTP/1.1\ndevice-discovery-protocol-version:"
                        "00030010\n"} +
                '\0');
  EXPECT_EQ(console->presence(), Presence::rest_mode);
  EXPECT_EQ(console->name(), "Loopback fixture");
  ASSERT_TRUE(console->last_address());
  EXPECT_EQ(console->last_address()->text(), "127.0.0.1");
  const auto stored = home.store.console(hardware_id(living_room));
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->name.text(), "Loopback fixture");
  EXPECT_EQ(home.known.everything().size(), 1U);

  reads = 0;
  home.clock.on_read = [&] {
    if (++reads != 2) return;
    static_cast<void>(ps4.receive());
    static_cast<void>(ps5.receive());
    ps5.reply(ps5_renamed);
  };
  const auto ready = console->check();
  ASSERT_TRUE(ready);
  EXPECT_EQ(ready.value(), Presence::ready);
  EXPECT_EQ(console->presence(), Presence::ready);
  EXPECT_EQ(console->name(), "Renamed");
  EXPECT_EQ(home.http.requests, 0);
}

TEST_F(ConsoleCheck, AdmitsRepliesStrictly) {
  const std::string duplicated = std::string{ps5_reply} + "host-name:Second\n";
  const std::string oversized =
      std::string{ps5_reply} + "x-padding:" + std::string(4200, 'p') + "\n";
  FakeConsole ps5{home.network, loopback_at(FakeConsole::ps5_search_port)};
  const auto console = home.saved(living_room, Generation::ps5, "127.0.0.1");
  int reads = 0;
  home.clock.on_read = [&] {
    ++reads;
    if (reads == 2) {
      static_cast<void>(ps5.receive());
      ps5.reply(duplicated);
      ps5.reply(oversized);
    }
    if (reads == 3) home.clock.now.store(3'000'000);
  };
  const auto unanswered = console->check();
  ASSERT_FALSE(unanswered);
  EXPECT_EQ(unanswered.error(), Unreachable::timed_out);
  EXPECT_EQ(console->presence(), Presence::not_nearby);
  EXPECT_EQ(console->name(), "PS5");
}

class ConsoleCheckEnds : public ConsoleCheck {
 protected:
  void SetUp() override {
    console = home.saved(living_room, Generation::ps5, "127.0.0.1");
  }

  FakeConsole ps5{home.network, loopback_at(FakeConsole::ps5_search_port)};
  std::shared_ptr<Console> console;
};

TEST_F(ConsoleCheckEnds, AtItsDeadline) {
  int reads = 0;
  home.clock.on_read = [&] {
    if (++reads == 3) home.clock.now.store(3'000'000);
  };
  const auto silent = console->check();
  ASSERT_FALSE(silent);
  EXPECT_EQ(silent.error(), Unreachable::timed_out);
  EXPECT_EQ(console->presence(), Presence::not_nearby);
}

TEST_F(ConsoleCheckEnds, WhenStoppedBeforeItStarts) {
  StopSource stop;
  static_cast<void>(stop.request_stop());
  static_cast<void>(stop.request_stop());
  const auto cancelled = console->check(stop.token());
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error(), Unreachable::cancelled);
}

TEST_F(ConsoleCheckEnds,
       WhenStoppedWhileTheConsoleAnswersKeepingItsLastPresence) {
  int reads = 0;
  home.clock.on_read = [&] {
    if (++reads == 3) home.clock.now.store(3'000'000);
  };
  ASSERT_FALSE(console->check());
  ASSERT_EQ(console->presence(), Presence::not_nearby);
  home.clock.now.store(0);
  StopSource stop;
  reads = 0;
  home.clock.on_read = [&] {
    if (++reads != 2) return;
    while (ps5.try_receive()) ps5.reply(ps5_reply);
    static_cast<void>(stop.request_stop());
  };
  const auto cancelled = console->check(stop.token());
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error(), Unreachable::cancelled);
  EXPECT_EQ(console->presence(), Presence::not_nearby);
}

TEST_F(ConsoleCheckEnds, WithoutAnAddressOrAResolvableOne) {
  const auto nowhere = home.saved(den, Generation::ps4);
  const auto unaddressed = nowhere->check();
  ASSERT_FALSE(unaddressed);
  EXPECT_EQ(unaddressed.error(), Unreachable::no_address);
  const auto lost =
      home.saved("0a0b0c0d0e0f", Generation::ps5, "unresolvable.invalid");
  const auto unresolved = lost->check();
  ASSERT_FALSE(unresolved);
  EXPECT_EQ(unresolved.error(), Unreachable::network);
}

TEST_F(ConsoleCheckEnds, OnceTheConsoleIsForgotten) {
  EXPECT_TRUE(home.known.forget(*console));
  const auto forgotten = console->check();
  ASSERT_FALSE(forgotten);
  EXPECT_EQ(forgotten.error(), Unreachable::forgotten);
}

TEST_F(ConsoleCheck, RunsOverTheNetworkItIsGiven) {
  const auto tunnel = std::make_shared<Tunnel>(false);
  FakeConsole ps5{tunnel->memory, loopback_at(FakeConsole::ps5_search_port)};
  const auto console = home.saved(living_room, Generation::ps5, "127.0.0.1");
  int reads = 0;
  home.clock.on_read = [&] {
    if (++reads != 2) return;
    static_cast<void>(ps5.receive());
    ps5.reply(ps5_reply);
  };
  const auto over = libchiaking::consoles::HomeNetwork{tunnel};
  const auto resting = console->check({}, over);
  ASSERT_TRUE(resting);
  EXPECT_EQ(resting.value(), Presence::rest_mode);
  EXPECT_TRUE(home.network.sent().empty());
  EXPECT_EQ(tunnel->destinations(),
            (std::vector<IpAddress>{IpAddress::loopback_ipv4()}));
  ASSERT_TRUE(console->last_address());
  EXPECT_EQ(console->last_address()->text(), "127.0.0.1");
  const auto stored = home.store.console(hardware_id(living_room));
  ASSERT_TRUE(stored);
  ASSERT_TRUE(stored->last_address);
  EXPECT_EQ(stored->last_address->text(), "127.0.0.1");
  EXPECT_EQ(tunnel.use_count(), 2);

  home.clock.on_read = nullptr;
  const auto lost =
      home.saved("0a0b0c0d0e0f", Generation::ps5, "unresolvable.invalid");
  const auto unresolved = lost->check({}, over);
  ASSERT_FALSE(unresolved);
  EXPECT_EQ(unresolved.error(), Unreachable::network);
  EXPECT_TRUE(home.network.sent().empty());
}

}  // namespace
}  // namespace api_consoles_test
