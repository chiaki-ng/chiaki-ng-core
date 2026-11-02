#include "src/core/net/kakusan/control_message.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/shared_setup_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace kakusan_test {
namespace {

class SharedSetupAudioHeader : public SharedSetupTest {
 protected:
  static auto accepted(std::span<const std::byte> header) -> bool {
    return libchiaking::net::kakusan::control::SourceSetup::
        validate_audio_header(header)
            .has_value();
  }
};

TEST_F(SharedSetupAudioHeader, AcceptsTheFixtureMonoAndAnyOpaqueTail) {
  EXPECT_TRUE(accepted(audio_header_of(fixture_audio_header)));
  EXPECT_TRUE(accepted(audio_header_with(1, 16, 48000, 960)));
  EXPECT_TRUE(accepted(audio_header_with(2, 16, 48000, 960, 0)));
  EXPECT_TRUE(accepted(audio_header_with(2, 16, 48000, 960, 0xffffffffU)));
}

TEST_F(SharedSetupAudioHeader, AcceptsOnlyOpusFrameSizes) {
  for (const std::uint32_t frame_size :
       {120U, 240U, 480U, 960U, 1920U, 2880U}) {
    SCOPED_TRACE(frame_size);
    EXPECT_TRUE(accepted(audio_header_with(2, 16, 48000, frame_size)));
  }
  for (const std::uint32_t frame_size : {0U, 1U, 119U, 121U, 441U, 1000U, 1919U,
                                         2881U, 3840U, 5760U, 0xffffffffU}) {
    SCOPED_TRACE(frame_size);
    EXPECT_FALSE(accepted(audio_header_with(2, 16, 48000, frame_size)));
  }
}

TEST_F(SharedSetupAudioHeader, RejectsOtherChannelsBitsAndRates) {
  for (const unsigned channels : {0U, 3U, 6U, 255U}) {
    SCOPED_TRACE(channels);
    EXPECT_FALSE(accepted(audio_header_with(static_cast<std::uint8_t>(channels),
                                            16, 48000, 960)));
  }
  for (const unsigned bits : {0U, 8U, 15U, 17U, 24U, 32U}) {
    SCOPED_TRACE(bits);
    EXPECT_FALSE(accepted(
        audio_header_with(2, static_cast<std::uint8_t>(bits), 48000, 960)));
  }
  for (const std::uint32_t rate :
       {0U, 8000U, 44100U, 47999U, 48001U, 96000U, 0xbb800000U}) {
    SCOPED_TRACE(rate);
    EXPECT_FALSE(accepted(audio_header_with(2, 16, rate, 960)));
  }
}

TEST_F(SharedSetupAudioHeader, RejectsAnyOtherLength) {
  const auto header = audio_header_of(fixture_audio_header);
  EXPECT_FALSE(accepted(std::span<const std::byte>{header}.first(13)));
  std::array<std::byte, 15> longer{};
  std::ranges::copy(header, longer.begin());
  EXPECT_FALSE(accepted(longer));
  EXPECT_FALSE(accepted({}));
}

TEST_F(SharedSetupAudioHeader, ReportsARejectionAsACodecError) {
  EXPECT_TRUE(fails_with(
      libchiaking::net::kakusan::control::SourceSetup::validate_audio_header(
          audio_header_with(2, 16, 44100, 960)),
      libchiaking::net::kakusan::Error::codec));
}

}  // namespace
}  // namespace kakusan_test
