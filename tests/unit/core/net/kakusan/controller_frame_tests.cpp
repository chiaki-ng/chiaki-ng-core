#include "src/core/net/kakusan/controller_frame.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/fixtures.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace net_kakusan_test {
namespace {

using namespace libchiaking::net::kakusan;
using test_support::Bytes;
using test_support::kakusan::Fixtures;

class ControllerInputFrame : public ::testing::Test {
 protected:
  static auto frame() -> InputFrame {
    InputFrame made;
    made.binding = SeatBinding{3, 7};
    made.sequence = 42;
    made.elapsed_ms = 1000;
    made.state.buttons = static_cast<std::uint32_t>(Button::south);
    made.state.left_x = -32767;
    made.state.left_y = 0;
    made.state.right_x = 32767;
    made.state.right_y = 0;
    made.state.left_trigger = 0;
    made.state.right_trigger = 65535;
    made.state.touch = TouchContact{true, 2, 32768, 65535};
    return made;
  }

  void SetUp() override {
    const auto vectors = Fixtures::text("vectors.json");
    record_ = Bytes::of(input_prefix);
    const auto payload =
        Bytes::unhex(Fixtures::string(vectors, "input_packet_hex"));
    ASSERT_EQ(payload.size(), input_payload_size);
    record_.insert(record_.end(), payload.begin(), payload.end());
  }

  auto record() const -> const std::vector<std::byte>& { return record_; }

  auto decode_with(std::size_t offset, std::uint8_t value) const {
    auto changed = record_;
    changed[offset] = static_cast<std::byte>(value);
    return ControllerFrameCodec::decode_input(changed);
  }

 private:
  std::vector<std::byte> record_;
};

TEST_F(ControllerInputFrame, EncodesTheFixtureRecord) {
  const auto encoded = ControllerFrameCodec::encode_input(frame());
  ASSERT_TRUE(encoded);
  EXPECT_TRUE(std::ranges::equal(encoded.value(), record()));
}

TEST_F(ControllerInputFrame, DecodesTheFixtureRecord) {
  const auto decoded = ControllerFrameCodec::decode_input(record());
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded.value().binding, (SeatBinding{3, 7}));
  EXPECT_EQ(decoded.value().sequence, 42U);
  EXPECT_EQ(decoded.value().elapsed_ms, 1000U);
  EXPECT_EQ(decoded.value().state, frame().state);
}

TEST_F(ControllerInputFrame, RejectsAShortOrLongRecord) {
  EXPECT_FALSE(ControllerFrameCodec::decode_input(
      std::span<const std::byte>{record()}.first(record().size() - 1)));
  auto longer = record();
  longer.push_back(std::byte{});
  EXPECT_FALSE(ControllerFrameCodec::decode_input(longer));
}

TEST_F(ControllerInputFrame, RejectsMalformedFields) {
  EXPECT_FALSE(decode_with(0, 'X'));
  EXPECT_FALSE(decode_with(4, 2));
  EXPECT_FALSE(decode_with(5, 2));
  EXPECT_FALSE(decode_with(6, 0));
  EXPECT_FALSE(decode_with(7, 1));
  EXPECT_FALSE(decode_with(4 + 16, 0x01));
  EXPECT_FALSE(decode_with(4 + 17, 0x04));
  EXPECT_TRUE(decode_with(4 + 17, 0x03));
  EXPECT_FALSE(decode_with(4 + 32, 2));
  EXPECT_FALSE(decode_with(4 + 33, 128));
  EXPECT_FALSE(decode_with(4 + 38, 1));
  EXPECT_FALSE(decode_with(4 + 39, 1));
}

TEST_F(ControllerInputFrame, RejectsTheMostNegativeStickValue) {
  auto minimum = record();
  minimum[4 + 20] = std::byte{0x80};
  minimum[4 + 21] = std::byte{0x00};
  EXPECT_FALSE(ControllerFrameCodec::decode_input(minimum));
}

TEST_F(ControllerInputFrame, AcceptsAnIdleTouchOnlyWhenItsFieldsAreZero) {
  auto idle_touch = record();
  idle_touch[4 + 32] = std::byte{0};
  EXPECT_FALSE(ControllerFrameCodec::decode_input(idle_touch));
  for (std::size_t offset = 33; offset < 38; ++offset)
    idle_touch[4 + offset] = std::byte{0};
  EXPECT_TRUE(ControllerFrameCodec::decode_input(idle_touch));
}

TEST_F(ControllerInputFrame, RejectsAZeroGeneration) {
  auto zero_generation = record();
  for (std::size_t offset = 4; offset < 8; ++offset)
    zero_generation[4 + offset] = std::byte{0};
  EXPECT_FALSE(ControllerFrameCodec::decode_input(zero_generation));
}

