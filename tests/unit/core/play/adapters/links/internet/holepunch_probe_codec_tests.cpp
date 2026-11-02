#include "src/core/net/nat/holepunch/holepunch_probe.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace play_test {
namespace {

namespace nat = libchiaking::net::nat::holepunch;
namespace pf = libchiaking::platform;
using nat::ProbeCodec;

class HolepunchProbeCodec : public ::testing::Test {
 protected:
  static auto byte(unsigned value) -> std::byte {
    return static_cast<std::byte>(value);
  }

  static auto full_probe() -> nat::CandidateProbe {
    nat::CandidateProbe probe;
    for (std::size_t i = 0; i < 20; ++i) {
      probe.local_hashed_id[i] = byte(0xA0 + static_cast<unsigned>(i));
      probe.console_hashed_id[i] = byte(0x10 + static_cast<unsigned>(i));
    }
    probe.local_sid = 0x1234;
    probe.console_sid = 0xBEEF;
    for (std::size_t i = 0; i < 5; ++i)
      probe.request_id[i] = byte(0xF0 + static_cast<unsigned>(i));
    return probe;
  }

  static auto short_probe() -> nat::CandidateProbe {
    nat::CandidateProbe probe;
    probe.local_sid = 0x1234;
    probe.console_sid = 0xBEEF;
    probe.request_id = {byte(0xF0), byte(0xF1), byte(0xF2), byte(0xF3),
                        byte(0xF4)};
    return probe;
  }

  template <std::size_t Size>
  static void expect_header(const std::array<std::byte, Size>& packet,
                            const nat::CandidateProbe& probe, unsigned type) {
    EXPECT_EQ(packet[0], byte(type));
    for (std::size_t i = 1; i < 4; ++i) EXPECT_EQ(packet[i], byte(0)) << i;
    for (std::size_t i = 0; i < 20; ++i) {
      EXPECT_EQ(packet[0x04 + i], probe.local_hashed_id[i]) << i;
      EXPECT_EQ(packet[0x24 + i], probe.console_hashed_id[i]) << i;
    }
    for (std::size_t i = 0x18; i < 0x24; ++i)
      EXPECT_EQ(packet[i], byte(0)) << i;
    for (std::size_t i = 0x38; i < 0x44; ++i)
      EXPECT_EQ(packet[i], byte(0)) << i;
    EXPECT_EQ(packet[0x44], byte(0x12));
    EXPECT_EQ(packet[0x45], byte(0x34));
    EXPECT_EQ(packet[0x46], byte(0xBE));
    EXPECT_EQ(packet[0x47], byte(0xEF));
    for (std::size_t i = 0x48; i < 0x4B; ++i)
      EXPECT_EQ(packet[i], byte(0)) << i;
    for (std::size_t i = 0; i < 5; ++i)
      EXPECT_EQ(packet[0x4B + i], probe.request_id[i]) << i;
  }

  template <std::size_t Size>
  static void expect_ipv4_tail(const std::array<std::byte, Size>& packet) {
    EXPECT_EQ(packet[0x50], byte(0xD2));
    EXPECT_EQ(packet[0x51], byte(0x34));
    EXPECT_EQ(packet[0x52], byte(0xBC));
    EXPECT_EQ(packet[0x53], byte(0xE6));
    EXPECT_EQ(packet[0x54], byte(0x44));
    EXPECT_EQ(packet[0x55], byte(0x4C));
  }

  template <std::size_t Size>
  static void expect_ipv6_tail(const std::array<std::byte, Size>& packet) {
    EXPECT_EQ(packet[0x50], byte(0x32));
    EXPECT_EQ(packet[0x51], byte(0x35));
    EXPECT_EQ(packet[0x52], byte(0xB3));
    EXPECT_EQ(packet[0x53], byte(0x57));
    EXPECT_EQ(packet[0x54], byte(0xB9));
    EXPECT_EQ(packet[0x55], byte(0xF9));
  }
};

TEST_F(HolepunchProbeCodec, EncodesARequestByteForByte) {
  const auto probe = full_probe();
  const auto packet = ProbeCodec::encode(probe);
  expect_header(packet, probe, 0x06);
  for (std::size_t i = 0x50; i < 88; ++i) EXPECT_EQ(packet[i], byte(0)) << i;
}

TEST_F(HolepunchProbeCodec, DecodesWhatItEncodes) {
  const auto probe = full_probe();
  const auto decoded = ProbeCodec::decode(ProbeCodec::encode(probe));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded.value().local_hashed_id, probe.local_hashed_id);
  EXPECT_EQ(decoded.value().console_hashed_id, probe.console_hashed_id);
  EXPECT_EQ(decoded.value().local_sid, probe.local_sid);
  EXPECT_EQ(decoded.value().console_sid, probe.console_sid);
  EXPECT_EQ(decoded.value().request_id, probe.request_id);
}

