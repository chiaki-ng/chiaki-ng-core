#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/kakusan/adapters/capture.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace kakusan_test {
namespace {

using libchiaking::SecretArray;
using libchiaking::kakusan::Capture;
using libchiaking::kakusan::capture_byte_limit;
using libchiaking::kakusan::capture_datagram_limit;
using libchiaking::kakusan::CapturedKeys;
using libchiaking::play::ReceiveKeys;
using Media = libchiaking::play::SharedDatagram::Media;

static_assert(!std::is_copy_constructible_v<CapturedKeys>);
static_assert(!std::is_copy_assignable_v<CapturedKeys>);
static_assert(std::is_nothrow_move_constructible_v<CapturedKeys>);
static_assert(!std::is_default_constructible_v<ReceiveKeys>);
static_assert(!std::is_copy_constructible_v<ReceiveKeys>);
static_assert(std::is_nothrow_move_constructible_v<ReceiveKeys>);

class CaptureTest : public ::testing::Test {
 protected:
  static auto sealed(Media media, std::uint64_t position,
                     const std::vector<std::byte>& bytes)
      -> libchiaking::play::SharedDatagram {
    libchiaking::play::SharedDatagram datagram;
    datagram.media = media;
    datagram.key_position = position;
    datagram.bytes = bytes;
    return datagram;
  }

  static auto zeroed(std::span<const std::byte> bytes) -> bool {
    return std::ranges::all_of(
        bytes, [](std::byte byte) { return byte == std::byte{}; });
  }

  static auto keys(std::byte first_key, std::byte first_iv) -> ReceiveKeys {
    SecretArray<ReceiveKeys::size> key;
    SecretArray<ReceiveKeys::size> iv;
    key[0] = first_key;
    iv[0] = first_iv;
    return ReceiveKeys{std::move(key), std::move(iv)};
  }

