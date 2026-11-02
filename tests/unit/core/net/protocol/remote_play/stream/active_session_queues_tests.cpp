#include "src/core/net/protocol/remote_play/stream/active_session.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace client_event = libchiaking::net::protocol::takion::client_event;
using remote_play::ActiveMediaQueue;
using Bytes = std::vector<std::byte>;

TEST(ActiveSessionMediaQueue, PreservesOrderAndBoundsMemory) {
  ActiveMediaQueue queue;
  queue.push(client_event::EncodedAudio{{std::byte{1}}});
  queue.push(client_event::EncodedVideo{{std::byte{2}}});
  queue.push(client_event::Haptics{{std::byte{3}}});
  queue.push(client_event::EncodedAudio{{std::byte{4}}});
  auto first = queue.take();
  auto second = queue.take();
  auto third = queue.take();
  auto fourth = queue.take();
  ASSERT_TRUE(first);
  ASSERT_TRUE(std::holds_alternative<client_event::EncodedAudio>(*first));
  ASSERT_TRUE(second);
  ASSERT_TRUE(std::holds_alternative<client_event::EncodedVideo>(*second));
  ASSERT_TRUE(third);
  ASSERT_TRUE(std::holds_alternative<client_event::Haptics>(*third));
  ASSERT_TRUE(fourth);
  ASSERT_TRUE(std::holds_alternative<client_event::EncodedAudio>(*fourth));
  EXPECT_EQ(std::get<client_event::EncodedAudio>(*first).bytes[0],
            std::byte{1});
  EXPECT_EQ(std::get<client_event::EncodedVideo>(*second).bytes[0],
            std::byte{2});
  EXPECT_EQ(std::get<client_event::Haptics>(*third).bytes[0], std::byte{3});
  EXPECT_EQ(std::get<client_event::EncodedAudio>(*fourth).bytes[0],
            std::byte{4});
  EXPECT_FALSE(queue.take());
  EXPECT_EQ(queue.dropped(), 0U);

  for (std::size_t i = 0; i < ActiveMediaQueue::audio_capacity + 5; ++i)
    queue.push(client_event::EncodedAudio{{static_cast<std::byte>(i)}});
  EXPECT_EQ(queue.dropped(), 6U);
  first = queue.take();
  ASSERT_TRUE(first);
  EXPECT_TRUE(std::get<client_event::EncodedAudio>(*first).bytes.empty());
  for (std::size_t i = 6; i < ActiveMediaQueue::audio_capacity + 5; ++i) {
    const auto event = queue.take();
    ASSERT_TRUE(event) << i;
    EXPECT_EQ(std::get<client_event::EncodedAudio>(*event).bytes,
              Bytes{static_cast<std::byte>(i)});
  }
  EXPECT_FALSE(queue.take());

  for (std::size_t i = 0; i <= ActiveMediaQueue::haptics_capacity; ++i)
    queue.push(client_event::Haptics{{static_cast<std::byte>(i)}});
  EXPECT_EQ(queue.dropped(), 7U);
  first = queue.take();
  ASSERT_TRUE(first);
  EXPECT_EQ(std::get<client_event::Haptics>(*first).bytes[0], std::byte{1});
  queue.clear();
  EXPECT_FALSE(queue.take());

  queue.push(
      client_event::EncodedAudio{Bytes(ActiveMediaQueue::max_bytes + 1)});
  EXPECT_EQ(queue.dropped(), 8U);
  first = queue.take();
  ASSERT_TRUE(first);
  EXPECT_TRUE(std::get<client_event::EncodedAudio>(*first).bytes.empty());
  queue.push(client_event::EncodedAudio{{std::byte{0x7f}}});
  queue.clear();
  EXPECT_FALSE(queue.take());
  EXPECT_EQ(queue.dropped(), 8U);
}

TEST(ActiveSessionMediaQueue, KeepsVoiceOverflowInItsOwnLane) {
  ActiveMediaQueue queue;
  client_event::EncodedAudio game_header{{std::byte{0x02}}};
  game_header.profile_header = true;
  queue.push(game_header);
  auto voice_header = game_header;
  voice_header.bytes = {std::byte{0x01}};
  voice_header.voice = true;
  queue.push(voice_header);
  queue.push(client_event::EncodedAudio{{std::byte{0x10}}});
  for (std::size_t i = 0; i < ActiveMediaQueue::audio_capacity + 5; ++i) {
    client_event::EncodedAudio voice{{std::byte{0x20}}};
    voice.voice = true;
    queue.push(voice);
  }
  const auto first = queue.take();
  ASSERT_TRUE(first);
  const auto& header = std::get<client_event::EncodedAudio>(*first);
  EXPECT_TRUE(header.profile_header);
  EXPECT_FALSE(header.voice);
  EXPECT_EQ(header.bytes, Bytes{std::byte{0x02}});
  bool voice_header_seen = false;
  bool game_frame_seen = false;
  bool voice_marker_seen = false;
  while (auto next = queue.take()) {
    const auto& audio = std::get<client_event::EncodedAudio>(*next);
    if (audio.voice && audio.profile_header) voice_header_seen = true;
    if (!audio.voice && !audio.profile_header) {
      EXPECT_EQ(audio.bytes, Bytes{std::byte{0x10}});
      game_frame_seen = true;
    }
    if (audio.voice && audio.bytes.empty()) voice_marker_seen = true;
  }
  EXPECT_TRUE(voice_header_seen);
  EXPECT_TRUE(game_frame_seen);
  EXPECT_TRUE(voice_marker_seen);
}

