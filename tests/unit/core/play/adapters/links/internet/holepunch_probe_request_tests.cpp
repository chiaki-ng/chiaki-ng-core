#include "src/core/net/nat/holepunch/holepunch_probe.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <vector>

namespace play_test {
namespace {

namespace nat = libchiaking::net::nat::holepunch;
namespace pf = libchiaking::platform;
using nat::Probe;

class SizeRecordingRandom final : public pf::SecureRandomProvider {
 public:
  auto fill_bytes(std::span<std::byte> output)
      -> libchiaking::Expected<void, pf::PlatformError> override {
    sizes.push_back(output.size());
    if (fail) return libchiaking::unexpected(pf::PlatformError::io_failure);
    for (std::size_t i = 0; i < output.size(); ++i)
      output[i] = static_cast<std::byte>(0xF0 + static_cast<unsigned>(i));
    return {};
  }

  std::vector<std::size_t> sizes;
  bool fail{};
};

class HolepunchProbeRequest : public ::testing::Test {
 protected:
  static auto byte(unsigned value) -> std::byte {
    return static_cast<std::byte>(value);
  }

  static auto input() -> nat::ProbeRequestInput {
    nat::ProbeRequestInput input;
    for (std::size_t i = 0; i < 20; ++i) {
      input.local_hashed_id[i] = byte(0xA0 + static_cast<unsigned>(i));
      input.console_hashed_id[i] = byte(0x10 + static_cast<unsigned>(i));
    }
    input.local_sid = 0x1234;
    input.console_sid = 0xBEEF;
    return input;
  }

  SizeRecordingRandom random_;
};

TEST_F(HolepunchProbeRequest, DrawsAFiveByteRequestIdFromTheProvider) {
  nat::ProbeRequestInput sids{};
  sids.local_sid = 0x1234;
  sids.console_sid = 0xBEEF;
  const auto result = Probe::make_request(sids, random_);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(random_.sizes.size(), 1U);
  EXPECT_EQ(random_.sizes[0], 5U);
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(result.value().packet[0x4B + i],
              byte(0xF0 + static_cast<unsigned>(i)));
    EXPECT_EQ(result.value().request_id[i],
              byte(0xF0 + static_cast<unsigned>(i)));
  }
  EXPECT_EQ(result.value().response_profile.request_id,
            result.value().request_id);
  EXPECT_EQ(result.value().packet[0x44], byte(0x12));
  EXPECT_EQ(result.value().packet[0x46], byte(0xBE));
}

TEST_F(HolepunchProbeRequest, EncodesTheProbeAndRemembersItsResponseProfile) {
  const auto in = input();
  const auto result = Probe::make_request(in, random_);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(random_.sizes.size(), 1U);
  EXPECT_EQ(random_.sizes[0], 5U);
  const auto& value = result.value();
  EXPECT_EQ(value.packet[0], byte(0x06));
  for (std::size_t i = 0; i < 20; ++i) {
    EXPECT_EQ(value.packet[0x04 + i], in.local_hashed_id[i]) << i;
    EXPECT_EQ(value.packet[0x24 + i], in.console_hashed_id[i]) << i;
  }
  EXPECT_EQ(value.packet[0x44], byte(0x12));
  EXPECT_EQ(value.packet[0x45], byte(0x34));
  EXPECT_EQ(value.packet[0x46], byte(0xBE));
  EXPECT_EQ(value.packet[0x47], byte(0xEF));
  for (std::size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(value.packet[0x4B + i], byte(0xF0 + static_cast<unsigned>(i)));
    EXPECT_EQ(value.request_id[i], value.response_profile.request_id[i]);
  }
  EXPECT_EQ(value.response_profile.local_hashed_id, in.local_hashed_id);
  EXPECT_EQ(value.response_profile.console_hashed_id, in.console_hashed_id);
  EXPECT_EQ(value.response_profile.local_sid, in.local_sid);
  EXPECT_EQ(value.response_profile.console_sid, in.console_sid);
}

TEST_F(HolepunchProbeRequest, RefusesSessionIdsBeyondSixteenBitsBeforeDrawing) {
  auto in = input();
  in.local_sid = 0x10000;
  auto result = Probe::make_request(in, random_);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), nat::ProbeRequestError::local_sid_out_of_range);
  in = input();
  in.console_sid = 0x1FFFF;
  result = Probe::make_request(in, random_);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), nat::ProbeRequestError::console_sid_out_of_range);
  EXPECT_TRUE(random_.sizes.empty());
}

