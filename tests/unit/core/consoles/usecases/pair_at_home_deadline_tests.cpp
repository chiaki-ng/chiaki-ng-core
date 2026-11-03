#include "src/core/net/ports.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/consoles/usecases/home_rig.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace consoles_test {
namespace {

using libchiaking::consoles::Firmware;
using libchiaking::consoles::Generation;
using libchiaking::consoles::HomePairing;
using libchiaking::consoles::PairingFailure;
using libchiaking::platform::StopToken;
using PairingReason = PairingFailure::Reason;

static_assert(HomePairing::deadline == std::chrono::milliseconds{30'000});
static_assert(HomePairing::wait_step == std::chrono::milliseconds{100});

class SteppingClock final : public libchiaking::platform::ClockProvider {
 public:
  explicit SteppingClock(std::uint64_t step_us) noexcept : step_us_(step_us) {}

  auto now_microseconds() const noexcept -> std::uint64_t override {
    const auto reading = reads_.fetch_add(1) + 1;
    if (on_read) on_read(reading);
    return reading * step_us_;
  }

  std::function<void(std::uint64_t)> on_read;

 private:
  std::uint64_t step_us_;
  mutable std::atomic<std::uint64_t> reads_{0};
};

class LeapingClock final : public libchiaking::platform::ClockProvider {
 public:
  static constexpr std::uint64_t tick_us = 10'000;

  auto now_microseconds() const noexcept -> std::uint64_t override {
    if (leap_once && leap_once->load() && !leapt_.exchange(true))
      now_us_.fetch_add(leap_us);
    return now_us_.fetch_add(tick_us);
  }

  const std::atomic<bool>* leap_once{};
  std::uint64_t leap_us{};

 private:
  mutable std::atomic<std::uint64_t> now_us_{5'000'000};
  mutable std::atomic<bool> leapt_{};
};

TEST(PairAtHomeDeadline, IsMetByASilentConsole) {
  Home<SteppingClock> home{std::uint64_t{1000}};
  HomeConsole silent{home.network};
  const auto user = home.signed_in.as("npsso", 7);
  HomePairing pairing{home.network, test_support::process_threading(),
                      home.crypto, home.clock, home.random};
  auto request = HomePairing::Request::of(
      *user, Firmware::newest(Generation::ps5), Home<>::pin());
  ASSERT_TRUE(request);
  const auto result =
      pairing.run(std::move(request.value()), "127.0.0.1", home.never,
                  libchiaking::rp_session_port, std::chrono::milliseconds{2});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, PairingReason::timed_out);
  EXPECT_TRUE(silent.hear_knock());
}

class PairAtHomeLeap : public ::testing::Test {
 protected:
  auto paired_after_a_leap_of(std::uint64_t leap_us) {
    home.clock.leap_once = &console.knock_answered;
    home.clock.leap_us = leap_us;
    console.serve("RES3", home.welcome());
    const auto result =
        home.known.pair_at_home(*user, Home<>::host("127.0.0.1"),
                                Generation::ps5, Home<>::pin(), home.never);
    console.stop_serving();
    return result;
  }

  Home<LeapingClock> home;
  HomeConsole console{home.network};
  std::shared_ptr<libchiaking::users::PsUser> user =
      home.signed_in.as("npsso", 7);
};

TEST_F(PairAtHomeLeap, StillPairsTwentyEightSecondsIn) {
  const auto result = paired_after_a_leap_of(28'000'000);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.value()->is_paired_with(*user));
}

TEST_F(PairAtHomeLeap, GivesUpThirtySecondsAfterItBegan) {
  const auto result = paired_after_a_leap_of(30'000'000);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, PairingReason::timed_out);
  EXPECT_TRUE(home.nothing_is_live());
}

TEST(PairAtHomeStop, StopsARunningPairing) {
  Home<SteppingClock> home{std::uint64_t{1}};
  HomeConsole silent{home.network};
  const auto user = home.signed_in.as("npsso", 7);
  const auto stop = std::make_shared<test_support::AtomicStop>();
  home.clock.on_read = [&stop](std::uint64_t reading) {
    if (reading == 4) stop->stopped.store(true);
  };
  const auto result =
      home.known.pair_at_home(*user, Home<>::host("127.0.0.1"), Generation::ps5,
                              Home<>::pin(), StopToken{stop});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, PairingReason::cancelled);
  EXPECT_TRUE(home.nothing_is_live());
}

}  // namespace
}  // namespace consoles_test
