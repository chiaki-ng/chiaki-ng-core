#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/guest_visit/guest_visit_core_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace kakusan_test {

using test_support::play::SourceStream;
namespace {

namespace control = libchiaking::net::kakusan::control;
namespace wire = libchiaking::net::kakusan;

class GuestVisitCoreSession : public GuestVisitCoreTest {
 protected:
  static auto pressed() -> wire::InputState {
    wire::InputState state{};
    state.buttons = static_cast<std::uint32_t>(wire::Button::south) |
                    static_cast<std::uint32_t>(wire::Button::r2);
    state.left_x = -1200;
    state.right_y = 32767;
    state.left_trigger = 65535;
    state.touch = wire::TouchContact{true, 3, 100, 200};
    return state;
  }

  Rig rig;
};

TEST_F(GuestVisitCoreSession, ConnectsAndPairsIntoPlay) {
  EXPECT_FALSE(rig.guest->next_deadline_us());
  EXPECT_EQ(rig.guest->state(), Progress::waiting);
  rig.accept();
  ASSERT_TRUE(rig.guest->next_deadline_us());
  EXPECT_LE(*rig.guest->next_deadline_us(),
            rig.now + wire::invitation_lifetime_seconds *
                          libchiaking::microseconds_per_second);
  rig.run_until([&] { return rig.reply.has_value(); });
  EXPECT_EQ(rig.count<GuestVisit::Core::ReplyReady>(), 1U);
  EXPECT_FALSE(rig.guest->accept_invitation(rig.invitation_block, rig.clock()));

  rig.run_until([&] {
    return rig.host.state() == Progress::verifying &&
           rig.guest->state() == Progress::verifying &&
           rig.guest_code.has_value();
  });
  EXPECT_EQ(rig.count<GuestVisit::Core::CodeReady>(), 1U);
  EXPECT_EQ(rig.count<GuestVisit::Core::HostIdentified>(), 1U);
  for (const auto& output : rig.outputs) {
    if (const auto* host =
            std::get_if<GuestVisit::Core::HostIdentified>(&output)) {
      EXPECT_EQ(host->key, rig.host_identity.key().bytes());
      EXPECT_EQ(host->seats.capacity, 4U);
      EXPECT_EQ(host->seats.local_seats, 1U);
    }
  }
  EXPECT_FALSE(rig.guest->controller_ready(rig.now));
  EXPECT_FALSE(rig.guest->enter_code("12a4", rig.now));
  rig.pair();
  EXPECT_FALSE(rig.guest->enter_code(host_code_text, rig.now));
  rig.run_until([&] {
    return rig.host.state() == Progress::playing &&
           rig.guest->state() == Progress::playing && rig.sender != nullptr;
  });
  EXPECT_EQ(rig.host_media_started, 1U);

  const auto ready = rig.all<GuestVisit::Core::StreamReady>();
  ASSERT_EQ(ready.size(), 1U);
  EXPECT_EQ(ready.front().video_fps, SourceStream::video_fps);
  EXPECT_NE(rig.guest->forwarded(), nullptr);

  const auto requested = rig.all<GuestVisit::Core::ControllerRequested>();
  ASSERT_EQ(requested.size(), 1U);
  EXPECT_EQ(requested.front().seat, seat);
  EXPECT_EQ(requested.front().capacity, 4U);
  EXPECT_EQ(requested.front().local_seats, 1U);
  EXPECT_EQ(requested.front().allowed_buttons, wire::button_mask);
  EXPECT_EQ(rig.last_state().state, Progress::playing);
  EXPECT_EQ(rig.last_state().seat, seat);
  EXPECT_FALSE(rig.last_state().error);

  const auto deadline = rig.guest->next_deadline_us();
  ASSERT_TRUE(deadline);
  EXPECT_GT(*deadline, rig.now - 1'000);
  EXPECT_LE(*deadline, rig.now + wire::ping_interval_us);
}

TEST_F(GuestVisitCoreSession, SendsControllerStateAtTheStatePeriod) {
  rig.activate();
  const auto held = pressed();
  EXPECT_TRUE(rig.guest->set_controller(held, rig.now));
  wire::InputState invalid{};
  invalid.buttons = 1U << 20U;
  EXPECT_FALSE(rig.guest->set_controller(invalid, rig.now));
  rig.run(6);
  ASSERT_FALSE(rig.applied.empty());
  EXPECT_EQ(rig.applied.back(), held);

  const auto idle_from = rig.applied.size();
  rig.run(1000);
  const auto idle_frames = rig.applied.size() - idle_from;
  EXPECT_GE(idle_frames, 55U);
  EXPECT_LE(idle_frames, 62U);
  for (std::size_t index = idle_from + 1; index < rig.applied.size(); ++index) {
    EXPECT_EQ(rig.applied[index], held);
    EXPECT_LE(rig.applied_at[index] - rig.applied_at[index - 1],
              wire::state_period_us + 1'000);
  }

  const auto burst_from = rig.applied.size();
  for (std::int16_t tick = 0; tick < 200; ++tick) {
    auto moving = held;
    moving.left_x = tick;
    EXPECT_TRUE(rig.guest->set_controller(moving, rig.now));
    rig.step();
  }
  const auto burst_frames = rig.applied.size() - burst_from;
  EXPECT_GE(burst_frames, 30U);
  EXPECT_LE(burst_frames, 50U);
  for (std::size_t index = burst_from + 1; index < rig.applied.size(); ++index)
    EXPECT_GE(rig.applied_at[index] - rig.applied_at[index - 1],
              wire::state_min_gap_us);
}

TEST_F(GuestVisitCoreSession, StopsRumbleWhenFeedbackStopsArriving) {
  rig.activate();
  rig.apply(
      rig.host.handle(libchiaking::kakusan::RumbleSampled{rig.now, 900, 300}));
  rig.run(100);
  ASSERT_FALSE(rig.rumbles().empty());
  EXPECT_EQ(rig.rumbles().back().low_frequency, 900U);
  EXPECT_EQ(rig.rumbles().back().high_frequency, 300U);
  rig.wire_link().drop = [](TransportRole role, Lane lane,
                            const std::vector<std::byte>& bytes) {
    return role == TransportRole::host && lane == Lane::record &&
           bytes.size() >= 4 && bytes[2] == std::byte{'F'};
  };
  const auto dropped_at = rig.now;
  std::optional<std::uint64_t> stopped_at;
  for (std::size_t index = 0; index < 400 && !stopped_at; ++index) {
    rig.step();
    const auto latest = rig.rumbles().back();
    if (latest.low_frequency == 0 && latest.high_frequency == 0)
      stopped_at = rig.now;
  }
  ASSERT_TRUE(stopped_at);
  EXPECT_GE(*stopped_at - dropped_at,
            libchiaking::kakusan::rumble_stop_us - 20'000);
  EXPECT_LE(*stopped_at - dropped_at,
            libchiaking::kakusan::rumble_stop_us + 20'000);
}

TEST_F(GuestVisitCoreSession, DeliversAndAcknowledgesTheSourceMedia) {
  rig.activate();
  const auto expected_info = SourceVectors::shared_stream().description;
  ASSERT_TRUE(rig.sender);
  EXPECT_EQ(rig.sender->controller().bytes_in_flight(), 0U);
  for (std::size_t index = 0; index < SourceStream::vectors.size(); ++index)
    rig.forward(index);
  rig.step();
  EXPECT_GT(rig.sender->controller().bytes_in_flight(), 0U);
  rig.run(60);
  EXPECT_GT(rig.acks, 0U);
  EXPECT_TRUE(rig.sender->controller().has_rtt_sample());
  EXPECT_EQ(rig.sender->controller().bytes_in_flight(), 0U);
  EXPECT_FALSE(rig.sender->failure());

  ASSERT_FALSE(rig.media.descriptions.empty());
  const auto& description = rig.media.descriptions.back();
  EXPECT_EQ(description.audio_header, expected_info.audio_header);
  EXPECT_EQ(description.profiles, expected_info.profiles);
  EXPECT_EQ(description.audio.channels, 2U);
  EXPECT_EQ(description.audio.rate, 48000U);
  EXPECT_EQ(description.audio.frame_size, 960U);
  expect_source_media(rig.media);
  EXPECT_EQ(rig.idr_requests, 0U);
  EXPECT_FALSE(rig.host_got_idr());
  EXPECT_EQ(rig.guest->state(), Progress::playing);
}

TEST_F(GuestVisitCoreSession, LeavesAndClosesBothSides) {
  rig.activate();
  rig.guest->leave(rig.now);
  EXPECT_EQ(rig.count<GuestVisit::Core::StreamStopped>(), 0U);
  rig.collect();
  EXPECT_EQ(rig.count<GuestVisit::Core::StreamStopped>(), 1U);
  expect_silent_rumble(rig);
  rig.run(50);
  EXPECT_EQ(rig.guest->state(), Progress::gone);
  EXPECT_EQ(rig.last_state().state, Progress::gone);
  EXPECT_EQ(rig.last_state().reason, control::Reason::user);
  EXPECT_FALSE(rig.last_state().by_peer);
  EXPECT_FALSE(rig.last_state().error);
  EXPECT_TRUE(rig.wire_link().sides[1].closed);
  EXPECT_FALSE(rig.guest->next_deadline_us());
  EXPECT_EQ(rig.guest->forwarded(), nullptr);
  EXPECT_TRUE(std::ranges::any_of(rig.host_received, [](const auto& item) {
    return std::holds_alternative<control::Leave>(item);
  }));
  rig.run(1100);
  EXPECT_EQ(rig.host.state(), Progress::gone);
  EXPECT_EQ(rig.host_closed, 1U);
  EXPECT_FALSE(rig.host_shown);
  EXPECT_EQ(rig.count<GuestVisit::Core::StreamStopped>(), 1U);
  EXPECT_EQ(rig.count<GuestVisit::Core::StreamReady>(), 1U);
}

TEST_F(GuestVisitCoreSession, EndsPairingOnAWrongCode) {
  rig.connect();
  rig.pair("1234");
  rig.run(200);
  EXPECT_EQ(rig.guest->state(), Progress::gone);
  const auto final_state = rig.last_state();
  EXPECT_EQ(final_state.state, Progress::gone);
  EXPECT_EQ(final_state.error, Error::pairing_failed);
  EXPECT_FALSE(final_state.seat);
  EXPECT_EQ(rig.count<GuestVisit::Core::StreamReady>(), 0U);
  EXPECT_FALSE(rig.guest->next_deadline_us());
  rig.run(1100);
  EXPECT_EQ(rig.host.state(), Progress::gone);
  EXPECT_EQ(rig.host_shown, Error::pairing_failed);
  EXPECT_FALSE(rig.guest->accept_invitation(rig.invitation_block, rig.clock()));
  EXPECT_FALSE(rig.guest->enter_code(host_code_text, rig.now));
}

TEST_F(GuestVisitCoreSession, LeavesBeforeAReplyWithoutSendingOne) {
  rig.accept();
  rig.guest->leave(rig.now);
  rig.collect();
  EXPECT_EQ(rig.guest->state(), Progress::gone);
  EXPECT_EQ(rig.last_state().reason, control::Reason::user);
  EXPECT_FALSE(rig.guest->next_deadline_us());
  rig.run(10);
  EXPECT_EQ(rig.count<GuestVisit::Core::ReplyReady>(), 0U);
}

}  // namespace
}  // namespace kakusan_test
