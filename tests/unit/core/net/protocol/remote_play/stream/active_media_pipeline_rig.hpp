#pragma once

#include "src/core/net/protocol/remote_play/stream/active_session.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace remote_play_test {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;

class ActiveMediaBitWriter {
 public:
  void u(std::uint32_t value, unsigned count) {
    for (unsigned index = count; index > 0; --index) {
      if (used_ % 8 == 0) data_.push_back(std::byte{0});
      if ((value >> (index - 1)) & 1U)
        data_.back() |= static_cast<std::byte>(1U << (7U - used_ % 8));
      ++used_;
    }
  }

  void ue(std::uint32_t value) {
    unsigned width = 0;
    for (auto n = value + 1; n; n >>= 1U) ++width;
    u(0, width - 1);
    u(value + 1, width);
  }

  void align() {
    while (used_ % 8) u(0, 1);
  }

  auto nal(bool hevc, unsigned type) -> std::vector<std::byte> {
    u(1, 1);
    align();
    std::vector<std::byte> out{
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1},
        static_cast<std::byte>(hevc ? type << 1U : type | 0x60U)};
    if (hevc) out.push_back(std::byte{1});
    unsigned zeros = 0;
    for (const auto byte : data_) {
      if (zeros >= 2 && std::to_integer<unsigned>(byte) <= 3) {
        out.push_back(std::byte{3});
        zeros = 0;
      }
      out.push_back(byte);
      zeros = byte == std::byte{0} ? zeros + 1 : 0;
    }
    out.resize(out.size() + 32);
    return out;
  }

 private:
  std::vector<std::byte> data_;
  unsigned used_{};
};

class ActiveMediaRecordingSink final : public remote_play::ActiveMediaSink {
 public:
  using Ranges = std::vector<std::pair<std::uint16_t, std::uint16_t>>;
  using FecFailures = std::vector<std::pair<std::uint16_t, bool>>;

  auto post_corrupt_frame(std::uint16_t first, std::uint16_t last)
      -> libchiaking::Expected<void, remote_play::ActiveSessionError> override {
    corrupt.emplace_back(first, last);
    return {};
  }

  auto post_idr_after_loss()
      -> libchiaking::Expected<bool, remote_play::ActiveSessionError> override {
    ++idr;
    return idr_accepted;
  }

  void on_video_fec_failure(std::uint16_t frame_index,
                            bool idr_request_sent) override {
    fec_failures.emplace_back(frame_index, idr_request_sent);
  }

  Ranges corrupt;
  FecFailures fec_failures;
  unsigned idr{};
  bool idr_accepted{true};
};

class ActiveMediaRig {
 public:
  using Ranges = ActiveMediaRecordingSink::Ranges;

  static auto header(bool hevc) -> std::vector<std::byte> {
    ActiveMediaBitWriter bits;
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
    bits.ue(4);
    return bits.nal(hevc, hevc ? 33 : 7);
  }

  static auto slice(bool hevc, bool intra, unsigned reference = 0)
      -> std::vector<std::byte> {
    ActiveMediaBitWriter bits;
    if (hevc) {
      bits.u(1, 1);
      if (intra) bits.u(0, 1);
      bits.ue(0);
      bits.ue(intra ? 2 : 1);
      if (!intra) {
        bits.u(3, 8);
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
        bits.u(3, 8);
        bits.u(0, 1);
        bits.u(1, 1);
        bits.ue(0);
        bits.ue(reference);
        bits.ue(3);
      }
    }
    return bits.nal(hevc, intra ? (hevc ? 20 : 5) : 1);
  }

  static auto unit(std::uint16_t frame, std::vector<std::byte> payload,
                   std::uint8_t asi = 0) -> takion::AvPacketValues {
    std::vector<std::byte> data{std::byte{0}, std::byte{0}};
    data.insert(data.end(), payload.begin(), payload.end());
    return takion::AvPacketValues{
        takion::PacketType{0x02}, false, 0, frame, 0, 1, 0, 0, 0, asi,
        std::move(data)};
  }

  static auto split_unit(std::uint16_t frame,
                         const std::vector<std::byte>& payload,
                         std::uint16_t index) -> takion::AvPacketValues {
    const auto half = static_cast<std::ptrdiff_t>(payload.size() / 2);
    std::vector<std::byte> data{std::byte{0}, std::byte{0}};
    if (index == 0)
      data.insert(data.end(), payload.begin(), payload.begin() + half);
    else
      data.insert(data.end(), payload.begin() + half, payload.end());
    return takion::AvPacketValues{
        takion::PacketType{0x02}, false, 0, frame, index, 2, 0, 0, 0, 0,
        std::move(data)};
  }

  static auto parity(std::uint16_t frame) -> takion::AvPacketValues {
    return takion::AvPacketValues{
        takion::PacketType{0x02},
        false,
        0,
        frame,
        2,
        3,
        1,
        0,
        0,
        0,
        {std::byte{0}, std::byte{0}, std::byte{0x8e}, std::byte{0}}};
  }

  static auto take_video(remote_play::ActiveMediaPipeline& media)
      -> std::optional<takion::client_event::EncodedVideo> {
    while (auto event = media.take())
      if (auto* video =
              std::get_if<takion::client_event::EncodedVideo>(&*event))
        if (!video->profile_header) return std::move(*video);
    return std::nullopt;
  }

