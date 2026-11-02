#include "src/core/net/kakusan/control_message.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/shared_setup_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace kakusan_test {
namespace {

using libchiaking::net::kakusan::Error;
using libchiaking::net::kakusan::control::SourceCodec;
using test_support::Bytes;

class SharedSetupH264Header : public SharedSetupTest {
 protected:
  static auto accepted(std::span<const std::byte> header) -> bool {
    return SourceSetup::validate_h264_profile_header(header).has_value();
  }
};

class SharedSetupH265Header : public SharedSetupTest {
 protected:
  static auto accepted(std::span<const std::byte> header) -> bool {
    return SourceSetup::validate_h265_profile_header(header).has_value();
  }

  static auto main_with(std::size_t offset, std::uint8_t value)
      -> std::vector<std::byte> {
    auto changed = Bytes::unhex(h265_main_header);
    changed[offset] = static_cast<std::byte>(value);
    return changed;
  }
};

TEST_F(SharedSetupH264Header, AcceptsBaselineAndHighUpToLevel42) {
  EXPECT_TRUE(accepted(fixture_header()));
  EXPECT_TRUE(accepted(Bytes::unhex(high_level_42)));
  EXPECT_TRUE(accepted(sps(100, 42)));
  EXPECT_TRUE(accepted(sps(100, 41)));
  EXPECT_TRUE(accepted(sps(100, 10)));
  EXPECT_TRUE(accepted(sps(66, 42)));
  EXPECT_TRUE(accepted(sps(66, 40, true, 0xc0)));
  EXPECT_TRUE(accepted(sps(66, 31, true, 0x40)));
  EXPECT_TRUE(accepted(sps(66, 42, false)));
  EXPECT_TRUE(accepted(sps(100, 42, false)));
}

TEST_F(SharedSetupH264Header, RejectsHigherLevelsAndOtherProfiles) {
  EXPECT_FALSE(accepted(sps(100, 43)));
  EXPECT_FALSE(accepted(sps(100, 51)));
  EXPECT_FALSE(accepted(sps(66, 50)));
  EXPECT_FALSE(accepted(sps(100, 255)));
  EXPECT_FALSE(accepted(sps(100, 0)));
  for (const unsigned profile :
       {0U, 44U, 77U, 88U, 110U, 122U, 244U, 83U, 86U, 118U, 128U, 255U}) {
    SCOPED_TRACE(profile);
    EXPECT_FALSE(accepted(sps(static_cast<std::uint8_t>(profile), 30)));
  }
}

TEST_F(SharedSetupH264Header, NeedsAWholeSequenceParameterSet) {
  EXPECT_FALSE(accepted({}));
  EXPECT_FALSE(accepted(Bytes::unhex("00")));
  EXPECT_FALSE(accepted(Bytes::unhex("000001")));
  EXPECT_FALSE(accepted(Bytes::unhex("00000001")));
  EXPECT_FALSE(accepted(Bytes::unhex("0000000167")));
  EXPECT_FALSE(accepted(Bytes::unhex("000000016764")));
  EXPECT_FALSE(accepted(Bytes::unhex("00000001676400")));
  EXPECT_TRUE(accepted(Bytes::unhex("0000000167640029")));
  EXPECT_FALSE(accepted(Bytes::unhex("64002aacd9405005bb01")));
  EXPECT_FALSE(accepted(Bytes::unhex("0000000268ebe3cb22c0")));
  EXPECT_FALSE(accepted(Bytes::unhex("0000000168ebe3cb22c0")));
  EXPECT_FALSE(accepted(Bytes::unhex("00000001e7640029")));
  EXPECT_FALSE(accepted(Bytes::unhex("0000000166640029")));
}

TEST_F(SharedSetupH264Header, FindsTheSequenceParameterSetAfterOtherUnits) {
  EXPECT_TRUE(accepted(Bytes::unhex("00000001091000000167640029")));
  EXPECT_TRUE(accepted(Bytes::unhex("00000001680000000167640029")));
  EXPECT_FALSE(accepted(Bytes::unhex("00000001680000000167770029")));
  EXPECT_FALSE(accepted(Bytes::unhex("00000001680000000167640036")));
}

TEST_F(SharedSetupH264Header, ReportsARejectionAsACodecError) {
  EXPECT_TRUE(fails_with(SourceSetup::validate_h264_profile_header(
                             Bytes::unhex("0000000168ebe3cb22c0")),
                         Error::codec));
}

TEST_F(SharedSetupH265Header, FixturesHoldTheExpectedProfileAndLevel) {
  const auto main = Bytes::unhex(h265_main_header);
  const auto main10 = Bytes::unhex(h265_main10_header);
  EXPECT_EQ(main[h265_sps_offset - 1], std::byte{1});
  EXPECT_EQ(main[h265_sps_offset], std::byte{0x42});
  EXPECT_EQ(main[h265_profile_byte], std::byte{1});
  EXPECT_EQ(main[h265_level_byte], std::byte{120});
  EXPECT_EQ(main10[h265_profile_byte], std::byte{2});
}

TEST_F(SharedSetupH265Header, DetectsTheCodecFromTheParameterSets) {
  const auto main = SourceSetup::detect_codec(Bytes::unhex(h265_main_header));
  ASSERT_TRUE(main);
  EXPECT_EQ(main.value(), SourceCodec::h265);
  const auto main10 =
      SourceSetup::detect_codec(Bytes::unhex(h265_main10_header));
  ASSERT_TRUE(main10);
  EXPECT_EQ(main10.value(), SourceCodec::h265);
  const auto high = SourceSetup::detect_codec(Bytes::unhex(h264_high_header));
  ASSERT_TRUE(high);
  EXPECT_EQ(high.value(), SourceCodec::h264);
  const auto fixture = SourceSetup::detect_codec(fixture_header());
  ASSERT_TRUE(fixture);
  EXPECT_EQ(fixture.value(), SourceCodec::h264);
  EXPECT_FALSE(SourceSetup::detect_codec(Bytes::unhex("0000000168ebe3cb22c0")));
  EXPECT_FALSE(
      SourceSetup::detect_codec(Bytes::unhex("0000000140010c01ffff0160")));
  EXPECT_FALSE(SourceSetup::detect_codec({}));
}

TEST_F(SharedSetupH265Header, IsValidatedOnlyAsItsOwnCodec) {
  const auto main = Bytes::unhex(h265_main_header);
  const auto main10 = Bytes::unhex(h265_main10_header);
  const auto high = Bytes::unhex(h264_high_header);
  EXPECT_TRUE(accepted(main));
  EXPECT_TRUE(accepted(main10));
  EXPECT_TRUE(SourceSetup::validate_h264_profile_header(high));
  EXPECT_FALSE(accepted(high));
  EXPECT_FALSE(SourceSetup::validate_h264_profile_header(main));
  EXPECT_TRUE(SourceSetup::validate_profile_header(SourceCodec::h265, main));
  EXPECT_FALSE(SourceSetup::validate_profile_header(SourceCodec::h264, main));
  EXPECT_FALSE(SourceSetup::validate_profile_header(SourceCodec::h265, high));
}

TEST_F(SharedSetupH265Header, AcceptsOnlyMainProfilesUpToLevel51) {
  EXPECT_FALSE(accepted(main_with(h265_profile_byte, 3)));
  EXPECT_FALSE(accepted(main_with(h265_profile_byte, 4)));
  EXPECT_FALSE(accepted(main_with(h265_profile_byte, 0)));
  EXPECT_FALSE(accepted(main_with(h265_profile_byte, 0x41)));
  EXPECT_TRUE(accepted(main_with(h265_profile_byte, 0x21)));
  EXPECT_TRUE(accepted(main_with(h265_level_byte, 153)));
  EXPECT_FALSE(accepted(main_with(h265_level_byte, 154)));
  EXPECT_FALSE(accepted(main_with(h265_level_byte, 0)));
  EXPECT_TRUE(accepted(main_with(h265_level_byte, 1)));
}

TEST_F(SharedSetupH265Header, NeedsTheWholeProfileTierAndLevel) {
  const auto main = Bytes::unhex(h265_main_header);
  for (std::size_t size = h265_sps_offset; size <= h265_level_byte; ++size) {
    SCOPED_TRACE(size);
    auto truncated = main;
    truncated.resize(size);
    EXPECT_FALSE(accepted(truncated));
  }
  auto exact = main;
  exact.resize(h265_level_byte + 1);
  EXPECT_TRUE(accepted(exact));
  EXPECT_FALSE(accepted(main_with(h265_sps_offset, 0x43)));
  EXPECT_FALSE(accepted(main_with(h265_sps_offset, 0xc2)));
}

}  // namespace
}  // namespace kakusan_test
