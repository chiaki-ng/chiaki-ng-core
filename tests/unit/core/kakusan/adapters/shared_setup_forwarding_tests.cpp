#include "src/core/utils/json/strict.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/fixtures.hpp"
#include "tests/unit/core/kakusan/adapters/shared_setup_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace kakusan_test {
namespace {

using libchiaking::kakusan::Admission;
using test_support::Bytes;
using Fixtures = test_support::kakusan::Fixtures;

class SharedSetupForwarding : public SharedSetupTest {};

TEST_F(SharedSetupForwarding, DecodesTheFixturePacketsAndWhatFollows) {
  const auto document = libchiaking::json::strict::Value::parse(
      Fixtures::text("wire-vectors.json"));
  ASSERT_TRUE(document);
  EXPECT_EQ(
      Fixtures::string_of(Fixtures::member(document.value(), "source_key_hex")),
      "000102030405060708090a0b0c0d0e0f");
  const auto& packets =
      *Fixtures::member(document.value(), "source_packets").array();
  EXPECT_EQ(packets.size(), 3U);

  ASSERT_NO_FATAL_FAILURE(open_receiver());
  for (const auto& packet : packets) {
    const auto bytes = Bytes::unhex(
        Fixtures::string_of(Fixtures::member(packet, "datagram_hex")));
    const auto position = *Fixtures::member(packet, "key_position").integer();
    EXPECT_TRUE(feed(12, position, bytes));
  }
  EXPECT_TRUE(frames_.empty());
  EXPECT_FALSE(receiver_->stream->wants_keyframe());

  for (const auto& vector : after_fixture_frames)
    EXPECT_TRUE(feed(12, vector.key_position, Bytes::unhex(vector.datagram)));
  ASSERT_EQ(frames_.size(), 3U);
  EXPECT_EQ(frames_[0], Bytes::unhex("f8fffe"));
  EXPECT_EQ(frames_[1], Bytes::unhex(after_fixture_frames[0].plaintext));
  EXPECT_EQ(frames_[2], Bytes::unhex(after_fixture_frames[1].plaintext));
}

TEST_F(SharedSetupForwarding, DecodesGeneratedVersion12Frames) {
  ASSERT_NO_FATAL_FAILURE(open_receiver());
  for (const auto& vector : version_12_frames)
    EXPECT_TRUE(feed(12, vector.key_position, Bytes::unhex(vector.datagram)));
  ASSERT_EQ(frames_.size(), version_12_frames.size());
  for (std::size_t index = 0; index < frames_.size(); ++index)
    EXPECT_EQ(frames_[index], Bytes::unhex(version_12_frames[index].plaintext));
}

TEST_F(SharedSetupForwarding, DecodesGeneratedVersion9Frames) {
  ASSERT_NO_FATAL_FAILURE(open_receiver(9));
  for (const auto& vector : version_9_frames)
    EXPECT_TRUE(feed(9, vector.key_position, Bytes::unhex(vector.datagram)));
  ASSERT_EQ(frames_.size(), version_9_frames.size());
  for (std::size_t index = 0; index < frames_.size(); ++index)
    EXPECT_EQ(frames_[index], Bytes::unhex(version_9_frames[index].plaintext));
}

TEST_F(SharedSetupForwarding, RejectsAFrameOfTheOtherVersion) {
  EXPECT_EQ(admit_audio(12, version_9_frames[0].key_position,
                        Bytes::unhex(version_9_frames[0].datagram)),
            verdict(Admission::reject));
  EXPECT_EQ(admit_audio(9, version_12_frames[0].key_position,
                        Bytes::unhex(version_12_frames[0].datagram)),
            verdict(Admission::reject));
}

TEST_F(SharedSetupForwarding, DecodesFramesCarryingTwoUnits) {
  ASSERT_NO_FATAL_FAILURE(open_receiver());
  for (const auto& vector : two_unit_frames)
    EXPECT_TRUE(feed(12, vector.key_position, Bytes::unhex(vector.datagram)));
  ASSERT_EQ(frames_.size(), 4U);
  EXPECT_EQ(frames_[0], Bytes::unhex("a1a2a3"));
  EXPECT_EQ(frames_[1], Bytes::unhex("a4a5a6"));
  EXPECT_EQ(frames_[2], Bytes::unhex("d0d1d2"));
  EXPECT_EQ(frames_[3], Bytes::unhex("d0d1d2"));
}

TEST_F(SharedSetupForwarding, FailsATamperedDatagramAfterAdmittingIt) {
  const auto valid = Bytes::unhex(version_12_frames[0].datagram);
  const auto position = version_12_frames[0].key_position;
  for (const std::size_t offset :
       {std::size_t{1}, std::size_t{3}, std::size_t{10}, std::size_t{13},
        valid.size() - 1}) {
    SCOPED_TRACE(offset);
    auto tampered = valid;
    tampered[offset] ^= std::byte{1};
    ASSERT_NO_FATAL_FAILURE(open_receiver());
    EXPECT_EQ(admit_audio(12, position, tampered), verdict(Admission::forward));
    const auto pushed = receiver_->push(position, tampered);
    ASSERT_FALSE(pushed);
    EXPECT_EQ(pushed.error(), Refusal::authentication_failed);
    EXPECT_TRUE(receiver_->heard.frames.empty());
  }
}

TEST_F(SharedSetupForwarding, RejectsAResizedDatagram) {
  const auto valid = Bytes::unhex(version_12_frames[0].datagram);
  const auto position = version_12_frames[0].key_position;
  auto truncated = valid;
  truncated.pop_back();
  EXPECT_EQ(admit_audio(12, position, truncated), verdict(Admission::reject));
  auto extended = valid;
  extended.push_back(std::byte{0});
  EXPECT_EQ(admit_audio(12, position, extended), verdict(Admission::reject));
}

TEST_F(SharedSetupForwarding, RefusesADatagramAtAShiftedKeyPosition) {
  ASSERT_NO_FATAL_FAILURE(open_receiver());
  const auto shifted =
      receiver_->push(version_12_frames[0].key_position + 1,
                      Bytes::unhex(version_12_frames[0].datagram));
  ASSERT_FALSE(shifted);
  EXPECT_EQ(shifted.error(), Refusal::malformed_datagram);
}

TEST_F(SharedSetupForwarding, NeverPassesHapticsToTheReceiver) {
  const auto bytes = Bytes::unhex(haptics_tag_two);
  EXPECT_EQ(admit_audio(12, 44992, bytes), verdict(Admission::exclude));
  ASSERT_NO_FATAL_FAILURE(open_receiver());
  const auto pushed = receiver_->push(44992, bytes);
  ASSERT_FALSE(pushed);
  EXPECT_EQ(pushed.error(), Refusal::malformed_datagram);
  EXPECT_TRUE(receiver_->heard.frames.empty());
}

}  // namespace
}  // namespace kakusan_test