TEST_F(HolepunchProbeCodec, EncodesAResponseWithTheObfuscatedIpv4Peer) {
  const auto probe = full_probe();
  const pf::SocketEndpoint peer{pf::IpAddress::ipv4({0xC0, 0x00, 0x02, 0x09}),
                                0x5678};
  const auto packet = ProbeCodec::encode_response(probe, peer);
  expect_header(packet, probe, 0x07);
  expect_ipv4_tail(packet);
  for (std::size_t i = 0x56; i < 88; ++i) EXPECT_EQ(packet[i], byte(0)) << i;
}

TEST_F(HolepunchProbeCodec, EncodesTheFirstFourBytesOfAnIpv6Peer) {
  nat::CandidateProbe probe;
  probe.local_sid = 0x1234;
  probe.console_sid = 0xBEEF;
  probe.request_id = {byte(1), byte(2), byte(3), byte(4), byte(5)};
  const pf::SocketEndpoint peer{
      pf::IpAddress::ipv6(
          {0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF}),
      0xABCD};
  expect_ipv6_tail(ProbeCodec::encode_response(probe, peer));
}

TEST_F(HolepunchProbeCodec, RefusesAPacketOfTheWrongSize) {
  using Error = nat::ProbeCodecError;
  const auto empty = ProbeCodec::decode(std::span<const std::byte>{});
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error(), Error::bad_size);
  const std::array<std::byte, 87> short_packet{};
  const auto short_result = ProbeCodec::decode(short_packet);
  ASSERT_FALSE(short_result);
  EXPECT_EQ(short_result.error(), Error::bad_size);
  const std::array<std::byte, 89> long_packet{};
  const auto long_result = ProbeCodec::decode(long_packet);
  ASSERT_FALSE(long_result);
  EXPECT_EQ(long_result.error(), Error::bad_size);
}

TEST_F(HolepunchProbeCodec, RefusesAnotherTypeButIgnoresReservedBytes) {
  nat::CandidateProbe probe;
  probe.local_sid = 0;
  probe.console_sid = 0xFFFF;
  auto packet = ProbeCodec::encode(probe);
  packet[0] = byte(0x05);
  const auto wrong = ProbeCodec::decode(packet);
  ASSERT_FALSE(wrong);
  EXPECT_EQ(wrong.error(), nat::ProbeCodecError::bad_type);
  packet[0] = byte(0x06);
  packet[0x48] = byte(0x99);
  packet[0x50] = byte(0xAB);
  const auto decoded = ProbeCodec::decode(packet);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded.value().local_sid, 0);
  EXPECT_EQ(decoded.value().console_sid, 0xFFFF);
}

TEST_F(HolepunchProbeCodec, ClassifiesEachKindOfDatagram) {
  using Kind = nat::ProbeDatagramKind;
  const std::array<std::byte, 5> request_id = {
      byte(0xF0), byte(0xF1), byte(0xF2), byte(0xF3), byte(0xF4)};
  nat::CandidateProbe probe;
  probe.request_id = request_id;
  const auto request = ProbeCodec::encode(probe);
  EXPECT_EQ(ProbeCodec::classify(request, request_id), Kind::incoming_request);
  auto response = request;
  response[0] = byte(0x07);
  EXPECT_EQ(ProbeCodec::classify(response, request_id),
            Kind::matching_response);
  auto stale = response;
  stale[0x4D] = byte(0x00);
  EXPECT_EQ(ProbeCodec::classify(stale, request_id), Kind::stale_response_id);
  EXPECT_EQ(ProbeCodec::classify(std::span<const std::byte>{}, request_id),
            Kind::invalid_size);
  const std::array<std::byte, 87> short_packet{};
  EXPECT_EQ(ProbeCodec::classify(short_packet, request_id), Kind::invalid_size);
  const std::array<std::byte, 89> long_packet{};
  EXPECT_EQ(ProbeCodec::classify(long_packet, request_id), Kind::invalid_size);
  auto bad_type = response;
  bad_type[0] = byte(0x08);
  EXPECT_EQ(ProbeCodec::classify(bad_type, request_id), Kind::invalid_type);
  bad_type[0] = byte(0x06);
  bad_type[1] = byte(0x01);
  EXPECT_EQ(ProbeCodec::classify(bad_type, request_id), Kind::invalid_type);
}

TEST_F(HolepunchProbeCodec, EncodesAResponseFromThePeerEndpoint) {
  const auto probe = short_probe();
  expect_ipv4_tail(ProbeCodec::encode_response(
      probe, {pf::IpAddress::ipv4({192, 0, 2, 9}), 0x5678}));
  const std::array<std::uint8_t, 16> v6_raw = {
      0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
  expect_ipv6_tail(ProbeCodec::encode_response(
      probe, {pf::IpAddress::ipv6(v6_raw), 0xABCD}));
}

}  // namespace
}  // namespace play_test