  libchiaking::platform::DefaultThreadingProvider threading;
  Capture capture{threading};
};

class CaptureForwarding : public CaptureTest {
 protected:
  void SetUp() override { capture.set_forwarding(true); }
};

TEST_F(CaptureTest, CopiesNothingUntilForwarding) {
  const std::vector<std::byte> bytes(40, std::byte{0x11});
  capture.datagram(sealed(Media::video, 16, bytes));
  const auto batch = capture.drain();
  EXPECT_TRUE(batch.datagrams.empty());
  EXPECT_EQ(batch.dropped.dropped_video, 0U);
  EXPECT_EQ(batch.dropped.dropped_audio, 0U);
  EXPECT_FALSE(capture.clone_keys());
}

TEST_F(CaptureForwarding, CopiesEveryNonEmptyDatagramOnce) {
  const std::vector<std::byte> bytes(40, std::byte{0x11});
  capture.datagram(sealed(Media::video, 16, bytes));
  capture.datagram(sealed(Media::audio, 64, bytes));
  capture.datagram(sealed(Media::audio, 80, {}));
  auto batch = capture.drain();
  const auto& out = batch.datagrams;
  ASSERT_EQ(out.size(), 2U);
  EXPECT_EQ(out[0].facts.media, Media::video);
  EXPECT_EQ(out[0].facts.key_position, 16U);
  EXPECT_EQ(out[1].facts.media, Media::audio);
  EXPECT_EQ(out[1].facts.key_position, 64U);
  EXPECT_EQ(out[0].bytes, bytes);
  EXPECT_TRUE(capture.drain().datagrams.empty());
}

TEST_F(CaptureForwarding, DropsWhatWaitsWhenForwardingStops) {
  capture.datagram(sealed(Media::video, 96, std::vector<std::byte>(40)));
  capture.set_forwarding(false);
  EXPECT_TRUE(capture.drain().datagrams.empty());
}

TEST(CapturedReceiveKeys, MoveOutAndWipeTheSource) {
  SecretArray<ReceiveKeys::size> key;
  SecretArray<ReceiveKeys::size> iv;
  key[0] = std::byte{1};
  iv[0] = std::byte{2};
  ReceiveKeys keys{std::move(key), std::move(iv)};
  EXPECT_EQ(key[0], std::byte{});
  EXPECT_EQ(iv[0], std::byte{});
  auto owned_clone = keys.clone();
  auto moved_keys = std::move(owned_clone);
  EXPECT_EQ(moved_keys.key()[0], std::byte{1});
  EXPECT_EQ(moved_keys.iv()[0], std::byte{2});
  EXPECT_TRUE(std::ranges::all_of(
      owned_clone.key(), [](std::byte byte) { return byte == std::byte{}; }));
  EXPECT_TRUE(std::ranges::all_of(
      owned_clone.iv(), [](std::byte byte) { return byte == std::byte{}; }));
}

TEST_F(CaptureForwarding, VersionsEachKeyChange) {
  const auto given = keys(std::byte{1}, std::byte{2});
  capture.keys(12, given.clone());
  auto first = capture.clone_keys();
  ASSERT_TRUE(first);
  EXPECT_EQ(first->version(), 12U);
  EXPECT_EQ(first->generation(), 1U);
  EXPECT_TRUE(std::ranges::equal(first->key(), given.key()));
  EXPECT_TRUE(std::ranges::equal(first->iv(), given.iv()));

  capture.datagram(
      sealed(Media::video, 16, std::vector<std::byte>(8, std::byte{0x22})));
  capture.keys(9, keys(std::byte{9}, std::byte{2}));
  const auto second = capture.clone_keys();
  ASSERT_TRUE(second);
  EXPECT_EQ(second->version(), 9U);
  EXPECT_EQ(second->generation(), 2U);
  EXPECT_EQ(first->key()[0], std::byte{1});
  EXPECT_TRUE(capture.drain().datagrams.empty());
}

TEST_F(CaptureForwarding, WipesCapturedKeysThatAreMovedOut) {
  capture.keys(12, keys(std::byte{1}, std::byte{2}));
  auto first = capture.clone_keys();
  ASSERT_TRUE(first);
  auto cloned = first->clone();
  auto moved = std::move(*first);
  EXPECT_TRUE(std::ranges::equal(moved.key(), cloned.key()));
  EXPECT_TRUE(zeroed(first->key()));
  EXPECT_TRUE(zeroed(first->iv()));
}

TEST_F(CaptureForwarding, ForgetsKeysAndDatagramsOnReset) {
  const std::vector<std::byte> bytes(8, std::byte{0x22});
  capture.keys(12, keys(std::byte{1}, std::byte{2}));
  capture.datagram(sealed(Media::video, 32, bytes));
  capture.reset();
  EXPECT_FALSE(capture.clone_keys());
  EXPECT_TRUE(capture.drain().datagrams.empty());
  capture.datagram(sealed(Media::video, 48, bytes));
  EXPECT_TRUE(capture.drain().datagrams.empty());
}

TEST_F(CaptureForwarding, DropsTheOldestBeyondTheDatagramLimitAndCountsThem) {
  const std::vector<std::byte> small(16, std::byte{0x33});
  for (std::size_t index = 0; index < capture_datagram_limit + 3; ++index)
    capture.datagram(
        sealed(index % 2 == 0 ? Media::video : Media::audio, index, small));
  const auto batch = capture.drain();
  ASSERT_EQ(batch.datagrams.size(), capture_datagram_limit);
  EXPECT_EQ(batch.datagrams.front().facts.key_position, 3U);
  EXPECT_EQ(batch.dropped.dropped_video, 2U);
  EXPECT_EQ(batch.dropped.dropped_audio, 1U);
  const auto next = capture.drain();
  EXPECT_EQ(next.dropped.dropped_video, 0U);
  EXPECT_EQ(next.dropped.dropped_audio, 0U);
}

TEST_F(CaptureForwarding, DropsTheOldestBeyondTheByteLimit) {
  const std::vector<std::byte> large(1024 * 1024, std::byte{0x44});
  for (std::uint64_t index = 0; index < 5; ++index)
    capture.datagram(sealed(Media::video, index, large));
  const auto batch = capture.drain();
  ASSERT_EQ(batch.datagrams.size(), 4U);
  EXPECT_EQ(batch.datagrams.front().facts.key_position, 1U);
  EXPECT_EQ(batch.dropped.dropped_video, 1U);
}

TEST_F(CaptureForwarding, RefusesADatagramLargerThanTheByteLimit) {
  const std::vector<std::byte> oversized(capture_byte_limit + 1,
                                         std::byte{0x55});
  capture.datagram(sealed(Media::video, 0, oversized));
  EXPECT_TRUE(capture.drain().datagrams.empty());
}

}  // namespace
}  // namespace kakusan_test
