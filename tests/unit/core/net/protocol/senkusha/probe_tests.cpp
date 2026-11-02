#include "src/core/net/protocol/senkusha/probe.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace senkusha_test {
namespace {

namespace senkusha = libchiaking::net::protocol::senkusha;
namespace takion = libchiaking::net::protocol::takion;

class SenkushaProbeCodec : public ::testing::Test {
 protected:
  using Status = senkusha::ProbeStatus;
  using Error = senkusha::Error;

  static auto profile(std::uint32_t version) -> takion::WireProfile {
    auto resolved = takion::resolve_takion_wire_profile(version);
    EXPECT_TRUE(resolved) << "version " << version;
    return std::move(resolved).value();
  }

  template <class Outcome>
  static auto status_of(const Outcome& result) -> std::optional<Status> {
    if (!result) return std::nullopt;
    return result.value().status;
  }

  template <class Outcome>
  static auto error_of(const Outcome& result) -> std::optional<Error> {
    if (result) return std::nullopt;
    return result.error();
  }

  static auto rtt_pong(takion::SenkushaProbeMedia media,
                       std::span<const std::byte> bytes, bool nalu,
                       std::uint16_t test, std::uint16_t unit,
                       std::uint32_t tag) {
    return senkusha::ProbeCodec::classify_local_rtt_pong(
        media, bytes, nalu, profile(7), senkusha::ProbePolicy{}, test, unit,
        tag);
  }

  static auto mtu_response(takion::SenkushaProbeMedia media,
                           std::span<const std::byte> bytes, bool nalu,
                           std::uint32_t request_id) {
    return senkusha::ProbeCodec::classify_local_inbound_mtu_response(
        media, bytes, nalu, profile(7), request_id);
  }

  static auto mtu_video() -> std::vector<std::byte> {
    std::vector<std::byte> video(0x14, std::byte{0});
    video[2] = std::byte{0x0b};
    video[3] = std::byte{0x0c};
    return video;
  }

  static auto nalu_mtu_video() -> std::vector<std::byte> {
    std::vector<std::byte> video(0x17, std::byte{0});
    video[2] = std::byte{0x0b};
    video[3] = std::byte{0x0c};
    return video;
  }