TEST_F(HolepunchProbeRequest, PassesOnARandomFailure) {
  random_.fail = true;
  const auto result = Probe::make_request(input(), random_);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), nat::ProbeRequestError::random_failed);
  EXPECT_EQ(random_.sizes.size(), 1U);
}

TEST_F(HolepunchProbeRequest, PassesOnARandomFailureForBareSessionIds) {
  nat::ProbeRequestInput sids{};
  sids.local_sid = 0x1234;
  sids.console_sid = 0xBEEF;
  random_.fail = true;
  const auto failed = Probe::make_request(sids, random_);
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error(), nat::ProbeRequestError::random_failed);
}

class HolepunchPeerProbeInput : public ::testing::Test {
 protected:
  HolepunchPeerProbeInput() {
    ours_.sid = 0x1234;
    peer_.sid = 0x5678;
    for (std::size_t i = 0; i < 20; ++i) {
      ours_.local_hashed_id[i] = static_cast<std::byte>(i + 1U);
      peer_.local_hashed_id[i] = static_cast<std::byte>(i + 0x40U);
    }
    nat::Candidate first{};
    first.addr = "first";
    first.port = 1001;
    nat::Candidate second{};
    second.addr = "second";
    second.port = 1002;
    peer_.candidates = {first, second};
  }

  auto make() { return Probe::make_peer_probe_input(ours_, peer_); }

  nat::ConnRequest ours_{};
  nat::ConnRequest peer_{};
};

TEST_F(HolepunchPeerProbeInput, PairsOurSessionWithThePeersCandidates) {
  auto input = make();
  ASSERT_TRUE(input);
  EXPECT_EQ(input.value().probe.local_sid, ours_.sid);
  EXPECT_EQ(input.value().probe.console_sid, peer_.sid);
  ASSERT_EQ(input.value().candidates.size(), 2U);
  EXPECT_EQ(input.value().candidates[0].addr, "first");
  EXPECT_EQ(input.value().candidates[1].addr, "second");
  EXPECT_EQ(input.value().candidates[0].port, 1001);
  EXPECT_EQ(input.value().candidates[1].port, 1002);
  for (std::size_t i = 0; i < 20; ++i) {
    EXPECT_EQ(input.value().probe.local_hashed_id[i], ours_.local_hashed_id[i]);
    EXPECT_EQ(input.value().probe.console_hashed_id[i],
              peer_.local_hashed_id[i]);
  }
  EXPECT_EQ(ours_.local_hashed_id[0], std::byte{1});
  EXPECT_EQ(peer_.local_hashed_id[0], std::byte{0x40});
}

TEST_F(HolepunchPeerProbeInput, AcceptsAZeroLocalSessionId) {
  ours_.sid = 0;
  auto input = make();
  ASSERT_TRUE(input);
  EXPECT_EQ(input.value().probe.local_sid, 0);
}

TEST_F(HolepunchPeerProbeInput, RefusesALocalSessionIdBeyondSixteenBits) {
  ours_.sid = 65536;
  auto input = make();
  ASSERT_FALSE(input);
  EXPECT_EQ(input.error(), nat::PeerProbeInputError::invalid_local_offer);
}

TEST_F(HolepunchPeerProbeInput, AcceptsAZeroPeerSessionId) {
  peer_.sid = 0;
  auto input = make();
  ASSERT_TRUE(input);
  EXPECT_EQ(input.value().probe.console_sid, 0);
}

TEST_F(HolepunchPeerProbeInput, RefusesAPeerSessionIdBeyondSixteenBits) {
  peer_.sid = 65536;
  auto input = make();
  ASSERT_FALSE(input);
  EXPECT_EQ(input.error(), nat::PeerProbeInputError::invalid_peer_offer);
}

TEST_F(HolepunchPeerProbeInput, RefusesAPeerWithoutCandidates) {
  peer_.candidates.clear();
  auto input = make();
  ASSERT_FALSE(input);
  EXPECT_EQ(input.error(), nat::PeerProbeInputError::invalid_peer_offer);
}

}  // namespace
}  // namespace play_test
