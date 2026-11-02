#include <libchiaking/play.hpp>
#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>

namespace play_test {
namespace {

namespace catalog = libchiaking::catalog;
namespace play = libchiaking::play;

class SessionEvents : public ::testing::Test {
 protected:
  class CountingEvents final : public play::SessionEvents {
   public:
    void on_state(play::SessionState state) override { last_ = state; }
    void on_ended(const play::Outcome& outcome) override {
      cause_ = outcome.cause();
    }
    void on_seat_refused(const play::SeatRefusal& refusal) override {
      kind_ = refusal.kind();
    }

    auto last() const -> play::SessionState { return last_; }
    auto cause() const -> std::optional<play::EndReason::Cause> {
      return cause_;
    }
    auto kind() const -> std::optional<play::SeatRefusal::Kind> {
      return kind_;
    }

   private:
    play::SessionState last_{play::SessionState::connecting};
    std::optional<play::EndReason::Cause> cause_;
    std::optional<play::SeatRefusal::Kind> kind_;
  };
};

TEST_F(SessionEvents, CanBeImplementedByTheApp) {
  CountingEvents counted;
  play::SessionEvents& events = counted;
  events.on_state(play::SessionState::playing);
  events.on_ended(
      play::Outcome::not_started(play::EndReason::Cause::provider_unavailable));
  events.on_seat_refused(play::SeatRefusal::by_console(
      libchiaking::controllers::SeatNumber::first(), 0xff));
  events.on_connect_progress(
      {play::ConnectStep::waking_console, std::chrono::microseconds{5}, 2});
  events.on_launch_progress(
      play::LaunchProgress{catalog::ServiceGeneration::ps5_cloud});
  events.on_datacenters_offered(play::DatacenterOffer{});
  events.on_login_pin_needed(true);
  events.on_keyboard_opened("text");
  events.on_seat_joined(libchiaking::controllers::SeatNumber::first(),
                        {1, 2, 3});
  EXPECT_EQ(counted.last(), play::SessionState::playing);
  EXPECT_EQ(counted.cause(), play::EndReason::Cause::provider_unavailable);
  EXPECT_EQ(counted.kind(), play::SeatRefusal::Kind::timed_out);
}

}  // namespace
}  // namespace play_test
