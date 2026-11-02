#pragma once

#include "libchiaking/play_types.hpp"
#include "tests/support/bytes.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace test_support::play {

class SourceStream {
 public:
  using Media = libchiaking::play::SharedDatagram::Media;

  struct Vector {
    Media media{Media::video};
    std::uint64_t key_position{};
    std::string_view datagram;
  };

  static constexpr std::uint32_t takion_version = 12;
  static constexpr std::string_view receive_key =
      "202122232425262728292a2b2c2d2e2f";
  static constexpr std::string_view receive_iv =
      "303132333435363738393a3b3c3d3e3f";
  static constexpr std::string_view audio_header =
      "02100000bb80000003c000000001";
  static constexpr std::uint16_t video_width = 1280;
  static constexpr std::uint16_t video_height = 720;
  static constexpr std::string_view video_header =
      "000000016764002aacd9405005bb0110000003001000000780f18319600000000168ebe3"
      "cb22c0";
  static constexpr std::uint8_t video_fps = 60;

  static constexpr std::array<Vector, 9> vectors{
      Vector{Media::video, 44800ULL,
             "0200000001000000000607f4969c0000af000000008ed31add5370d0a428e08ee"
             "70af7"},
      Vector{Media::audio, 44928ULL,
             "03000000010000080105a5eb78400000af800000e4dd93ee79aff8ec"},
      Vector{Media::audio, 45056ULL,
             "0300010002000008010582fe9d6f0000b00000006a0c1788be4d46f0"},
      Vector{Media::video, 45184ULL,
             "02000100020000000006485f048e0000b080000000e4e6951e5a18caa3b131cd9"
             "59d21"},
      Vector{Media::audio, 45312ULL,
             "030002000300000801058b1982740000b1000000464254c349d0252a"},
      Vector{Media::video, 45440ULL,
             "02000200030000040006ff87e2860000b1800000003b4c91be661d4330"},
      Vector{Media::video, 45568ULL,
             "02000300030020040006bf8158e20000b2000000007cd9813320eba2e7"},
      Vector{Media::audio, 45696ULL,
             "0300030004000008010599679de40000b2800000b759718096ec6815"},
      Vector{Media::video, 45824ULL,
             "0200040004000000000695238f900000b300000000803270453593416d87249a1"
             "c7070"},
  };

  static constexpr std::array<std::string_view, 4> video_frames{
      "000000016588840001112233",
      "000000016588840002112233",
      "000000016588840003112233",
      "000000016588840004112233",
  };

  static constexpr std::array<std::string_view, 4> audio_frames{
      "f8fffe01a0b1c2d3",
      "f8fffe02a0b1c2d3",
      "f8fffe03a0b1c2d3",
      "f8fffe04a0b1c2d3",
  };

  static auto datagram(std::size_t index) -> std::vector<std::byte> {
    return Bytes::unhex(vectors[index].datagram);
  }

  static auto receive_keys() -> libchiaking::play::ReceiveKeys {
    libchiaking::SecretArray<libchiaking::play::ReceiveKeys::size> key;
    libchiaking::SecretArray<libchiaking::play::ReceiveKeys::size> iv;
    std::ranges::copy(
        Bytes::unhex_array<libchiaking::play::ReceiveKeys::size>(receive_key),
        key.begin());
    std::ranges::copy(
        Bytes::unhex_array<libchiaking::play::ReceiveKeys::size>(receive_iv),
        iv.begin());
    return libchiaking::play::ReceiveKeys{std::move(key), std::move(iv)};
  }

  static auto description() -> libchiaking::media::StreamDescription {
    libchiaking::media::StreamDescription description;
    description.audio.channels = 2;
    description.audio.bits = 16;
    description.audio.rate = 48000;
    description.audio.frame_size = 960;
    description.audio.streams = 1;
    description.audio.coupled_streams = 1;
    description.audio.mapping = {0, 1};
    description.audio.positions = {
        libchiaking::media::AudioChannelPosition::front_left,
        libchiaking::media::AudioChannelPosition::front_right};
    description.profiles.push_back(libchiaking::media::VideoProfile{
        video_width, video_height, Bytes::unhex(video_header)});
    description.audio_header = Bytes::unhex_array<
        libchiaking::media::StreamDescription::audio_header_size>(audio_header);
    return description;
  }
};

}  // namespace test_support::play
