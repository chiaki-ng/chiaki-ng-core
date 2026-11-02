#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/manual_pair_rig.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace kakusan_test {

using test_support::play::SourceStream;
namespace {

using std::chrono::milliseconds;

class ManualPairCalls : public ManualWorldTest {};

TEST_F(ManualPairCalls, RefuseInvalidArgumentsWithoutAnEvent) {
  auto& host = *world.host;
  const GuestId guest{GuestId::Bytes{std::byte{1}}};
  const std::string oversized(manual::max_block_size + 1, 'A');
  EXPECT_TRUE(ManualPair::failed_with(
      host.source_joined(manual::Seat{0, 1}, ManualPair::seat(1)),
      Error::invalid_argument));
  EXPECT_TRUE(ManualPair::failed_with(
      host.source_joined(manual::Seat{2, 0}, ManualPair::seat(1)),
      Error::invalid_argument));
  EXPECT_TRUE(ManualPair::failed_with(
      host.source_join_failed(manual::Seat{0, 1}), Error::invalid_argument));
  EXPECT_TRUE(ManualPair::failed_with(
      host.source_join_failed(manual::Seat{2, 0}), Error::invalid_argument));
  EXPECT_TRUE(
      ManualPair::failed_with(host.import_reply({}), Error::invalid_argument));
  EXPECT_TRUE(ManualPair::failed_with(host.import_reply(oversized),
                                      Error::invalid_block));
  EXPECT_TRUE(ManualPair::failed_with(host.enter_code(guest, {}),
                                      Error::invalid_argument));
  EXPECT_TRUE(ManualPair::failed_with(host.set_local_seats(0),
                                      Error::invalid_argument));
  EXPECT_TRUE(
      ManualPair::failed_with(host.set_capacity(0), Error::invalid_argument));
  EXPECT_TRUE(world.side.events.empty());

  ManualPlayer player{world.lan, 0x33, world.clock};
  ASSERT_TRUE(player.guest);
  EXPECT_TRUE(ManualPair::failed_with(player.guest->accept_invitation({}),
                                      Error::invalid_argument));
  EXPECT_TRUE(ManualPair::failed_with(
      player.guest->accept_invitation(oversized), Error::invalid_block));
  EXPECT_TRUE(ManualPair::failed_with(player.guest->enter_code({}),
                                      Error::invalid_argument));
  EXPECT_EQ(player.guest->state(), Progress::waiting);
}

TEST_F(ManualPairCalls, SurfaceTheEnginesErrors) {
  auto& host = *world.host;
  EXPECT_TRUE(ManualPair::failed_with(host.invite(), Error::source_ended));
  auto info = SourceStream::description();
  EXPECT_TRUE(ManualPair::failed_with(
      host.source_started(info, SourceStream::video_fps),
      Error::source_format));
  auto crowded = info;
  crowded.profiles.resize(manual::Host::max_video_profiles + 1,
                          info.profiles.front());
  EXPECT_TRUE(ManualPair::failed_with(host.source_started(crowded, 60),
                                      Error::source_format));
  auto bloated = info;
  bloated.profiles.front().header.resize(manual::Host::max_profile_header_size +
                                         1);
  EXPECT_TRUE(ManualPair::failed_with(host.source_started(bloated, 60),
                                      Error::source_format));

  manual::MediaTuning tuning;
  tuning.queue_age_min = milliseconds{100};
  tuning.queue_age_max = milliseconds{400};
  tuning.queue_age_round_trips = 3;
  tuning.startup_grace = milliseconds{8000};
  tuning.drop_seconds_to_end = 5;
  tuning.acknowledgement_timeout = milliseconds{6000};
  tuning.queue_bytes = 8192U * 1024U;
  tuning.queue_packets = 16384;
  tuning.keyframe_interval = milliseconds{500};
  host.set_media_tuning(tuning);
  host.set_media_tuning(manual::MediaTuning{});
  EXPECT_TRUE(
      ManualPair::failed_with(host.set_local_seats(9), Error::no_capacity));
  EXPECT_TRUE(ManualPair::failed_with(
      host.source_joined(manual::Seat{2, 1}, ManualPair::seat(1)),
      Error::internal));
  EXPECT_TRUE(ManualPair::failed_with(host.import_reply("RCP1:AAAA"),
                                      Error::invalid_block));

  world.players.push_back(
      std::make_unique<ManualPlayer>(world.lan, 0x55, world.clock));
  auto& player = world.latest();
  ASSERT_TRUE(player.guest);
  auto& guest = *player.guest;
  EXPECT_EQ(guest.state(), Progress::waiting);
  EXPECT_TRUE(ManualPair::failed_with(guest.accept_invitation("RCP1:AAAA"),
                                      Error::invalid_block));
  EXPECT_TRUE(
      ManualPair::failed_with(guest.enter_code("1234"), Error::internal));
  EXPECT_TRUE(
      ManualPair::failed_with(guest.controller_ready(), Error::internal));
  EXPECT_EQ(guest.next_wait(milliseconds{25}), milliseconds{25});
  guest.leave();
  EXPECT_EQ(guest.state(), Progress::gone);
  EXPECT_EQ(guest.next_wait(milliseconds{25}), milliseconds{0});
  EXPECT_TRUE(ManualPair::pump(guest));
  ASSERT_NE(player.last_state(), nullptr);
  EXPECT_EQ(player.last_state()->state, Progress::gone);
  EXPECT_EQ(player.last_state()->reason, manual::Reason::user);
}

TEST_F(ManualPairCalls, LetTheLaunchCodecDecideTheHdrRefusal) {
  auto& host = *world.host;
  host.source_keys(SourceStream::takion_version,
                   SourceStream::receive_keys());
  auto info = SourceStream::description();
  info.picture = libchiaking::media::GrantedPicture{};
  info.picture->codec = libchiaking::media::VideoCodec::h265_hdr;
  EXPECT_TRUE(ManualPair::failed_with(
      host.source_started(info, SourceStream::video_fps), Error::codec));
  info.picture->codec = libchiaking::media::VideoCodec::h265;
  EXPECT_TRUE(host.source_started(info, SourceStream::video_fps));
  info.picture->codec = libchiaking::media::VideoCodec::h264;
  EXPECT_TRUE(host.source_started(info, SourceStream::video_fps));
}

TEST_F(ManualPairCalls, AreSerialisedAcrossThreads) {
  ASSERT_NO_FATAL_FAILURE(world.start_source());
  ASSERT_NO_FATAL_FAILURE(world.join(0x40));
  auto& player = world.latest();
  const auto moon = static_cast<std::uint32_t>(
      libchiaking::controllers::ControllerButton::moon);
  std::atomic<bool> stop{false};
  std::atomic<std::size_t> calls{0};
  const auto hammering = [&] {
    libchiaking::controllers::ControllerState state;
    std::uint32_t round = 0;
    while (!stop.load()) {
      state.buttons = (round & 1U) != 0 ? moon : 0U;
      state.left_x = static_cast<std::int16_t>(round % 30000U);
      EXPECT_TRUE(player.guest->set_controller(state));
      static_cast<void>(player.guest->next_wait(milliseconds{10}));
      static_cast<void>(world.host->next_wait(milliseconds{10}));
      EXPECT_EQ(world.host->seats().capacity, 4U);
      ++round;
      calls.fetch_add(1);
    }
  };
  auto hammer = libchiaking::platform::Thread::start(
                    TestPlatform::threading(), "hammer",
                    libchiaking::platform::ThreadRole::unspecified, hammering)
                    .value();
  const auto moon_seen = [&]() {
    const auto inputs = world.side.inputs();
    return std::ranges::any_of(inputs, [&](const auto& state) {
      return state.controller_id == 1 && state.buttons == moon;
    });
  };
  for (std::size_t index = 0; index < 400; ++index) {
    world.step(500);
    if (HasFatalFailure()) break;
  }
  const auto deadline =
      TestPlatform::steady().now_microseconds() +
      static_cast<std::uint64_t>(
          std::chrono::microseconds{std::chrono::seconds{10}}.count());
  while (!HasFatalFailure() && !moon_seen() &&
         TestPlatform::steady().now_microseconds() < deadline)
    world.step(500);
  stop.store(true);
  static_cast<void>(hammer.join());
  EXPECT_GT(calls.load(), 0U);
  EXPECT_TRUE(moon_seen());
  EXPECT_EQ(player.guest->state(), Progress::playing);
}

TEST_F(ManualPairCalls, DoNotDeliverQueuedEventsToADestroyedHost) {
  ASSERT_NO_FATAL_FAILURE(world.start_source());
  EXPECT_TRUE(world.host->invite());
  EXPECT_TRUE(world.host->set_capacity(6));
  EXPECT_EQ(world.host->next_wait(milliseconds{1000}), milliseconds{0});
  EXPECT_TRUE(world.side.events.empty());
}

TEST_F(ManualPairCalls, KeepWorkingOnAMovedHost) {
  ASSERT_NO_FATAL_FAILURE(world.start_source());
  manual::Host moved{std::move(*world.host)};
  world.side.host = &moved;
  world.host.reset();
  EXPECT_TRUE(moved.set_capacity(6));
  EXPECT_EQ(moved.next_wait(milliseconds{1000}), milliseconds{0});
  EXPECT_TRUE(world.side.events.empty());
  EXPECT_TRUE(ManualPair::pump(moved));
  EXPECT_EQ(world.side.count<manual::host_event::SeatsChanged>(), 1U);
  EXPECT_EQ(moved.seats().capacity, 6U);
}

}  // namespace
}  // namespace kakusan_test