TEST_F(ControllerInputFrame, RefusesToEncodeAnInvalidState) {
  auto unknown_button = frame();
  unknown_button.state.buttons = 1U << 18U;
  EXPECT_FALSE(ControllerFrameCodec::encode_input(unknown_button));
  auto minimum_stick = frame();
  minimum_stick.state.left_y = -32768;
  EXPECT_FALSE(ControllerFrameCodec::encode_input(minimum_stick));
  auto idle_touch = frame();
  idle_touch.state.touch = TouchContact{false, 0, 1, 0};
  EXPECT_FALSE(ControllerFrameCodec::encode_input(idle_touch));
  auto no_seat = frame();
  no_seat.binding.seat = 0;
  EXPECT_FALSE(ControllerFrameCodec::encode_input(no_seat));
}

class ControllerFeedbackFrame : public ::testing::Test {
 protected:
  static auto frame() -> FeedbackFrame {
    FeedbackFrame made;
    made.binding = SeatBinding{3, 7};
    made.sequence = 0xfffffffeU;
    made.elapsed_ms = 0x01020304U;
    made.low_frequency = 0x1234;
    made.high_frequency = 0xffff;
    return made;
  }

  void SetUp() override {
    const auto encoded = ControllerFrameCodec::encode_feedback(frame());
    ASSERT_TRUE(encoded);
    record_.assign(encoded.value().begin(), encoded.value().end());
  }

  auto record() const -> const std::vector<std::byte>& { return record_; }

 private:
  std::vector<std::byte> record_;
};

TEST_F(ControllerFeedbackFrame, EncodesTheWireLayout) {
  EXPECT_EQ(Bytes::hex(record()),
            "524346310102030000000007fffffffe010203041234ffff");
}

TEST_F(ControllerFeedbackFrame, RoundTripsEveryField) {
  const auto decoded = ControllerFrameCodec::decode_feedback(record());
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded.value().binding, frame().binding);
  EXPECT_EQ(decoded.value().sequence, frame().sequence);
  EXPECT_EQ(decoded.value().elapsed_ms, frame().elapsed_ms);
  EXPECT_EQ(decoded.value().low_frequency, frame().low_frequency);
  EXPECT_EQ(decoded.value().high_frequency, frame().high_frequency);
}

TEST_F(ControllerFeedbackFrame, RejectsTheWrongKindReservedBitsOrAShortRecord) {
  auto wrong_kind = record();
  wrong_kind[5] = std::byte{1};
  EXPECT_FALSE(ControllerFrameCodec::decode_feedback(wrong_kind));
  auto reserved = record();
  reserved[7] = std::byte{1};
  EXPECT_FALSE(ControllerFrameCodec::decode_feedback(reserved));
  EXPECT_FALSE(ControllerFrameCodec::decode_feedback(
      std::span<const std::byte>{record()}.first(23)));
}

TEST_F(ControllerFeedbackFrame, IsNotReadAsInput) {
  EXPECT_FALSE(ControllerFrameCodec::decode_input(record()));
}

TEST(ControllerFrameSequence, IsNewerOnlyAheadWithinHalfTheRange) {
  EXPECT_TRUE(ControllerFrameCodec::is_newer(1, 0));
  EXPECT_FALSE(ControllerFrameCodec::is_newer(0, 0));
  EXPECT_FALSE(ControllerFrameCodec::is_newer(0, 1));
  EXPECT_TRUE(ControllerFrameCodec::is_newer(0, 0xffffffffU));
  EXPECT_TRUE(ControllerFrameCodec::is_newer(5, 0xfffffff0U));
  EXPECT_FALSE(ControllerFrameCodec::is_newer(0xfffffff0U, 5));
  EXPECT_TRUE(ControllerFrameCodec::is_newer(0x7fffffffU, 0));
  EXPECT_FALSE(ControllerFrameCodec::is_newer(0x80000000U, 0));
}

TEST(ControllerButtons, SplitEighteenBitsIntoRequiredAndOptional) {
  EXPECT_EQ(button_mask, 262143U);
  EXPECT_EQ(static_cast<std::uint32_t>(Button::r2), 1U << 17U);
  EXPECT_EQ(required_buttons & static_cast<std::uint32_t>(Button::share), 0U);
  EXPECT_EQ(required_buttons & static_cast<std::uint32_t>(Button::home), 0U);
  EXPECT_EQ(required_buttons | optional_buttons, button_mask);
}

}  // namespace
}  // namespace net_kakusan_test
