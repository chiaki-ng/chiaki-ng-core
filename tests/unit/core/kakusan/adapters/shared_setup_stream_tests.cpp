#include "src/core/kakusan/adapters/shared_setup.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/shared_setup_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace kakusan_test {
namespace {

using libchiaking::kakusan::SharedSetup;
using libchiaking::net::kakusan::Error;
using test_support::Bytes;

static_assert(!std::is_copy_constructible_v<
              libchiaking::net::kakusan::control::SourceSetup>);
static_assert(!std::is_copy_constructible_v<SharedSetup::Stream>);

class SharedSetupStream : public SharedSetupTest {
 protected:
  static auto refused(const SourceSetup& changed)
      -> ::testing::AssertionResult {
    return fails_with(forward(changed), Error::codec);
  }

  const SourceSetup setup_{fixture_setup()};
};

TEST_F(SharedSetupStream, CarriesTheFixtureSetup) {
  const auto forwarded = forward(setup_);
  ASSERT_TRUE(forwarded);
  const auto& stream = forwarded.value().stream;
  EXPECT_EQ(stream.protocol_version, 12U);
  EXPECT_EQ(stream.frames_per_second, setup_.video_fps);
  EXPECT_EQ(Bytes::hex(forwarded.value().keys.key()),
            "000102030405060708090a0b0c0d0e0f");
  EXPECT_EQ(Bytes::hex(forwarded.value().keys.iv()),
            "101112131415161718191a1b1c1d1e1f");
  ASSERT_EQ(stream.description.profiles.size(), 1U);
  EXPECT_EQ(stream.description.profiles[0].width, 1280U);
  EXPECT_EQ(stream.description.profiles[0].height, 720U);
  EXPECT_EQ(stream.description.profiles[0].header,
            setup_.video_profiles[0].header);
  EXPECT_EQ(Bytes::hex(stream.description.audio_header), fixture_audio_header);
}

TEST_F(SharedSetupStream, KeepsProtocolNine) {
  auto nine = setup_.clone();
  nine.source_protocol = 9;
  const auto forwarded = forward(nine);
  ASSERT_TRUE(forwarded);
  EXPECT_EQ(forwarded.value().stream.protocol_version, 9U);
}

TEST_F(SharedSetupStream, RefusesASetupOutsideTheRules) {
  auto changed = setup_.clone();
  changed.source_protocol = 7;
  EXPECT_TRUE(refused(changed));
  changed = setup_.clone();
  changed.video_fps = 0;
  EXPECT_TRUE(refused(changed));
  changed = setup_.clone();
  changed.video_profiles[0].width = 1921;
  EXPECT_TRUE(refused(changed));
  changed = setup_.clone();
  changed.audio_header = audio_header_with(2, 16, 44100, 960);
  EXPECT_TRUE(refused(changed));
}

TEST_F(SharedSetupStream, RefusesProfileHeadersItCannotDecode) {
  auto changed = setup_.clone();
  changed.video_profiles[0].header = Bytes::unhex("0000000168ebe3cb22c0");
  EXPECT_TRUE(refused(changed));
  changed.video_profiles[0].header = sps(77, 40);
  EXPECT_TRUE(refused(changed));
  changed.video_profiles[0].header = sps(100, 50);
  EXPECT_TRUE(refused(changed));
}

TEST_F(SharedSetupStream, ChecksEveryProfileHeader) {
  auto changed = setup_.clone();
  changed.video_profiles[0].header = sps(66, 31, true, 0xc0);
  EXPECT_TRUE(forward(changed));
  changed.video_profiles.push_back(setup_.video_profiles[0]);
  changed.video_profiles.push_back(setup_.video_profiles[0]);
  changed.video_profiles[2].header = sps(110, 40);
  EXPECT_TRUE(refused(changed));
  changed.video_profiles[2].header = sps(100, 42);
  const auto three = forward(changed);
  ASSERT_TRUE(three);
  ASSERT_EQ(three.value().stream.description.profiles.size(), 3U);
  EXPECT_EQ(three.value().stream.description.profiles[0].header,
            sps(66, 31, true, 0xc0));
  EXPECT_EQ(three.value().stream.description.profiles[2].header, sps(100, 42));
}

TEST_F(SharedSetupStream, HandsTheKeysToTheirNewOwner) {
  auto setup = fixture_setup();
  auto forwarded = SharedSetup::stream(std::move(setup));
  EXPECT_EQ(setup.receive_key, libchiaking::net::kakusan::control::SourceKey{});
  EXPECT_EQ(setup.receive_iv, libchiaking::net::kakusan::control::SourceKey{});
  ASSERT_TRUE(forwarded);
  auto owned = std::move(forwarded.value().keys);
  EXPECT_TRUE(
      std::ranges::all_of(forwarded.value().keys.key(),
                          [](std::byte byte) { return byte == std::byte{}; }));
  EXPECT_TRUE(
      std::ranges::all_of(forwarded.value().keys.iv(),
                          [](std::byte byte) { return byte == std::byte{}; }));
  EXPECT_EQ(Bytes::hex(owned.key()), "000102030405060708090a0b0c0d0e0f");
  EXPECT_EQ(forwarded.value().stream.description.profiles.size(), 1U);
  EXPECT_EQ(forwarded.value().stream.protocol_version, 12U);
}

}  // namespace
}  // namespace kakusan_test
