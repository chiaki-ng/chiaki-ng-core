#include "src/core/net/ports.hpp"
#include "tests/unit/core/consoles/adapters/pairing/home_pairing_rig.hpp"

#include <libchiaking/testing/in_memory_network.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace consoles_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::Expected;
using libchiaking::consoles::HomePairing;
using Error = HomePairing::Error;
using Step = HomePairing::Step;
using Target = remote_play::RegistrationTarget;

class HomePairingAt : public ::testing::Test {
 protected:
  static auto request(Target target, bool with_account,
                      std::optional<std::string> online_id = std::nullopt)
      -> HomePairing::Request {
    return PairingTime::request(
        target, with_account ? PairingTime::account(1) : std::nullopt,
        std::move(online_id), 12345678, 4321);
  }

  auto pump_until_finished() -> Expected<void, Error> {
    Expected<void, Error> pumped{};
    const auto deadline = PairingTime::now() + std::chrono::seconds{10};
    while (PairingTime::now() < deadline) {
      pumped = pairing.pump_once();
      const auto step = pairing.step();
      if (step == Step::failed || step == Step::paired) break;
      PairingTime::pause(std::chrono::milliseconds{1});
    }
    return pumped;
  }

  libchiaking::testing::InMemoryNetwork network{PairingTime::threading(),
                                                PairingTime::steady()};
  libchiaking::platform::SteadyClock clock;
  test_support::CountingRandom random;
  XorCrypto crypto;
  HomePairing pairing{network, PairingTime::threading(), crypto, clock, random};
};

TEST_F(HomePairingAt, AnIdlePairingRefusesToPumpOrGiveAResult) {
  EXPECT_EQ(pairing.step(), Step::idle);
  const auto pumped = pairing.pump_once();
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), Error::invalid_state);
  const auto taken = pairing.take_result();
  ASSERT_FALSE(taken);
  EXPECT_EQ(taken.error(), Error::invalid_state);
  EXPECT_EQ(pairing.step(), Step::idle);
  EXPECT_FALSE(pairing.refusal().http_status);
  EXPECT_FALSE(pairing.refusal().ps_reason);
}

TEST_F(HomePairingAt, RefusesAnUnknownFamily) {
  for (const int target : {5, 9, 255}) {
    const auto unknown = pairing.start_at(
        request(static_cast<Target>(target), true), "127.0.0.1", 9295);
    ASSERT_FALSE(unknown) << target;
    EXPECT_EQ(unknown.error(), Error::invalid_request) << target;
    EXPECT_EQ(pairing.step(), Step::idle) << target;
  }
}

TEST_F(HomePairingAt, AsksAnUnknownPs4InTheLegacyDialect) {
  HomeConsole console{network, 0};
  console.serve("RES2", HomeConsole::bytes(HomeConsole::refusal));
  ASSERT_TRUE(pairing.start_at(request(Target::ps4_unknown, false, "online"),
                               "127.0.0.1", console.endpoint().port));
  const auto pumped = pump_until_finished();
  console.stop_serving();
  EXPECT_EQ(pairing.step(), Step::failed);
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), Error::refused);
  EXPECT_EQ(pairing.error(), Error::refused);
  EXPECT_EQ(pairing.refusal().http_status, std::uint16_t{403});
  EXPECT_EQ(pairing.refusal().ps_reason, std::uint32_t{0x80108b12});
  EXPECT_EQ(console.knock.size(), 5U);
  EXPECT_EQ(console.knock.substr(0, 4), "SRC2");
  const auto request_text = console.request_text();
  EXPECT_EQ(request_text.find("POST /sie/ps4/rp/sess/rgst HTTP/1.1\r\n"), 0U);
  EXPECT_EQ(request_text.find("RP-Version"), std::string::npos);
  EXPECT_NE(request_text.find("Content-Length: 524\r\n"), std::string::npos);
  const auto taken = pairing.take_result();
  ASSERT_FALSE(taken);
  EXPECT_EQ(taken.error(), Error::invalid_state);
}

TEST_F(HomePairingAt, PairsALegacyPs4ByOnlineIdThroughTheRefusal) {
  HomeConsole console{network, 0};
  console.serve("RES2", HomeConsole::bytes(HomeConsole::refusal));
  ASSERT_TRUE(pairing.start_at(request(Target::ps4_8, false, "online"),
                               "127.0.0.1", console.endpoint().port));
  const auto pumped = pump_until_finished();
  console.stop_serving();
  EXPECT_EQ(pairing.step(), Step::failed);
  ASSERT_FALSE(pumped);
  EXPECT_EQ(pumped.error(), Error::refused);
  EXPECT_EQ(console.knock.substr(0, 4), "SRC2");
  EXPECT_EQ(console.request_text().find("POST /sce/rp/regist HTTP/1.1\r\n"),
            0U);
}

