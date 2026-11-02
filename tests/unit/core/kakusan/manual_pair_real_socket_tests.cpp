#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/manual_pair_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace kakusan_test {

using test_support::play::SourceStream;
namespace {

using std::chrono::milliseconds;
namespace host_event = manual::host_event;
namespace guest_event = manual::guest_event;
using manual::Progress;

class ManualPairRealSockets : public testing::Test {
 protected:
  void SetUp() override {
    const auto config = manual::SharingRules::create(4, 1, manual::all_buttons);
    ASSERT_TRUE(config);
    auto environment = manual::Environment::system(
        TestPlatform::crypto(), TestPlatform::secure_datagram());
    ASSERT_TRUE(environment);
    auto created =
        manual::Host::create(config.value(), TestPlatform::identity(0x31),
                             std::move(environment).value(), side);
    ASSERT_TRUE(created);
    host.emplace(std::move(created).value());
    side.host = &*host;
    ASSERT_TRUE(player.guest);
  }

  template <class Done>
  auto real_until(milliseconds limit, Done done) -> bool {
    libchiaking::platform::Sleeper sleeper{TestPlatform::threading(),
                                           TestPlatform::steady()};
    const auto deadline =
        TestPlatform::steady().now_microseconds() +
        static_cast<std::uint64_t>(std::chrono::microseconds{limit}.count());
    while (!done()) {
      if (TestPlatform::steady().now_microseconds() >= deadline) return false;
      if (!ManualPair::pump(*host)) {
        ADD_FAILURE() << "the host failed to pump";
        return false;
      }
      if (!ManualPair::pump(*player.guest)) {
        ADD_FAILURE() << "the guest failed to pump";
        return false;
      }
      sleeper.sleep_for(std::min({host->next_wait(milliseconds{2}),
                                  player.guest->next_wait(milliseconds{2}),
                                  milliseconds{2}}));
    }
    return true;
  }

  ManualHostSide side;
  std::optional<manual::Host> host;
  ManualPlayer player{0x61};
};

TEST_F(ManualPairRealSockets, CarryAWholeVisit) {
  host->source_keys(SourceStream::takion_version,
                    SourceStream::receive_keys());
  ASSERT_TRUE(host->source_started(SourceStream::description(),
                                   SourceStream::video_fps));

  const auto invited_guest = host->invite();
  ASSERT_TRUE(invited_guest);
  player.id = invited_guest.value();
  const auto invited = real_until(milliseconds{3000}, [&] {
    return side.find<host_event::InvitationReady>(player.id) != nullptr ||
           side.state(player.id) == Progress::gone;
  });
  if (!invited || side.find<host_event::InvitationReady>(player.id) == nullptr)
    GTEST_SKIP() << "no invitation over the local network";
  const auto block =
      side.find<host_event::InvitationReady>(player.id)->block.view();
  EXPECT_TRUE(player.guest->accept_invitation(block));
  ASSERT_TRUE(real_until(milliseconds{3000}, [&] {
    return player.find<guest_event::ReplyReady>() != nullptr;
  }));
  const auto reply = player.find<guest_event::ReplyReady>()->block.view();
  EXPECT_TRUE(host->import_reply(reply));
  const auto connected = real_until(milliseconds{8000}, [&] {
    return side.find<host_event::CodeReady>(player.id) != nullptr &&
           player.find<guest_event::CodeReady>() != nullptr;
  });
  if (!connected) GTEST_SKIP() << "no path between the local sockets";

  const std::string guest_code{
      player.find<guest_event::CodeReady>()->code.digits()};
  const std::string host_code{
      side.find<host_event::CodeReady>(player.id)->code.digits()};
  EXPECT_TRUE(host->enter_code(player.id, guest_code));
  EXPECT_TRUE(player.guest->enter_code(host_code));
  ASSERT_TRUE(real_until(milliseconds{8000}, [&] {
    return player.guest->state() == Progress::playing &&
           side.state(player.id) == Progress::playing;
  }));

  EXPECT_FALSE(real_until(milliseconds{200}, [] { return false; }));
  for (std::size_t index = 0; index < SourceStream::vectors.size();
       ++index) {
    const auto bytes = SourceStream::datagram(index);
    host->source_datagram(SourceDescriptions::describe(
        SourceStream::takion_version,
        SourceStream::vectors[index].media,
        SourceStream::vectors[index].key_position, bytes));
    EXPECT_FALSE(real_until(milliseconds{5}, [] { return false; }));
  }
  ASSERT_TRUE(real_until(milliseconds{5000}, [&] {
    return player.frames(ManualStream::Media::video).size() >=
               SourceStream::video_frames.size() &&
           player.frames(ManualStream::Media::audio).size() >=
               SourceStream::audio_frames.size();
  }));
  const auto video = player.frames(ManualStream::Media::video);
  EXPECT_EQ(video.back(),
            test_support::Bytes::unhex(SourceStream::video_frames.back()));

  const auto cross = static_cast<std::uint32_t>(
      libchiaking::controllers::ControllerButton::cross);
  libchiaking::controllers::ControllerState pressed;
  pressed.buttons = cross;
  EXPECT_TRUE(player.guest->set_controller(pressed));
  EXPECT_TRUE(real_until(milliseconds{3000}, [&] {
    const auto inputs = side.inputs();
    return !inputs.empty() && inputs.back().buttons == cross &&
           inputs.back().controller_id == 1;
  }));

  player.guest->leave();
  EXPECT_TRUE(real_until(milliseconds{5000}, [&] {
    return player.guest->state() == Progress::gone &&
           side.state(player.id) == Progress::gone;
  }));
  EXPECT_EQ(player.count<guest_event::StreamStopped>(), 1U);
}

}  // namespace
}  // namespace kakusan_test