  static auto take_any_video(remote_play::ActiveMediaPipeline& media)
      -> std::optional<takion::client_event::EncodedVideo> {
    while (auto event = media.take())
      if (auto* video =
              std::get_if<takion::client_event::EncodedVideo>(&*event))
        return std::move(*video);
    return std::nullopt;
  }

  static auto take_audio(remote_play::ActiveMediaPipeline& media)
      -> std::vector<takion::client_event::EncodedAudio> {
    std::vector<takion::client_event::EncodedAudio> result;
    while (auto event = media.take())
      if (auto* audio =
              std::get_if<takion::client_event::EncodedAudio>(&*event))
        result.push_back(std::move(*audio));
    return result;
  }

  static auto delivered(remote_play::ActiveMediaPipeline& media, bool hevc,
                        std::uint16_t last) -> testing::AssertionResult {
    if (!media.consume(unit(1, slice(hevc, true)), 0))
      return testing::AssertionFailure() << "frame 1 was refused";
    for (std::uint16_t frame = 2; frame <= last; ++frame)
      if (!media.consume(unit(frame, slice(hevc, false)), 0))
        return testing::AssertionFailure()
               << "frame " << frame << " was refused";
    for (std::uint16_t frame = 1; frame <= last; ++frame) {
      const auto video = take_video(media);
      if (!video)
        return testing::AssertionFailure()
               << "frame " << frame << " was not delivered";
      if (video->frame_index != frame)
        return testing::AssertionFailure() << "frame " << video->frame_index
                                           << " came instead of " << frame;
    }
    return testing::AssertionSuccess();
  }

  static auto audio_header(std::uint8_t channels)
      -> remote_play::AudioHeaderBytes {
    return remote_play::AudioHeaderBytes{
        std::byte{channels}, std::byte{0x10}, std::byte{0}, std::byte{0},
        std::byte{0xbb},     std::byte{0x80}, std::byte{0}, std::byte{0},
        std::byte{0x01},     std::byte{0xe0}, std::byte{0}, std::byte{0},
        std::byte{0},        std::byte{1}};
  }

  static auto audio_unit(std::uint16_t frame, std::uint8_t tag,
                         std::uint8_t profile, std::byte payload)
      -> takion::AvPacketValues {
    takion::AvPacketValues values;
    values.type = takion::PacketType{0x03};
    values.frame_index = frame;
    values.units_in_frame_total = 1;
    values.units_in_frame_fec = 0x0101;
    values.codec = 5;
    values.data = {payload};
    values.av_tag = tag;
    values.audio_profile = profile;
    return values;
  }

  static auto bytes_of(const remote_play::AudioHeaderBytes& header)
      -> std::vector<std::byte> {
    return {header.begin(), header.end()};
  }

  static auto codec_name(const testing::TestParamInfo<bool>& info)
      -> std::string {
    return info.param ? "Hevc" : "Avc";
  }
};

template <class Base>
class ActiveMediaHarness : public Base {
 protected:
  auto pipeline(bool hevc, std::vector<std::byte> profile,
                bool idr_on_fec_failure = true)
      -> remote_play::ActiveMediaPipeline {
    remote_play::ActiveMediaPipeline media{
        sink,
        remote_play::ActiveMediaOptions{
            remote_play::AudioAssemblyPolicy::console_units, false, hevc,
            idr_on_fec_failure},
        diagnostics};
    remote_play::ValidatedStreamInfo info;
    info.resolutions.push_back(
        takion::StreamInfoResolution{1920, 1080, std::move(profile)});
    media.on_stream_info(info);
    return media;
  }

  auto two_profiles(bool hevc) -> remote_play::ActiveMediaPipeline {
    remote_play::ActiveMediaPipeline media{
        sink,
        remote_play::ActiveMediaOptions{
            remote_play::AudioAssemblyPolicy::console_units, false, hevc, true},
        diagnostics};
    remote_play::ValidatedStreamInfo info;
    info.resolutions.push_back(
        takion::StreamInfoResolution{1280, 720, ActiveMediaRig::header(hevc)});
    auto second = ActiveMediaRig::header(hevc);
    second.push_back(std::byte{0x5a});
    info.resolutions.push_back(
        takion::StreamInfoResolution{1920, 1080, std::move(second)});
    media.on_stream_info(info);
    return media;
  }

  auto audio_pipeline(std::uint32_t version)
      -> remote_play::ActiveMediaPipeline {
    remote_play::ActiveMediaPipeline media{
        sink,
        remote_play::ActiveMediaOptions{
            remote_play::AudioAssemblyPolicy::console_units, false, false,
            false},
        diagnostics};
    remote_play::ValidatedStreamInfo info;
    info.resolutions.push_back(
        takion::StreamInfoResolution{1920, 1080, {std::byte{1}}});
    info.audio_header = ActiveMediaRig::audio_header(2);
    remote_play::AudioChannelSetup main;
    main.header = ActiveMediaRig::audio_header(2);
    main.profiles[6] = ActiveMediaRig::audio_header(6);
    info.audio_channels.push_back(main);
    media.on_stream_info(
        info, takion::resolve_takion_wire_profile(version)->features());
    return media;
  }

  ActiveMediaRecordingSink sink;
  remote_play::StreamDiagnostics diagnostics;
};

}  // namespace remote_play_test
