#pragma once

#include "src/core/net/protocol/remote_play/stream/video_bitstream.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace remote_play_test {

class VideoBits {
 public:
  using Byte = std::byte;

  void u(std::uint32_t value, unsigned count) {
    for (unsigned index = count; index > 0; --index) {
      if (used % 8 == 0) data.push_back(Byte{0});
      if ((value >> (index - 1)) & 1U)
        data.back() |= static_cast<Byte>(1U << (7U - used % 8));
      ++used;
    }
  }
  void ue(std::uint32_t value) {
    unsigned width = 0;
    for (auto n = value + 1; n; n >>= 1U) ++width;
    u(0, width - 1);
    u(value + 1, width);
  }
  void align() {
    while (used % 8) u(0, 1);
  }
  auto nal(bool hevc, unsigned type) -> std::vector<Byte> {
    u(1, 1);
    align();
    std::vector<Byte> out{Byte{0}, Byte{0}, Byte{0}, Byte{1},
                          static_cast<Byte>(hevc ? type << 1U : type | 0x60U)};
    if (hevc) out.push_back(Byte{1});
    unsigned zeros = 0;
    for (const auto byte : data) {
      if (zeros >= 2 && std::to_integer<unsigned>(byte) <= 3) {
        out.push_back(Byte{3});
        zeros = 0;
      }
      out.push_back(byte);
      zeros = byte == Byte{0} ? zeros + 1 : 0;
    }
    out.resize(out.size() + 32);
    return out;
  }

 private:
  std::vector<Byte> data;
  unsigned used{};
};

class VideoBitstreamRig : public ::testing::Test {
 protected:
  using Byte = std::byte;
  static auto header(bool hevc, unsigned frame_bits) -> std::vector<Byte> {
    VideoBits bits;
    if (hevc) {
      bits.u(1, 8);
      bits.u(0, 32);
      bits.u(0, 32);
      bits.u(0, 32);
      bits.ue(0);
      bits.ue(1);
      bits.ue(1920);
      bits.ue(1080);
      bits.u(0, 1);
      bits.ue(2);
      bits.ue(2);
    } else {
      bits.u(100, 8);
      bits.u(0, 8);
      bits.u(40, 8);
      bits.ue(0);
      bits.ue(1);
      bits.ue(0);
      bits.ue(0);
      bits.u(0, 2);
    }
    bits.ue(frame_bits - 4);
    return bits.nal(hevc, hevc ? 33 : 7);
  }
  static auto frame(bool hevc, unsigned frame_bits, unsigned reference,
                    bool intra = false) -> std::vector<Byte> {
    VideoBits bits;
    if (hevc) {
      bits.u(1, 1);
      if (intra) bits.u(0, 1);
      bits.ue(0);
      bits.ue(intra ? 2 : 1);
      if (!intra) {
        bits.u(3, frame_bits);
        bits.u(0, 1);
        bits.ue(16);
        bits.ue(0);
        for (unsigned index = 0; index < 16; ++index) {
          bits.ue(0);
          bits.u(index == reference ? 1 : 0, 1);
        }
      }
    } else {
      bits.ue(0);
      bits.ue(intra ? 2 : 0);
      if (!intra) {
        bits.ue(0);
        bits.u(3, frame_bits);
        bits.u(0, 1);
        bits.u(1, 1);
        bits.ue(0);
        bits.ue(reference);
        bits.ue(3);
      }
    }
    return bits.nal(hevc, intra ? (hevc ? 20 : 5) : 1);
  }
  static auto sliced_header(unsigned frame_bits, bool dependent)
      -> std::vector<Byte> {
    VideoBits sps;
    sps.u(1, 8);
    sps.u(0, 32);
    sps.u(0, 32);
    sps.u(0, 32);
    sps.ue(0);
    sps.ue(1);
    sps.ue(1280);
    sps.ue(720);
    sps.u(0, 1);
    sps.ue(2);
    sps.ue(2);
    sps.ue(frame_bits - 4);
    sps.u(1, 1);
    sps.ue(0);
    sps.ue(0);
    sps.ue(0);
    sps.ue(0);
    sps.ue(3);
    auto bytes = sps.nal(true, 33);
    VideoBits pps;
    pps.ue(0);
    pps.ue(0);
    pps.u(dependent ? 1 : 0, 1);
    pps.u(0, 1);
    pps.u(0, 3);
    const auto tail = pps.nal(true, 34);
    bytes.insert(bytes.end(), tail.begin(), tail.end());
    return bytes;
  }
  static auto slice_unit(unsigned frame_bits, unsigned reference,
                         unsigned address, bool dependent_enabled,
                         bool dependent = false, bool sps_set = false)
      -> std::vector<Byte> {
    VideoBits bits;
    bits.u(address == 0 ? 1 : 0, 1);
    bits.ue(0);
    if (address != 0) {
      if (dependent_enabled) bits.u(dependent ? 1 : 0, 1);
      bits.u(address, 8);
    }
    if (!dependent) {
      bits.ue(1);
      bits.u(3, frame_bits);
      bits.u(sps_set ? 1 : 0, 1);
      if (!sps_set) {
        bits.ue(16);
        bits.ue(0);
        for (unsigned index = 0; index < 16; ++index) {
          bits.ue(0);
          bits.u(index == reference ? 1 : 0, 1);
        }
      }
    }
    bits.u(0x5a, 8);
    return bits.nal(true, 1);
  }
};

}  // namespace remote_play_test