  static auto outbound(
      std::uint32_t mtu, std::uint16_t attempt, std::uint32_t tag,
      std::uint32_t version = 7,
      takion::PacketEnvelope envelope = takion::PlainEnvelope{}) {
    return senkusha::ProbeCodec::build_local_outbound_mtu_probe(
        mtu, attempt, tag, profile(version), envelope, senkusha::MtuPolicy{});
  }
};

TEST_F(SenkushaProbeCodec, BuildsTheExactLocalRttProbe) {
  const auto encoded = senkusha::ProbeCodec::build_local_rtt_probe(
      0x1234, 0x56, 0xaabbccddU, profile(7), takion::PlainEnvelope{});
  ASSERT_TRUE(encoded);
  const auto& av = encoded.value().encoded_av_payload;
  EXPECT_EQ(encoded.value().datagram_size, 0x224U);
  EXPECT_EQ(encoded.value().family, takion::PacketFamily::audio_and_haptics);
  ASSERT_EQ(av.size(), 0x223U);
  EXPECT_EQ(av[0], std::byte{0});
  EXPECT_EQ(av[1], std::byte{0});
  EXPECT_EQ(av[2], std::byte{0x12});
  EXPECT_EQ(av[3], std::byte{0x34});
  EXPECT_EQ(av[4], std::byte{0x0a});
  EXPECT_EQ(av[5], std::byte{0xdf});
  EXPECT_EQ(av[6], std::byte{0xfc});
  EXPECT_EQ(av[7], std::byte{0x00});
  EXPECT_EQ(av[8], std::byte{0xff});
  EXPECT_EQ(av[0x15], std::byte{0xaa});
  EXPECT_EQ(av[0x16], std::byte{0xbb});
  EXPECT_EQ(av[0x17], std::byte{0xcc});
  EXPECT_EQ(av[0x18], std::byte{0xdd});
  for (std::size_t index = 0x19; index < av.size(); ++index)
    EXPECT_EQ(av[index], std::byte{0}) << "index " << index;
}

TEST_F(SenkushaProbeCodec, TellsAMatchingPongFromUnrelatedOnes) {
  const auto encoded = senkusha::ProbeCodec::build_local_rtt_probe(
      0x42, 3, 0x11223344U, profile(7), takion::PlainEnvelope{});
  ASSERT_TRUE(encoded);
  const auto& bytes = encoded.value().encoded_av_payload;
  const auto matched = rtt_pong(takion::SenkushaProbeMedia::audio, bytes, false,
                                0x42, 3, 0x11223344U);
  ASSERT_TRUE(matched);
  EXPECT_EQ(matched.value().status, Status::matched);
  EXPECT_EQ(status_of(rtt_pong(takion::SenkushaProbeMedia::video, bytes, false,
                               0x42, 3, 0x11223344U)),
            Status::unrelated);
  EXPECT_EQ(status_of(rtt_pong(takion::SenkushaProbeMedia::audio, bytes, false,
                               0x43, 3, 0x11223344U)),
            Status::unrelated);
  EXPECT_EQ(status_of(rtt_pong(takion::SenkushaProbeMedia::audio, bytes, false,
                               0x42, 4, 0x11223344U)),
            Status::unrelated);
  EXPECT_EQ(status_of(rtt_pong(takion::SenkushaProbeMedia::audio, bytes, false,
                               0x42, 3, 0x11223345U)),
            Status::unrelated);
}

TEST_F(SenkushaProbeCodec, SkipsTheNaluInfoBytesOfAFlaggedPong) {
  const auto encoded = senkusha::ProbeCodec::build_local_rtt_probe(
      0x42, 3, 0x11223344U, profile(7), takion::PlainEnvelope{});
  ASSERT_TRUE(encoded);
  const auto& plain = encoded.value().encoded_av_payload;
  std::vector<std::byte> flagged(plain.begin(), plain.begin() + 0x11);
  flagged.insert(flagged.end(), 3, std::byte{0x5a});
  flagged.insert(flagged.end(), plain.begin() + 0x11, plain.end());
  const auto classify = [](std::span<const std::byte> bytes, bool nalu) {
    return rtt_pong(takion::SenkushaProbeMedia::audio, bytes, nalu, 0x42, 3,
                    0x11223344U);
  };
  const auto matched = classify(flagged, true);
  ASSERT_TRUE(matched);
  EXPECT_EQ(matched.value().status, Status::matched);
  const auto shifted = classify(flagged, false);
  ASSERT_TRUE(shifted);
  EXPECT_EQ(shifted.value().status, Status::unrelated);
  EXPECT_EQ(status_of(classify(
                std::span<const std::byte>{flagged.data(), 0x14 + 8}, true)),
            Status::matched);
  const auto truncated =
      classify(std::span<const std::byte>{flagged.data(), 0x14 + 7}, true);
  ASSERT_FALSE(truncated);
  EXPECT_EQ(truncated.error(), Error::malformed_response);
}

TEST_F(SenkushaProbeCodec, RejectsATruncatedRttPong) {
  const auto encoded = senkusha::ProbeCodec::build_local_rtt_probe(
      1, 1, 0xdeadbeefU, profile(7), takion::PlainEnvelope{});
  ASSERT_TRUE(encoded);
  const auto& full = encoded.value().encoded_av_payload;
  for (std::size_t size = 0; size < 0x19; ++size)
    EXPECT_EQ(error_of(rtt_pong(takion::SenkushaProbeMedia::audio,
                                std::span<const std::byte>{full.data(), size},
                                false, 1, 1, 0xdeadbeefU)),
              Error::malformed_response)
        << "size " << size;
}

TEST_F(SenkushaProbeCodec, RejectsAnRttPongTheProbePolicyDoesNotFit) {
  const auto encoded = senkusha::ProbeCodec::build_local_rtt_probe(
      1, 1, 0xdeadbeefU, profile(7), takion::PlainEnvelope{});
  ASSERT_TRUE(encoded);
  const auto& full = encoded.value().encoded_av_payload;
  auto policy = senkusha::ProbePolicy{};
  policy.minimum_payload_size = 9;
  EXPECT_FALSE(senkusha::ProbeCodec::classify_local_rtt_pong(
      takion::SenkushaProbeMedia::audio,
      std::span<const std::byte>{full.data(), 0x19}, false, profile(7), policy,
      1, 1, 0xdeadbeefU));
  policy.minimum_payload_size = 8;
  policy.tag_offset = 6;
  EXPECT_FALSE(senkusha::ProbeCodec::classify_local_rtt_pong(
      takion::SenkushaProbeMedia::audio,
      std::span<const std::byte>{full.data(), 0x19}, false, profile(7), policy,
      1, 1, 0xdeadbeefU));
}

TEST_F(SenkushaProbeCodec, SpeaksTheRttProbeOnlyInTheVersion7Layout) {
  const auto encoded = senkusha::ProbeCodec::build_local_rtt_probe(
      1, 1, 0xdeadbeefU, profile(7), takion::PlainEnvelope{});
  ASSERT_TRUE(encoded);
  EXPECT_FALSE(senkusha::ProbeCodec::classify_local_rtt_pong(
      takion::SenkushaProbeMedia::audio, encoded.value().encoded_av_payload,
      false, profile(20), senkusha::ProbePolicy{}, 1, 1, 0xdeadbeefU));
  EXPECT_FALSE(senkusha::ProbeCodec::build_local_rtt_probe(
      1, 1, 1, profile(20), takion::PlainEnvelope{}));
}

TEST_F(SenkushaProbeCodec, CountsThePsnEnvelopeInTheRttProbeSize) {
  const auto wrapped = senkusha::ProbeCodec::build_local_rtt_probe(
      1, 1, 1, profile(9), takion::PsnEnvelope{1});
  ASSERT_TRUE(wrapped);
  EXPECT_EQ(wrapped.value().datagram_size, 0x228U);
}

TEST_F(SenkushaProbeCodec, TellsAMatchingInboundMtuResponseFromUnrelatedOnes) {
  const auto video = mtu_video();
  const auto matched =
      mtu_response(takion::SenkushaProbeMedia::video, video, false, 0x0b0c);
  ASSERT_TRUE(matched);
  EXPECT_EQ(matched.value().status, Status::matched);
  EXPECT_EQ(status_of(mtu_response(takion::SenkushaProbeMedia::audio, video,
                                   false, 0x0b0c)),
            Status::unrelated);
  EXPECT_EQ(status_of(mtu_response(takion::SenkushaProbeMedia::video, video,
                                   false, 0x0b0d)),
            Status::unrelated);
}

TEST_F(SenkushaProbeCodec, RejectsATruncatedInboundMtuResponse) {
  const auto video = mtu_video();
  for (std::size_t size = 0; size < 0x14; ++size)
    EXPECT_EQ(
        error_of(mtu_response(takion::SenkushaProbeMedia::video,
                              std::span<const std::byte>{video.data(), size},
                              false, 0x0b0c)),
        Error::malformed_response)
        << "size " << size;
}

TEST_F(SenkushaProbeCodec, ReadsAFlaggedInboundMtuResponsePastItsNaluInfo) {
  const auto nalu_video = nalu_mtu_video();
  for (std::size_t size = 0; size < 0x17; ++size)
    EXPECT_EQ(
        error_of(mtu_response(
            takion::SenkushaProbeMedia::video,
            std::span<const std::byte>{nalu_video.data(), size}, true, 0x0b0c)),
        Error::malformed_response)
        << "size " << size;
  const auto nalu_matched =
      mtu_response(takion::SenkushaProbeMedia::video, nalu_video, true, 0x0b0c);
  ASSERT_TRUE(nalu_matched);
  EXPECT_EQ(nalu_matched.value().status, Status::matched);
}

TEST_F(SenkushaProbeCodec,
       RejectsAnInboundMtuResponseOutsideTheVersion7Layout) {
  const auto unsupported =
      senkusha::ProbeCodec::classify_local_inbound_mtu_response(
          takion::SenkushaProbeMedia::video, mtu_video(), false, profile(20),
          0x0b0c);
  ASSERT_FALSE(unsupported);
  EXPECT_EQ(unsupported.error(), Error::malformed_response);
}

TEST_F(SenkushaProbeCodec, RejectsAnInboundMtuRequestIdWiderThanSixteenBits) {
  const auto oversized = mtu_response(takion::SenkushaProbeMedia::video,
                                      mtu_video(), false, 0x1'0000);
  ASSERT_FALSE(oversized);
  EXPECT_EQ(oversized.error(), Error::arithmetic_overflow);
}

TEST_F(SenkushaProbeCodec, BuildsTheSmallestOutboundMtuProbe) {
  const auto minimum = outbound(54, 0x123, 0x01020304U);
  ASSERT_TRUE(minimum);
  EXPECT_EQ(minimum.value().datagram_size, 26U);
  const auto& small = minimum.value().encoded_av_payload;
  ASSERT_EQ(small.size(), 25U);
  EXPECT_EQ(small[0], std::byte{0});
  EXPECT_EQ(small[1], std::byte{0});
  EXPECT_EQ(small[2], std::byte{0});
  EXPECT_EQ(small[3], std::byte{0});
  EXPECT_EQ(small[8], std::byte{0xff});
  EXPECT_EQ(small[0x15], std::byte{0x01});
  EXPECT_EQ(small[0x16], std::byte{0x02});
  EXPECT_EQ(small[0x17], std::byte{0x03});
  EXPECT_EQ(small[0x18], std::byte{0x04});
}

TEST_F(SenkushaProbeCodec, PadsTheLargestOutboundMtuProbeWithChiaki) {
  const auto upper = outbound(1454, 0x7ff, 0xa1b2c3d4U);
  ASSERT_TRUE(upper);
  EXPECT_EQ(upper.value().datagram_size, 1426U);
  const auto& av = upper.value().encoded_av_payload;
  ASSERT_EQ(av.size(), 1425U);
  EXPECT_EQ(av[2], std::byte{0});
  EXPECT_EQ(av[3], std::byte{0});
  EXPECT_EQ(av[8], std::byte{0xff});
  EXPECT_EQ(av[0x15], std::byte{0xa1});
  EXPECT_EQ(av[0x16], std::byte{0xb2});
  EXPECT_EQ(av[0x17], std::byte{0xc3});
  EXPECT_EQ(av[0x18], std::byte{0xd4});
  constexpr std::array padding{std::byte{'C'}, std::byte{'H'}, std::byte{'I'},
                               std::byte{'A'}, std::byte{'K'}, std::byte{'I'}};
  for (std::size_t index = 0x19; index < av.size(); ++index)
    EXPECT_EQ(av[index], padding[(index - 0x19) % padding.size()])
        << "index " << index;
}

TEST_F(SenkushaProbeCodec, SizesAdjacentOutboundMtuProbesOneByteApart) {
  const auto adjacent = outbound(1453, 1, 1);
  ASSERT_TRUE(adjacent);
  EXPECT_EQ(adjacent.value().datagram_size, 1425U);
}

TEST_F(SenkushaProbeCodec, RefusesOutboundMtuProbesItCannotBuild) {
  EXPECT_EQ(error_of(outbound(1454, 0x800, 1)), Error::arithmetic_overflow);
  EXPECT_EQ(error_of(outbound(20, 1, 1)), Error::arithmetic_overflow);
  EXPECT_EQ(error_of(outbound(53, 1, 1)), Error::invalid_configuration);
  EXPECT_EQ(error_of(outbound(1455, 1, 1)), Error::invalid_configuration);
  EXPECT_EQ(error_of(outbound(1454, 1, 1, 20)), Error::invalid_configuration);
}

TEST_F(SenkushaProbeCodec, CountsThePsnEnvelopeInTheOutboundMtuProbeSize) {
  const auto non_plain = outbound(1454, 1, 1, 7, takion::PsnEnvelope{1});
  ASSERT_TRUE(non_plain);
  EXPECT_EQ(non_plain.value().datagram_size, 1454U - 28U + 4U);
}

}  // namespace
}  // namespace senkusha_test
