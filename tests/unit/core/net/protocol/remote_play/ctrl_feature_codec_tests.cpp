#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using remote_play::CtrlCodec;
using remote_play::CtrlOutboundFrame;

class CtrlFeatureCodec : public ::testing::Test {
 protected:
  static auto bytes(std::initializer_list<int> values)
      -> std::vector<std::byte> {
    std::vector<std::byte> result;
    for (const int value : values)
      result.push_back(static_cast<std::byte>(value));
    return result;
  }

  static auto text_bytes(std::string_view text) -> std::vector<std::byte> {
    std::vector<std::byte> result;
    for (const char value : text)
      result.push_back(static_cast<std::byte>(value));
    return result;
  }

  template <class T>
  static auto notice_as(const std::optional<remote_play::CtrlNotice>& notice)
      -> std::optional<T> {
    if (!notice) return std::nullopt;
    const auto* value = std::get_if<T>(&*notice);
    if (!value) return std::nullopt;
    return *value;
  }

  static auto is_malformed(const std::optional<remote_play::CtrlNotice>& notice)
      -> bool {
    return notice_as<remote_play::CtrlMalformedNotice>(notice).has_value();
  }
};

TEST_F(CtrlFeatureCodec, EncodesSimpleCommandsLikeTheReference) {
  EXPECT_EQ(CtrlCodec::encode_goto_bed(), (CtrlOutboundFrame{0x50, {}}));
  EXPECT_EQ(CtrlCodec::encode_go_home(),
            (CtrlOutboundFrame{0x14, bytes({0x00, 0xff, 0, 0, 0, 0, 0, 0, 0, 0,
                                            0, 0, 0, 0, 0, 0})}));
  const auto enable = CtrlCodec::encode_keyboard_enable();
  EXPECT_EQ(enable[0],
            (CtrlOutboundFrame{0xd, bytes({0, 0, 0, 1, 0, 0, 0x05, 0xae, 0, 0,
                                           0, 0, 0, 0, 0, 0})}));
  EXPECT_EQ(enable[1], (CtrlOutboundFrame{0x20, bytes({1})}));
  EXPECT_EQ(CtrlCodec::encode_keyboard_accept(),
            (CtrlOutboundFrame{0x25, bytes({0, 0, 0, 0})}));
  EXPECT_EQ(CtrlCodec::encode_keyboard_reject(),
            (CtrlOutboundFrame{0x25, bytes({0, 0, 0, 1})}));
}

TEST_F(CtrlFeatureCodec, EncodesKeyboardTextLikeTheReference) {
  auto expected = bytes({0, 0, 0, 3, 0, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                         0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5});
  const auto hello = text_bytes("hello");
  expected.insert(expected.end(), hello.begin(), hello.end());
  const auto text = CtrlCodec::encode_keyboard_set_text(3, "hello");
  ASSERT_TRUE(text);
  EXPECT_EQ(text.value(), (CtrlOutboundFrame{0x23, expected}));
  const auto empty = CtrlCodec::encode_keyboard_set_text(1, "");
  ASSERT_TRUE(empty);
  ASSERT_EQ(empty.value().payload.size(), 0x24U);
  EXPECT_EQ(empty.value().payload[3], std::byte{1});
  const auto long_text =
      CtrlCodec::encode_keyboard_set_text(9, std::string(534, 'x'));
  ASSERT_TRUE(long_text);
  ASSERT_EQ(long_text.value().payload.size(), 570U);
  EXPECT_EQ(long_text.value().payload[6], std::byte{2});
  EXPECT_EQ(long_text.value().payload[7], std::byte{0x16});
}

TEST_F(CtrlFeatureCodec, EncodesACouchUserJoinAndRefusesBadArguments) {
  const auto join = CtrlCodec::encode_couch_user_join(2, "AQIDBAUGBwg=");
  ASSERT_TRUE(join);
  EXPECT_EQ(join.value(),
            (CtrlOutboundFrame{0x68, bytes({1, 2, 3, 4, 5, 6, 7, 8, 2, 3, 0, 0,
                                            0, 0, 0, 0})}));
  EXPECT_FALSE(CtrlCodec::encode_couch_user_join(0, "AQIDBAUGBwg="));
  EXPECT_FALSE(CtrlCodec::encode_couch_user_join(4, "AQIDBAUGBwg="));
  EXPECT_FALSE(CtrlCodec::encode_couch_user_join(1, ""));
  EXPECT_FALSE(CtrlCodec::encode_couch_user_join(1, "AQID"));
  EXPECT_FALSE(CtrlCodec::encode_couch_user_join(1, "AQIDBAUGBwgJ"));
}