TEST_F(HomePairingAt, AnIdleWaitEndsOnAWake) {
  const auto idle = PairingTime::now();
  auto waker = libchiaking::platform::Thread::start(
                   PairingTime::threading(), "test-waker",
                   libchiaking::platform::ThreadRole::unspecified,
                   [this] {
                     PairingTime::pause(std::chrono::milliseconds{40});
                     pairing.wake();
                   })
                   .value();
  pairing.wait(std::chrono::milliseconds{5000});
  const auto woken = PairingTime::now() - idle;
  static_cast<void>(waker.join());
  EXPECT_GE(woken, std::chrono::milliseconds{30});
  EXPECT_LT(woken, std::chrono::seconds{2});
}

TEST_F(HomePairingAt, AWaitDrivenPairingFinishesWithoutSpinning) {
  HomeConsole console{network, 0};
  console.serve("RES3", HomeConsole::bytes(HomeConsole::refusal));
  ASSERT_TRUE(pairing.start_at(request(Target::ps5_1, true), "127.0.0.1",
                               console.endpoint().port));
  int waits = 0;
  auto step = pairing.step();
  const auto started = PairingTime::now();
  while (PairingTime::now() - started < std::chrono::seconds{10}) {
    pairing.wait(std::chrono::milliseconds{1000});
    ++waits;
    static_cast<void>(pairing.pump_once());
    step = pairing.step();
    if (step == Step::failed || step == Step::paired) break;
  }
  console.stop_serving();
  EXPECT_EQ(step, Step::failed);
  EXPECT_LT(waits, 100);
  EXPECT_LT(PairingTime::now() - started, std::chrono::seconds{3});
  EXPECT_TRUE(console.knock.starts_with("SRC3")) << console.knock;
}

TEST_F(HomePairingAt, AnUnresolvableHostIsANetworkFailure) {
  const auto unresolved =
      pairing.start_at(request(Target::ps5_1, true), "unresolvable.invalid");
  ASSERT_FALSE(unresolved);
  EXPECT_EQ(unresolved.error(), Error::network_failure);
  EXPECT_EQ(pairing.step(), Step::idle);
}

TEST_F(HomePairingAt, RefusesAModernFamilyWithoutAnAccountId) {
  for (const auto target : {Target::ps5_1, Target::ps4_10}) {
    const auto refused =
        pairing.start_at(request(target, false, "online"), "127.0.0.1",
                         libchiaking::rp_session_port);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error(), Error::invalid_request);
  }
  EXPECT_EQ(pairing.step(), Step::idle);
}

TEST_F(HomePairingAt, RefusesALegacyPs4WithoutAnOnlineId) {
  const auto nobody =
      pairing.start_at(request(Target::ps4_9, false), "127.0.0.1");
  ASSERT_FALSE(nobody);
  EXPECT_EQ(nobody.error(), Error::invalid_request);
  EXPECT_EQ(pairing.step(), Step::idle);
}

TEST_F(HomePairingAt, RefusesAnInvalidHost) {
  for (const auto& invalid :
       {std::string{}, std::string(256, 'a'), std::string{"a\0b", 3}}) {
    const auto refused =
        pairing.start_at(request(Target::ps5_1, true), invalid);
    ASSERT_FALSE(refused) << invalid.size();
    EXPECT_EQ(refused.error(), Error::invalid_request) << invalid.size();
  }
  EXPECT_EQ(pairing.step(), Step::idle);
}

TEST_F(HomePairingAt, StartsAtAHostNameAndRefusesASecondStart) {
  HomeConsole console{network, 0};
  ASSERT_TRUE(pairing.start_at(request(Target::ps5_1, true), "localhost",
                               console.endpoint().port));
  static_cast<void>(console.await_knock());
  EXPECT_EQ(console.knock.find("SRC"), 0U);
  EXPECT_NE(pairing.step(), Step::idle);
  const auto again = pairing.start_at(request(Target::ps5_1, true), "localhost",
                                      console.endpoint().port);
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error(), Error::invalid_state);
  EXPECT_TRUE(pairing.cancel());
}

}  // namespace
}  // namespace consoles_test
