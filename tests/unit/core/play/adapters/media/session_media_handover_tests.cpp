#include "tests/unit/core/play/session_fakes.hpp"
#include "tests/support/play/session_rig.hpp"

#include "libchiaking/detail/diagnostics/session_journal.hpp"
#include "libchiaking/detail/media/media_sink.hpp"
#include "src/core/net/protocol/remote_play/stream/video_assembler.hpp"
#include "src/core/play/adapters/session/session_core.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/media/counted_allocations.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace play_test {

using test_support::LiveProviders;
using test_support::play::RecordedEvents;
using test_support::play::SignedInPlayer;
namespace {

namespace media = libchiaking::media;
namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::testing::ScriptedSession;

class SeenMedia final : public media::MediaSink {
 public:
  struct Seen {
    const std::byte* bytes{};
    const std::byte* padded{};
    std::size_t size{};
    std::size_t padded_size{};
    std::uint16_t frame_index{};
    std::byte first{};
    std::byte last{};
    bool tail_is_zero{};
  };

  struct Heard {
    const std::byte* bytes{};
    std::size_t size{};
    std::byte first{};
    std::byte last{};
    media::AudioUnit::Kind kind{media::AudioUnit::Kind::samples};
  };

  static constexpr std::size_t capacity = 16;

  SeenMedia() {
    frames_.reserve(capacity);
    sounds_.reserve(capacity);
  }

  void on_stream_description(const media::StreamDescription&) override {}

  auto on_video(const media::VideoUnit& unit) -> media::VideoResult override {
    const auto bytes = unit.frame().bytes();
    const auto padded = unit.frame().padded_bytes();
    EXPECT_LT(frames_.size(), capacity);
    frames_.push_back(
        {bytes.data(), padded.data(), bytes.size(), padded.size(),
         unit.frame().frame_index(), bytes.front(), bytes.back(),
         std::ranges::all_of(padded.subspan(bytes.size()), [](std::byte value) {
           return value == std::byte{};
         })});
    return {};
  }

  void on_video_gap() override {}

  auto on_audio(const media::AudioUnit& unit) -> media::AudioResult override {
    EXPECT_LT(sounds_.size(), capacity);
    sounds_.push_back({unit.bytes.data(), unit.bytes.size(), unit.bytes.front(),
                       unit.bytes.back(), unit.kind});
    return {};
  }

  void on_feedback(
      const libchiaking::controllers::ControllerFeedback&) override {}

  auto frames() const -> const std::vector<Seen>& { return frames_; }
  auto sounds() const -> const std::vector<Heard>& { return sounds_; }

 private:
  std::vector<Seen> frames_;
  std::vector<Heard> sounds_;
};

class SessionMediaHandover : public ::testing::Test {
 protected:
  struct Handed {
    const std::byte* bytes{};
    std::size_t size{};
    std::size_t allocations{};
  };

  static constexpr std::size_t count = 8;

  SessionMediaHandover() { handed_.reserve(count); }

  auto run(std::function<void(ScriptedSession&)> step)
      -> ::testing::AssertionResult {
    auto owned = std::make_unique<ScriptedSession>(
        stop_, libchiaking::play::Venue::home());
    owned->script.push_back(std::move(step));
    auto opened = libchiaking::play::Session::Core::open(
        {.player = SignedInPlayer::fresh(),
         .console = nullptr,
         .where = libchiaking::play::Where::home,
         .service = std::nullopt,
         .link = std::move(owned),
         .stop = stop_,
         .media = media_,
         .events = events_,
         .clock = clock_,
         .threading = LiveProviders::threading(),
         .journal = std::make_shared<libchiaking::diagnostics::SessionJournal>(
             LiveProviders::threading(), LiveProviders::steady(), 1),
         .first_controller =
             libchiaking::controllers::ControllerKind::dualsense,
         .settings = {},
         .share = nullptr});
    if (!opened)
      return ::testing::AssertionFailure() << "the session did not open";
    auto& loop = opened.value();
    if (!loop->run_on_this_thread())
      return ::testing::AssertionFailure() << "the session did not run";
    const auto outcome = loop->outcome();
    if (!outcome) return ::testing::AssertionFailure() << "no outcome";
    if (!outcome->succeeded())
      return ::testing::AssertionFailure() << "the session failed";
    return ::testing::AssertionSuccess();
  }