TEST_F(CtrlFeatureCodec, AcceptsOnlyEightByteBase64CouchAccountIds) {
  EXPECT_TRUE(CtrlCodec::valid_couch_account_id("AAAAAAAAAAA="));
  EXPECT_FALSE(CtrlCodec::valid_couch_account_id("not-an-account"));
  EXPECT_FALSE(CtrlCodec::valid_couch_account_id(""));
  EXPECT_FALSE(CtrlCodec::valid_couch_account_id(std::string(24, 'A')));
}

TEST_F(CtrlFeatureCodec, EncodesCouchPadJoinsLeavesAndPasscodes) {
  EXPECT_EQ(CtrlCodec::encode_couch_pad_join(3, 0).value(),
            (CtrlOutboundFrame{0x08, bytes({3, 3})}));
  EXPECT_EQ(CtrlCodec::encode_couch_pad_join(1, 6).value(),
            (CtrlOutboundFrame{0x08, bytes({1, 6})}));
  EXPECT_FALSE(CtrlCodec::encode_couch_pad_join(0, 3));
  EXPECT_EQ(CtrlCodec::encode_couch_leave(2).value(),
            (CtrlOutboundFrame{0x09, bytes({2})}));
  EXPECT_FALSE(CtrlCodec::encode_couch_leave(4));
  EXPECT_EQ(
      CtrlCodec::encode_couch_passcode(1, "0427").value(),
      (CtrlOutboundFrame{0x8064, bytes({1, '0', '4', '2', '7', 0, 0, 0})}));
  EXPECT_FALSE(CtrlCodec::encode_couch_passcode(1, "042"));
  EXPECT_FALSE(CtrlCodec::encode_couch_passcode(1, "04a7"));
  EXPECT_FALSE(CtrlCodec::encode_couch_passcode(0, "0427"));
}

TEST_F(CtrlFeatureCodec, DecodesAKeyboardOpenNoticeWithItsText) {
  auto open = std::vector<std::byte>(0x20);
  open[0x1f] = std::byte{3};
  const auto abc = text_bytes("abc");
  open.insert(open.end(), abc.begin(), abc.end());
  const auto opened = notice_as<remote_play::CtrlKeyboardOpen>(
      CtrlCodec::decode_notice(0x21, open));
  ASSERT_TRUE(opened);
  EXPECT_EQ(opened->text, "abc");
  open[0x1f] = std::byte{4};
  EXPECT_TRUE(is_malformed(CtrlCodec::decode_notice(0x21, open)));
  EXPECT_TRUE(is_malformed(
      CtrlCodec::decode_notice(0x21, std::vector<std::byte>(0x1f))));
  const auto empty = notice_as<remote_play::CtrlKeyboardOpen>(
      CtrlCodec::decode_notice(0x21, std::vector<std::byte>(0x20)));
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->text.empty());
}

TEST_F(CtrlFeatureCodec, DecodesKeyboardTextChangesAndRemoteCloses) {
  auto change = std::vector<std::byte>(0x28);
  change[0] = std::byte{7};
  change[0xb] = std::byte{2};
  change[0x27] = std::byte{9};
  const auto hi = text_bytes("hi");
  change.insert(change.end(), hi.begin(), hi.end());
  const auto changed = notice_as<remote_play::CtrlKeyboardTextChange>(
      CtrlCodec::decode_notice(0x24, change));
  ASSERT_TRUE(changed);
  EXPECT_EQ(changed->text, "hi");
  EXPECT_TRUE(is_malformed(
      CtrlCodec::decode_notice(0x24, std::vector<std::byte>(0x27))));
  EXPECT_TRUE(notice_as<remote_play::CtrlKeyboardRemoteClose>(
                  CtrlCodec::decode_notice(0x22, bytes({1, 2})))
                  .has_value());
}

TEST_F(CtrlFeatureCodec, DecodesDisplayNotices) {
  const auto display_a = notice_as<remote_play::CtrlDisplayA>(
      CtrlCodec::decode_notice(0x1, bytes({1})));
  ASSERT_TRUE(display_a);
  EXPECT_EQ(display_a->state, 1);
  const auto display_b = notice_as<remote_play::CtrlDisplayB>(
      CtrlCodec::decode_notice(0x16, bytes({1, 0xff, 7})));
  ASSERT_TRUE(display_b);
  EXPECT_EQ(display_b->first, 1);
  EXPECT_EQ(display_b->second, 0xff);
  EXPECT_TRUE(is_malformed(CtrlCodec::decode_notice(0x16, bytes({1}))));
}

