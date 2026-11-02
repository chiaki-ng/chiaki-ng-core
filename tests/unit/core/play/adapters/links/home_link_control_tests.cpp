#include "libchiaking/play_types.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/home_link_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace play_test {
namespace {

using namespace home_links;

using Frame = remote_play::CtrlOutboundFrame;
namespace command = remote_play::command;
namespace event = remote_play::event;

class HomeLinkCtrlFeatures : public HomeLinkTest {
 protected:
  static auto skip_frames(CtrlFeatureRig& rig, std::size_t count)
      -> testing::AssertionResult {
    for (std::size_t index = 0; index < count; ++index)
      if (!rig.next_frame())
        return testing::AssertionFailure() << "frame " << index << " missing";
    return testing::AssertionSuccess();
  }
};

TEST_F(HomeLinkCtrlFeatures, SendsNoKeyboardEnableWhenNotConfigured) {
  CtrlFeatureRig rig{false};
  ASSERT_NO_FATAL_FAILURE(rig.open());
  const auto expected = remote_play::CtrlCodec::post_id_frames(true, false);
  EXPECT_EQ(expected.size(), 5U);
  for (const auto& frame : expected) EXPECT_EQ(rig.next_frame(), frame);
}

TEST_F(HomeLinkCtrlFeatures, SendsKeyboardEnableWhenConfigured) {
  CtrlFeatureRig rig{true};
  ASSERT_NO_FATAL_FAILURE(rig.open());
  const auto expected = remote_play::CtrlCodec::post_id_frames(true, true);
  ASSERT_EQ(expected.size(), 7U);
  for (const auto& frame : expected) EXPECT_EQ(rig.next_frame(), frame);
  EXPECT_EQ(expected[2].type, 0xd);
  EXPECT_EQ(expected[3].type, 0x20);
  EXPECT_EQ(expected[2].payload, CtrlWire::of({0, 0, 0, 1, 0, 0, 0x05, 0xae, 0,
                                               0, 0, 0, 0, 0, 0, 0}));
  EXPECT_EQ(expected[3].payload, CtrlWire::of({1}));
}

TEST_F(HomeLinkCtrlFeatures, SendsPowerAndKeyboardCommands) {
  CtrlFeatureRig rig{true};
  ASSERT_NO_FATAL_FAILURE(rig.open());
  ASSERT_TRUE(skip_frames(rig, 7));
  auto& link = rig.link();
  EXPECT_TRUE(link.execute(command::GotoBed{}));
  EXPECT_TRUE(link.execute(command::GoHome{}));
  EXPECT_TRUE(
      link.execute(command::KeyboardSetText{libchiaking::SecretString{"hi"}}));
  EXPECT_TRUE(link.execute(command::KeyboardSetText{}));
  EXPECT_TRUE(link.execute(command::KeyboardAccept{}));
  EXPECT_TRUE(link.execute(command::KeyboardReject{}));
  EXPECT_EQ(rig.next_frame(), (Frame{0x50, {}}));
  EXPECT_EQ(rig.next_frame(),
            (Frame{0x14, CtrlWire::of({0, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                       0, 0, 0})}));
  auto hi = CtrlWire::of({0, 0, 0, 1, 0, 0, 0, 2});
  hi.resize(0x24);
  hi[0x23] = std::byte{2};
  hi.push_back(std::byte{'h'});
  hi.push_back(std::byte{'i'});
  EXPECT_EQ(rig.next_frame(), (Frame{0x23, hi}));
  auto empty = CtrlWire::of({0, 0, 0, 2});
  empty.resize(0x24);
  EXPECT_EQ(rig.next_frame(), (Frame{0x23, empty}));
  EXPECT_EQ(rig.next_frame(), (Frame{0x25, CtrlWire::of({0, 0, 0, 0})}));
  EXPECT_EQ(rig.next_frame(), (Frame{0x25, CtrlWire::of({0, 0, 0, 1})}));
  const std::string long_text(534, 'x');
  EXPECT_TRUE(link.execute(
      command::KeyboardSetText{libchiaking::SecretString{long_text}}));
  const auto long_frame = rig.next_frame();
  ASSERT_TRUE(long_frame);
  const auto encoded =
      remote_play::CtrlCodec::encode_keyboard_set_text(3, long_text);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(*long_frame, encoded.value());
  EXPECT_EQ(long_frame->payload.size(), 570U);
}

TEST_F(HomeLinkCtrlFeatures, SurfacesKeyboardEvents) {
  CtrlFeatureRig rig{true};
  ASSERT_NO_FATAL_FAILURE(rig.open());
  auto open = std::vector<std::byte>(0x20);
  open[0x1f] = std::byte{3};
  for (const char c : std::string_view{"abc"})
    open.push_back(static_cast<std::byte>(c));
  ASSERT_NO_FATAL_FAILURE(rig.send_frame(0x21, open));
  ASSERT_NO_FATAL_FAILURE(rig.pump_until_event<event::KeyboardOpen>(1));
  EXPECT_EQ(rig.sink().all<event::KeyboardOpen>()[0].text, "abc");
  auto change = std::vector<std::byte>(0x28);
  change[0xb] = std::byte{2};
  change.push_back(std::byte{'o'});
  change.push_back(std::byte{'k'});
  ASSERT_NO_FATAL_FAILURE(rig.send_frame(0x24, change));
  ASSERT_NO_FATAL_FAILURE(rig.pump_until_event<event::KeyboardTextChange>(1));
  EXPECT_EQ(rig.sink().all<event::KeyboardTextChange>()[0].text, "ok");
  ASSERT_NO_FATAL_FAILURE(rig.send_frame(0x22, {}));
  ASSERT_NO_FATAL_FAILURE(rig.pump_until_event<event::KeyboardRemoteClose>(1));
  ASSERT_NO_FATAL_FAILURE(rig.send_frame(0x21, std::vector<std::byte>(0x10)));
  ASSERT_NO_FATAL_FAILURE(rig.send_frame(0x22, {}));
  ASSERT_NO_FATAL_FAILURE(rig.pump_until_event<event::KeyboardRemoteClose>(2));
  EXPECT_EQ(rig.sink().all<event::KeyboardOpen>().size(), 1U);
  EXPECT_TRUE(rig.diagnostics().contains(
      "Ctrl received malformed message type 0x21 with payload size 16"));
  EXPECT_EQ(rig.link().state(), StreamState::negotiating_stream);
}

TEST_F(HomeLinkCtrlFeatures, ReportsAProhibitedDisplayFromTheCtrlReply) {
  CtrlFeatureRig rig{false, "HTTP/1.1 200 OK\r\nRP-Prohibit: 1\r\n\r\n"};
  ASSERT_NO_FATAL_FAILURE(rig.open());
  const auto prohibited = rig.sink().all<event::DisplayProhibited>();
  ASSERT_EQ(prohibited.size(), 1U);
  EXPECT_TRUE(prohibited[0].prohibited);
}

TEST_F(HomeLinkCtrlFeatures, TracksDisplayProhibitionFromCtrlMessages) {
  CtrlFeatureRig rig{false, "HTTP/1.1 200 OK\r\nRP-Prohibit: 0\r\n\r\n"};
  ASSERT_NO_FATAL_FAILURE(rig.open());
  EXPECT_TRUE(rig.sink().all<event::DisplayProhibited>().empty());
  ASSERT_NO_FATAL_FAILURE(rig.send_frame(0x16, CtrlWire::of({0, 0})));
  ASSERT_NO_FATAL_FAILURE(rig.send_frame(0x1, CtrlWire::of({1})));
  ASSERT_NO_FATAL_FAILURE(rig.send_frame(0x16, CtrlWire::of({1, 0xff})));
  ASSERT_NO_FATAL_FAILURE(rig.send_frame(0x16, CtrlWire::of({0, 5})));
  ASSERT_NO_FATAL_FAILURE(rig.send_frame(0x16, CtrlWire::of({0, 6})));
  ASSERT_NO_FATAL_FAILURE(rig.send_frame(0x1, CtrlWire::of({0})));
  ASSERT_NO_FATAL_FAILURE(rig.send_frame(0x16, CtrlWire::of({1, 0xff})));
  ASSERT_NO_FATAL_FAILURE(rig.send_frame(0x1, CtrlWire::of({0})));
  ASSERT_NO_FATAL_FAILURE(rig.pump_until_event<event::DisplayProhibited>(2));
  const auto events = rig.sink().all<event::DisplayProhibited>();
  EXPECT_TRUE(events[0].prohibited);
  EXPECT_FALSE(events[1].prohibited);
  EXPECT_TRUE(rig.diagnostics().contains(
      "Ctrl received message that the stream can't display due to displaying "
      "some content that can't be streamed."));
  EXPECT_TRUE(rig.diagnostics().contains(
      "Ctrl received message that the stream can now display."));
}

TEST_F(HomeLinkCtrlFeatures, RepeatsThePostIdFramesOnEverySessionId) {
  CtrlFeatureRig rig{true};
  ASSERT_NO_FATAL_FAILURE(rig.open());
  const auto expected = remote_play::CtrlCodec::post_id_frames(true, true);
  for (const auto& frame : expected) EXPECT_EQ(rig.next_frame(), frame);
  ASSERT_NO_FATAL_FAILURE(rig.send_frame(0x33, CtrlWire::session_id('B')));
  ASSERT_NO_FATAL_FAILURE(rig.send_frame(0x33, CtrlWire::of({0})));
  for (int round = 0; round < 2; ++round) {
    SCOPED_TRACE(round);
    for (const auto& frame : expected) EXPECT_EQ(rig.next_frame(), frame);
  }
  EXPECT_TRUE(rig.link().execute(command::GotoBed{}));
  EXPECT_EQ(rig.next_frame(), (Frame{0x50, {}}));
}

TEST_F(HomeLinkCtrlFeatures, RoutesCouchPlayThroughCtrl) {
  CtrlFeatureRig rig{false};
  ASSERT_NO_FATAL_FAILURE(rig.open());
  ASSERT_TRUE(skip_frames(rig, 5));
  auto& owner = rig.link();
  const auto missing = owner.execute(command::CouchUserJoin{1});
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error(), StreamError::invalid_argument);
  EXPECT_TRUE(owner.execute(command::CouchSetAccountId{
      1, libchiaking::AccountId::from_base64("AQIDBAUGBwg=").value()}));
  EXPECT_TRUE(owner.execute(command::CouchUserJoin{1}));
  const auto unsolicited = owner.execute(
      command::CouchPasscode{1, play::AccessCode::parse("0427").value()});
  ASSERT_FALSE(unsolicited);
  EXPECT_EQ(unsolicited.error(), StreamError::invalid_state);
  EXPECT_TRUE(owner.execute(command::SetPadCount{2}));
  const auto offline = owner.execute(command::CouchOpenPad{1, 0});
  ASSERT_FALSE(offline);
  EXPECT_EQ(offline.error(), StreamError::invalid_state);
  EXPECT_EQ(rig.next_frame(),
            (Frame{0x68, CtrlWire::of({1, 2, 3, 4, 5, 6, 7, 8, 1, 3, 0, 0, 0, 0,
                                       0, 0})}));
  ASSERT_NO_FATAL_FAILURE(rig.send_frame(0x8068, CtrlWire::of({1, 3})));
  ASSERT_NO_FATAL_FAILURE(rig.pump_until_event<event::PadPasscodeRequest>(1));
  EXPECT_FALSE(rig.sink().all<event::PadPasscodeRequest>()[0].retry);
  EXPECT_TRUE(owner.execute(
      command::CouchPasscode{1, play::AccessCode::parse("0427").value()}));
  EXPECT_EQ(rig.next_frame(),
            (Frame{0x8064, CtrlWire::of({1, '0', '4', '2', '7', 0, 0, 0})}));
  ASSERT_NO_FATAL_FAILURE(rig.send_frame(0x8068, CtrlWire::of({1, 0})));
  ASSERT_NO_FATAL_FAILURE(rig.pump_until_event<event::PadJoinFailed>(1));
  const auto failed = rig.sink().all<event::PadJoinFailed>()[0];
  EXPECT_EQ(failed.index, 1);
  EXPECT_EQ(failed.status, 1);
  EXPECT_EQ(rig.next_frame(), (Frame{0x09, CtrlWire::of({1})}));
  EXPECT_TRUE(owner.execute(command::CouchSetAccountId{
      2, libchiaking::AccountId::from_base64("CAcGBQQDAgE=").value()}));
  EXPECT_TRUE(owner.execute(command::CouchUserJoin{2}));
  EXPECT_EQ(rig.next_frame(),
            (Frame{0x68, CtrlWire::of({8, 7, 6, 5, 4, 3, 2, 1, 2, 3, 0, 0, 0, 0,
                                       0, 0})}));
  ASSERT_NO_FATAL_FAILURE(rig.send_frame(0xa, CtrlWire::of({2})));
  ASSERT_NO_FATAL_FAILURE(rig.pump_until_event<event::PadDropped>(1));
  EXPECT_EQ(rig.next_frame(), (Frame{0x09, CtrlWire::of({2})}));
}

}  // namespace
}  // namespace play_test