  void hand(remote_play::event::EncodedMedia unit, ScriptedSession& session) {
    const auto* bytes = unit.bytes.data();
    const auto size = unit.bytes.size();
    const auto before = CountedAllocations::on_this_thread();
    session.emit(std::move(unit));
    handed_.push_back(
        {bytes, size, CountedAllocations::on_this_thread() - before});
  }

  static auto assembled(std::uint16_t frame, std::size_t size)
      -> remote_play::event::EncodedMedia {
    remote_play::event::EncodedMedia unit{remote_play::EncodedMediaKind::video,
                                          {}};
    unit.bytes.reserve(size + remote_play::VideoAssembler::buffer_padding);
    unit.bytes.assign(size, static_cast<std::byte>(frame));
    unit.bytes.back() = std::byte{0x7f};
    unit.frame_index = frame;
    return unit;
  }

  static auto sound(std::size_t index) -> remote_play::event::EncodedMedia {
    remote_play::event::EncodedMedia packet{
        index == 0 ? remote_play::EncodedMediaKind::audio_profile
                   : remote_play::EncodedMediaKind::audio,
        {}};
    packet.bytes.assign(1200 + index, static_cast<std::byte>(index + 1));
    packet.bytes.back() = std::byte{0x7e};
    return packet;
  }

  auto seen() const -> const SeenMedia& { return media_; }
  auto handed() const -> const std::vector<Handed>& { return handed_; }

 private:
  libchiaking::platform::StopSource stop_;
  api_test::ManualClock clock_;
  SeenMedia media_;
  RecordedEvents events_;
  std::vector<Handed> handed_;
};

TEST_F(SessionMediaHandover,
       GivesTheSinkEachFrameInTheBufferTheLinkHandedOver) {
  constexpr std::size_t frame_size = 256 * 1024;
  ASSERT_TRUE(run([&](ScriptedSession& session) {
    session.set_state(remote_play::StreamState::streaming);
    session.emit(api_test::stream_info());
    for (std::size_t index = 0; index < count; ++index)
      hand(assembled(static_cast<std::uint16_t>(index + 1), frame_size + index),
           session);
  }));
  ASSERT_EQ(seen().frames().size(), count);
  ASSERT_EQ(handed().size(), count);
  for (std::size_t index = 0; index < count; ++index) {
    SCOPED_TRACE(index);
    const auto& frame = seen().frames()[index];
    const auto& given = handed()[index];
    EXPECT_EQ(frame.bytes, given.bytes);
    EXPECT_EQ(frame.padded, given.bytes);
    EXPECT_EQ(frame.size, given.size);
    EXPECT_EQ(frame.padded_size, given.size + media::VideoFrame::tail_padding);
    EXPECT_EQ(frame.frame_index, index + 1);
    EXPECT_EQ(frame.first, static_cast<std::byte>(index + 1));
    EXPECT_EQ(frame.last, std::byte{0x7f});
    EXPECT_TRUE(frame.tail_is_zero);
    EXPECT_EQ(given.allocations, 0);
  }
}

TEST_F(SessionMediaHandover,
       GivesTheSinkEachAudioPacketInTheBufferTheLinkHandedOver) {
  ASSERT_TRUE(run([&](ScriptedSession& session) {
    session.set_state(remote_play::StreamState::streaming);
    session.emit(api_test::stream_info());
    for (std::size_t index = 0; index < count; ++index)
      hand(sound(index), session);
  }));
  ASSERT_EQ(seen().sounds().size(), count);
  ASSERT_EQ(handed().size(), count);
  for (std::size_t index = 0; index < count; ++index) {
    SCOPED_TRACE(index);
    const auto& heard = seen().sounds()[index];
    const auto& given = handed()[index];
    EXPECT_EQ(heard.bytes, given.bytes);
    EXPECT_EQ(heard.size, given.size);
    EXPECT_EQ(heard.first, static_cast<std::byte>(index + 1));
    EXPECT_EQ(heard.last, std::byte{0x7e});
    EXPECT_EQ(heard.kind, index == 0 ? media::AudioUnit::Kind::format
                                     : media::AudioUnit::Kind::samples);
    EXPECT_EQ(given.allocations, 0);
  }
}

}  // namespace
}  // namespace play_test
