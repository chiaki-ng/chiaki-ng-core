#pragma once

#include "src/core/net/protocol/remote_play/stream/cloud_stream_media.hpp"
#include "src/core/net/protocol/remote_play/stream/public_media_event.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <variant>
#include <vector>

namespace remote_play_test {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;

class CloudStreamMediaRig {
 public:
  static auto video_unit(std::uint16_t index, std::vector<std::byte> data)
      -> takion::AvPacketValues {
    return takion::AvPacketValues{
        takion::PacketType{0x02}, false, 0, 7, index, 2, 0, 1, 0, 3,
        std::move(data)};
  }

  static auto audio_unit(std::uint16_t frame, std::uint16_t index,
                         std::vector<std::byte> data)
      -> takion::AvPacketValues {
    return takion::AvPacketValues{
        takion::PacketType{0x03}, false, 0, frame, index, 2, 0, 5, 0, 0,
        std::move(data)};
  }

  static auto counted_audio_unit(std::uint16_t frame, std::uint16_t index,
                                 std::uint16_t total, std::uint16_t fec,
                                 std::vector<std::byte> data)
      -> takion::AvPacketValues {
    return takion::AvPacketValues{
        takion::PacketType{0x03}, false, 0, frame, index, total, fec, 5, 0, 0,
        std::move(data)};
  }

  static auto video_frame_unit(std::uint16_t packet_index, std::uint16_t frame,
                               std::uint16_t index, std::uint16_t total,
                               std::uint16_t fec, std::vector<std::byte> data)
      -> takion::AvPacketValues {
    auto packet = video_unit(index, std::move(data));
    packet.packet_index = packet_index;
    packet.frame_index = frame;
    packet.units_in_frame_total = total;
    packet.units_in_frame_fec = fec;
    return packet;
  }

  static auto bytes(std::initializer_list<int> values)
      -> std::vector<std::byte> {
    std::vector<std::byte> out;
    for (const auto value : values)
      out.push_back(static_cast<std::byte>(value));
    return out;
  }

  static auto audio_of(const remote_play::AssembledMedia& event)
      -> const takion::client_event::EncodedAudio& {
    return std::get<takion::client_event::EncodedAudio>(event);
  }

  static auto policy_name(remote_play::AudioAssemblyPolicy policy) -> const
      char* {
    return policy == remote_play::AudioAssemblyPolicy::console_units
               ? "console units"
               : "cloud generation";
  }
};

}  // namespace remote_play_test
