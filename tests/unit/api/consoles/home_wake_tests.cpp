#include "tests/unit/api/consoles/home_rig.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace api_consoles_test {
namespace {

using libchiaking::AccountId;
using libchiaking::consoles::Generation;
using libchiaking::consoles::Host;
using libchiaking::consoles::WakeRefusal;
using libchiaking::platform::IpAddress;
using libchiaking::testing::FakeConsole;

class ConsoleWake : public HomeConsolesTest {
 protected:
  FakeConsole ps4{home.network, loopback_at(FakeConsole::ps4_search_port)};
  FakeConsole ps5{home.network, loopback_at(FakeConsole::ps5_search_port)};
};

TEST_F(ConsoleWake, SendsTheUsersCredentialToThePs5) {
  const auto user = home.home_user(7, "home_player");
  const auto living = home.paired(living_room, Generation::ps5, "127.0.0.1",
                                  AccountId{7}, "3a5f9c1e");
  EXPECT_TRUE(living->wake(*user));
  static constexpr char ps5_bytes[] =
      "WAKEUP * HTTP/1.1\n"
      "client-type:vr\n"
      "auth-type:R\n"
      "model:w\n"
      "app-type:r\n"
      "user-credential:979344414\n"
      "device-discovery-protocol-version:00030010\n\0";
  EXPECT_EQ(ps5.receive(),
            (std::string_view{ps5_bytes, sizeof(ps5_bytes) - 1}));
  EXPECT_FALSE(ps4.try_receive());
  EXPECT_EQ(home.http.requests, 0);
}

TEST_F(ConsoleWake, SendsTheUsersCredentialToThePs4) {
  const auto user = home.home_user(7, "home_player");
  const auto older =
      home.paired(den, Generation::ps4, "localhost", AccountId{7}, "2A");
  EXPECT_TRUE(older->wake(*user));
  const auto ps4_wake = ps4.receive();
  EXPECT_TRUE(ps4_wake.starts_with("WAKEUP * HTTP/1.1\n"));
  EXPECT_NE(ps4_wake.find("user-credential:42\n"), std::string::npos);
  EXPECT_NE(ps4_wake.find("device-discovery-protocol-version:00020020\n"),
            std::string::npos);
}

TEST_F(ConsoleWake, IsRefusedToAUserWithoutAUsablePairing) {
  const auto user = home.home_user(7, "home_player");
  const auto stranger = home.home_user(8, "guest_player");
  const auto living = home.paired(living_room, Generation::ps5, "127.0.0.1",
                                  AccountId{7}, "3a5f9c1e");
  const auto not_theirs = living->wake(*stranger);
  ASSERT_FALSE(not_theirs);
  EXPECT_EQ(not_theirs.error(), WakeRefusal::not_paired);
  const auto unparsable = home.paired("0a0b0c0d0e0f", Generation::ps5,
                                      "127.0.0.1", AccountId{7}, "not-hex");
  const auto unusable = unparsable->wake(*user);
  ASSERT_FALSE(unusable);
  EXPECT_EQ(unusable.error(), WakeRefusal::not_paired);
  EXPECT_FALSE(ps5.try_receive());
}

TEST_F(ConsoleWake, IsRefusedWithoutAReachableAddress) {
  const auto user = home.home_user(7, "home_player");
  const auto nowhere = home.paired("0a0b0c0d0e10", Generation::ps5, {},
                                   AccountId{7}, "3a5f9c1e");
  const auto unaddressed = nowhere->wake(*user);
  ASSERT_FALSE(unaddressed);
  EXPECT_EQ(unaddressed.error(), WakeRefusal::no_address);
  const auto lost =
      home.paired("0a0b0c0d0e11", Generation::ps5, "unresolvable.invalid",
                  AccountId{7}, "3a5f9c1e");
  const auto unresolved = lost->wake(*user);
  ASSERT_FALSE(unresolved);
  EXPECT_EQ(unresolved.error(), WakeRefusal::network);
  EXPECT_FALSE(ps5.try_receive());
}

TEST(ConsoleHosts, RefuseAnEmptyOrOverlongAddress) {
  EXPECT_EQ(Host::create("", Generation::ps5).error(), Host::Invalid::empty);
  EXPECT_EQ(Host::create(std::string(256, 'a'), Generation::ps5).error(),
            Host::Invalid::too_long);
}

TEST_F(ConsoleWake, IsRefusedOnceTheConsoleIsForgotten) {
  const auto user = home.home_user(7, "home_player");
  const auto living = home.paired(living_room, Generation::ps5, "127.0.0.1",
                                  AccountId{7}, "3a5f9c1e");
  EXPECT_TRUE(home.known.forget(*living));
  const auto forgotten = living->wake(*user);
  ASSERT_FALSE(forgotten);
  EXPECT_EQ(forgotten.error(), WakeRefusal::forgotten);
  EXPECT_EQ(home.http.requests, 0);
}

class ConsoleWakeOverATunnel : public HomeConsolesTest {
 protected:
  std::shared_ptr<Tunnel> tunnel = std::make_shared<Tunnel>(false);
  FakeConsole ps5{tunnel->memory, loopback_at(FakeConsole::ps5_search_port)};
  FakeConsole local{home.network, loopback_at(FakeConsole::ps5_search_port)};
};

TEST_F(ConsoleWakeOverATunnel, RunsOverTheNetworkItIsGiven) {
  const auto user = home.home_user(7, "home_player");
  const auto living = home.paired(living_room, Generation::ps5, "127.0.0.1",
                                  AccountId{7}, "3a5f9c1e");
  {
    const libchiaking::consoles::HomeNetwork over{tunnel};
    EXPECT_TRUE(living->wake(*user, over));
  }
  EXPECT_EQ(tunnel.use_count(), 1);
  EXPECT_NE(ps5.receive().find("user-credential:979344414\n"),
            std::string::npos);
  EXPECT_FALSE(local.try_receive());
  EXPECT_TRUE(home.network.sent().empty());
  EXPECT_EQ(tunnel->destinations(),
            (std::vector<IpAddress>{IpAddress::loopback_ipv4()}));

  EXPECT_TRUE(living->wake(*user));
  EXPECT_NE(local.receive().find("user-credential:979344414\n"),
            std::string::npos);
  EXPECT_FALSE(ps5.try_receive());
  EXPECT_EQ(home.network.sent().size(), 1U);
}

}  // namespace
}  // namespace api_consoles_test