TEST(ActiveSessionMediaQueue, BoundsControllerSpeakerAudioInItsOwnLane) {
  ActiveMediaQueue queue;
  queue.push(client_event::EncodedAudio{{std::byte{0xa0}}});
  for (std::size_t i = 0; i <= ActiveMediaQueue::speaker_capacity; ++i)
    queue.push(client_event::ControllerSpeaker{{static_cast<std::byte>(i)}, 2});
  queue.push(client_event::Haptics{{std::byte{0x55}}});
  EXPECT_EQ(queue.dropped(), 1U);
  const auto first = queue.take();
  ASSERT_TRUE(first);
  EXPECT_TRUE(std::holds_alternative<client_event::EncodedAudio>(*first));
  for (std::size_t i = 1; i <= ActiveMediaQueue::speaker_capacity; ++i) {
    SCOPED_TRACE(i);
    const auto event = queue.take();
    ASSERT_TRUE(event);
    const auto& speaker = std::get<client_event::ControllerSpeaker>(*event);
    EXPECT_EQ(speaker.pad_index, 2U);
    EXPECT_EQ(speaker.bytes, Bytes{static_cast<std::byte>(i)});
  }
  const auto haptics = queue.take();
  ASSERT_TRUE(haptics);
  EXPECT_TRUE(std::holds_alternative<client_event::Haptics>(*haptics));
  queue.push(client_event::ControllerSpeaker{
      Bytes(ActiveMediaQueue::max_bytes + 1), 0});
  EXPECT_EQ(queue.dropped(), 2U);
  EXPECT_FALSE(queue.take());
}

TEST(ActiveSessionMediaQueue, IsolatesVideoFromAudioFloods) {
  ActiveMediaQueue queue;
  EXPECT_TRUE(
      queue.push(client_event::EncodedVideo{{std::byte{1}}, 7}).empty());
  for (std::size_t i = 0; i < 4 * ActiveMediaQueue::audio_capacity; ++i)
    EXPECT_TRUE(queue.push(client_event::EncodedAudio{{std::byte{2}}}).empty())
        << i;
  auto first = queue.take();
  ASSERT_TRUE(first);
  EXPECT_EQ(std::get<client_event::EncodedVideo>(*first).frame_index, 7U);
  queue.clear();

  for (const auto index : {65534U, 65535U, 0U, 1U})
    EXPECT_TRUE(queue
                    .push(client_event::EncodedVideo{
                        {std::byte{1}}, static_cast<std::uint16_t>(index)})
                    .empty())
        << index;
  EXPECT_FALSE(queue.video_overflows(1));
  EXPECT_TRUE(queue.video_overflows(ActiveMediaQueue::max_bytes));
  auto losses = queue.push(
      client_event::EncodedVideo{Bytes(ActiveMediaQueue::max_bytes), 2});
  ASSERT_EQ(losses.size(), 1U);
  EXPECT_EQ(losses[0].first, 65534U);
  EXPECT_EQ(losses[0].last, 1U);
  first = queue.take();
  ASSERT_TRUE(first);
  EXPECT_EQ(std::get<client_event::EncodedVideo>(*first).frame_index, 2U);
  losses = queue.push(
      client_event::EncodedVideo{Bytes(ActiveMediaQueue::max_bytes + 1), 99});
  ASSERT_EQ(losses.size(), 1U);
  EXPECT_EQ(losses[0].first, 99U);
  EXPECT_EQ(losses[0].last, 99U);
  for (unsigned index = 0; index < ActiveMediaQueue::video_capacity; ++index)
    EXPECT_TRUE(queue
                    .push(client_event::EncodedVideo{
                        {std::byte{1}}, static_cast<std::uint16_t>(index)})
                    .empty())
        << index;
  EXPECT_TRUE(queue.video_overflows(1));
  const auto dropped = queue.drop_video();
  ASSERT_TRUE(dropped);
  EXPECT_EQ(dropped->first, 0U);
  EXPECT_EQ(dropped->last, ActiveMediaQueue::video_capacity - 1);
  EXPECT_FALSE(queue.take());
  EXPECT_FALSE(queue.drop_video());
}