TEST_F(CtrlFeatureCodec,
       DecodesAnUnresolvableCouchAccountKeepingOnlyPrintables) {
  auto refusal = bytes({1});
  const auto fields = text_bytes("Ab3dEf7hIjKlMnOpQrStUv");
  refusal.insert(refusal.end(), fields.begin(), fields.end());
  refusal.push_back(std::byte{0});
  ASSERT_EQ(refusal.size(), 24U);
  refusal[4] = std::byte{0x07};
  const auto unresolvable =
      notice_as<remote_play::CtrlCouchAccountUnresolvable>(
          CtrlCodec::decode_notice(0x61, refusal));
  ASSERT_TRUE(unresolvable);
  EXPECT_EQ(unresolvable->pad, 1);
  EXPECT_EQ(unresolvable->printable, "Ab3Ef7hIjKlMnOpQrStUv");
  const auto pad_only = notice_as<remote_play::CtrlCouchAccountUnresolvable>(
      CtrlCodec::decode_notice(0x61, bytes({2})));
  ASSERT_TRUE(pad_only);
  EXPECT_EQ(pad_only->pad, 2);
  EXPECT_TRUE(is_malformed(CtrlCodec::decode_notice(0x61, {})));
}

TEST_F(CtrlFeatureCodec, DecodesCouchReplies) {
  const auto passcode = notice_as<remote_play::CtrlCouchPasscodeRequest>(
      CtrlCodec::decode_notice(0x64, bytes({2, 0, 0, 0})));
  ASSERT_TRUE(passcode);
  EXPECT_EQ(passcode->pad, 2);
  const auto joined = notice_as<remote_play::CtrlCouchUserJoinReply>(
      CtrlCodec::decode_notice(0x8068, bytes({3, 1})));
  ASSERT_TRUE(joined);
  EXPECT_EQ(joined->pad, 3);
  EXPECT_EQ(joined->status, 1);
  const auto pad_join = notice_as<remote_play::CtrlCouchPadJoinReply>(
      CtrlCodec::decode_notice(0x8008, bytes({2, 0})));
  ASSERT_TRUE(pad_join);
  EXPECT_EQ(pad_join->pad, 2);
  EXPECT_EQ(pad_join->result, 0);
  const auto dropped = notice_as<remote_play::CtrlCouchPadDropped>(
      CtrlCodec::decode_notice(0xa, bytes({3})));
  ASSERT_TRUE(dropped);
  EXPECT_EQ(dropped->pad, 3);
  const auto leave = notice_as<remote_play::CtrlCouchLeaveReply>(
      CtrlCodec::decode_notice(0x8009, bytes({1, 0})));
  ASSERT_TRUE(leave);
  EXPECT_EQ(leave->pad, 1);
  EXPECT_EQ(leave->result, 0);
}

TEST_F(CtrlFeatureCodec, DecodesCouchRepliesWithTrailingBytes) {
  const auto longer = notice_as<remote_play::CtrlCouchUserJoinReply>(
      CtrlCodec::decode_notice(0x8068, bytes({2, 0, 9})));
  ASSERT_TRUE(longer);
  EXPECT_EQ(longer->pad, 2);
  EXPECT_EQ(longer->status, 0);
  const auto passcode = notice_as<remote_play::CtrlCouchPasscodeRequest>(
      CtrlCodec::decode_notice(0x64, bytes({3})));
  ASSERT_TRUE(passcode);
  EXPECT_EQ(passcode->pad, 3);
  const auto dropped = notice_as<remote_play::CtrlCouchPadDropped>(
      CtrlCodec::decode_notice(0xa, bytes({1, 0})));
  ASSERT_TRUE(dropped);
  EXPECT_EQ(dropped->pad, 1);
}

TEST_F(CtrlFeatureCodec, ReportsShortCouchRepliesAsMalformed) {
  for (const auto& [type, size] :
       std::initializer_list<std::pair<std::uint16_t, std::size_t>>{
           {0x64, 0}, {0x8068, 1}, {0x8008, 1}, {0xa, 0}, {0x8009, 1}}) {
    const auto malformed = notice_as<remote_play::CtrlMalformedNotice>(
        CtrlCodec::decode_notice(type, std::vector<std::byte>(size)));
    ASSERT_TRUE(malformed) << "type " << type;
    EXPECT_EQ(malformed->type, type);
    EXPECT_EQ(malformed->size, size) << "type " << type;
  }
}

TEST_F(CtrlFeatureCodec, IsNotANoticeForOtherTypes) {
  EXPECT_FALSE(CtrlCodec::decode_notice(0x33, bytes({1})));
}

}  // namespace
}  // namespace remote_play_test