TEST(ActiveSessionMediaQueue, KeepsAudioProfileHeadersThroughOverflow) {
  ActiveMediaQueue queue;
  client_event::EncodedAudio header{{std::byte{6}, std::byte{0x10}}};
  header.profile_header = true;
  queue.push(client_event::EncodedAudio{{std::byte{0xa0}}});
  queue.push(header);
  for (std::size_t i = 0; i < ActiveMediaQueue::audio_capacity + 4; ++i)
    queue.push(client_event::EncodedAudio{{static_cast<std::byte>(i)}});
  auto first = queue.take();
  ASSERT_TRUE(first);
  const auto& kept = std::get<client_event::EncodedAudio>(*first);
  EXPECT_TRUE(kept.profile_header);
  EXPECT_EQ(kept.bytes, header.bytes);
  const auto second = queue.take();
  ASSERT_TRUE(second);
  EXPECT_TRUE(std::get<client_event::EncodedAudio>(*second).bytes.empty());
  EXPECT_FALSE(std::get<client_event::EncodedAudio>(*second).profile_header);
  std::size_t remaining = 0;
  while (auto event = queue.take()) {
    EXPECT_FALSE(std::get<client_event::EncodedAudio>(*event).profile_header);
    ++remaining;
  }
  EXPECT_LE(remaining + 2, ActiveMediaQueue::audio_capacity);

  auto newer = header;
  newer.bytes = {std::byte{8}, std::byte{0x10}};
  queue.push(header);
  queue.push(client_event::EncodedAudio{{std::byte{0xa1}}});
  queue.push(newer);
  for (std::size_t i = 0; i < ActiveMediaQueue::audio_capacity + 4; ++i)
    queue.push(client_event::EncodedAudio{{static_cast<std::byte>(i)}});
  first = queue.take();
  ASSERT_TRUE(first);
  EXPECT_EQ(std::get<client_event::EncodedAudio>(*first).bytes, newer.bytes);
  while (auto event = queue.take())
    EXPECT_FALSE(std::get<client_event::EncodedAudio>(*event).profile_header);
}

TEST(ActiveSessionFeedbackQueue, PreservesOrderAndBoundsCapacity) {
  remote_play::ActiveFeedbackQueue queue;
  queue.push(client_event::Rumble{1, 2, 3});
  queue.push(client_event::TriggerEffects{2, 4, 5});
  auto first = queue.take();
  const auto second = queue.take();
  ASSERT_TRUE(first);
  ASSERT_TRUE(std::holds_alternative<client_event::Rumble>(*first));
  ASSERT_TRUE(second);
  EXPECT_TRUE(std::holds_alternative<client_event::TriggerEffects>(*second));
  EXPECT_EQ(std::get<client_event::Rumble>(*first).left, 2U);
  EXPECT_FALSE(queue.take());
  for (std::size_t index = 0;
       index <= remote_play::ActiveFeedbackQueue::capacity; ++index)
    queue.push(client_event::Rumble{static_cast<std::uint8_t>(index), 0, 0});
  first = queue.take();
  ASSERT_TRUE(first);
  EXPECT_EQ(std::get<client_event::Rumble>(*first).player_index, 1U);
  for (std::size_t index = 2;
       index <= remote_play::ActiveFeedbackQueue::capacity; ++index) {
    const auto event = queue.take();
    ASSERT_TRUE(event) << index;
    EXPECT_EQ(std::get<client_event::Rumble>(*event).player_index, index);
  }
  EXPECT_FALSE(queue.take());
  queue.push(client_event::Rumble{1, 2, 3});
  queue.clear();
  EXPECT_FALSE(queue.take());
}

TEST(ActiveSessionEventInbox, HandsOutEventsInArrivalOrder) {
  remote_play::ActiveEventInbox inbox;
  EXPECT_FALSE(inbox.take_next());
  EXPECT_EQ(inbox.size(), 0U);

  inbox.on_event(client_event::Connected{});
  inbox.on_event(client_event::ProfileCommitted{7, {}});
  inbox.on_event(client_event::ControlReceived{
      libchiaking::net::protocol::takion::Heartbeat{}});
  EXPECT_EQ(inbox.size(), 3U);

  const auto first = inbox.take_next();
  ASSERT_TRUE(first);
  EXPECT_TRUE(std::holds_alternative<client_event::Connected>(*first));
  const auto second = inbox.take_next();
  ASSERT_TRUE(second);
  EXPECT_TRUE(std::holds_alternative<client_event::ProfileCommitted>(*second));
  const auto third = inbox.take_next();
  ASSERT_TRUE(third);
  EXPECT_TRUE(std::holds_alternative<client_event::ControlReceived>(*third));
  EXPECT_FALSE(inbox.take_next());
  EXPECT_EQ(inbox.size(), 0U);
}

}  // namespace
}  // namespace remote_play_test
