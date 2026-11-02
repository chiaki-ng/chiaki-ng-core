#pragma once

#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/platform/factory.hpp"
#include "src/core/kakusan/adapters/shared_session/packet_verdict.hpp"
#include "src/core/kakusan/adapters/shared_setup.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/utils/json/strict.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/fixtures.hpp"
#include "tests/unit/core/kakusan/source_description.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kakusan_test {

class SharedSetupTest : public ::testing::Test {
 protected:
  using SourceSetup = libchiaking::net::kakusan::control::SourceSetup;
  using AudioHeader = libchiaking::net::kakusan::control::AudioHeader;
  using SessionId = libchiaking::net::kakusan::control::SourceSessionId;
  using Refusal = libchiaking::play::ForwardedStream::Refusal;
  using Verdict = libchiaking::kakusan::PacketVerdict;

  struct Generated {
    std::uint64_t key_position;
    std::uint16_t frame;
    std::string_view datagram;
    std::string_view plaintext;
  };

  class FixedClock final : public libchiaking::platform::ClockProvider {
   public:
    std::uint64_t now_microseconds() const noexcept override { return 1000; }
  };

  class HeardAudio final : public libchiaking::media::MediaSink {
   public:
    void on_stream_description(
        const libchiaking::media::StreamDescription&) override {}
    auto on_video(const libchiaking::media::VideoUnit&)
        -> libchiaking::media::VideoResult override {
      return {};
    }
    void on_video_gap() override {}
    auto on_audio(const libchiaking::media::AudioUnit& unit)
        -> libchiaking::media::AudioResult override {
      if (unit.kind == libchiaking::media::AudioUnit::Kind::samples)
        frames.push_back(unit.bytes);
      return {};
    }
    void on_feedback(
        const libchiaking::controllers::ControllerFeedback&) override {}

    std::vector<std::vector<std::byte>> frames;
  };

  class Receiver {
   public:
    [[nodiscard]] auto push(std::uint64_t position,
                            const std::vector<std::byte>& bytes)
        -> libchiaking::Expected<void, Refusal> {
      libchiaking::play::SharedDatagram datagram;
      datagram.media = libchiaking::play::SharedDatagram::Media::audio;
      datagram.key_position = position;
      datagram.bytes = bytes;
      return stream->receive(datagram);
    }

    HeardAudio heard;
    std::unique_ptr<libchiaking::play::ForwardedStream> stream;
  };

  static constexpr std::array<Generated, 6> version_12_frames{
      Generated{44992ULL, 20, "03001e00140000030105194a964d0000afc0000013d880",
                "f8fffe"},
      Generated{45008ULL, 21, "03001f00150000030105d55a75540000afd0000047d22b",
                "010203"},
      Generated{4294967328ULL, 22,
                "03002000160000030105d96a71d40000002000008c4006", "a0b1c2"},
      Generated{90016ULL, 23, "03002100170000030105c282874900015fa00000599313",
                "ffeedd"},
      Generated{135040ULL, 24, "03002200180000030105140462b500020f8000008b7513",
                "102030"},
      Generated{135072ULL, 25, "03002300190000030105c6243fdf00020fa00000646b93",
                "c0ffee"},
  };

  static constexpr std::array<Generated, 4> version_9_frames{
      Generated{44992ULL, 40, "030032002800000301052e20a3470000afc00013d880",
                "f8fffe"},
      Generated{45008ULL, 41, "03003300290000030105d8f6552e0000afd00047d22b",
                "010203"},
      Generated{4294967328ULL, 42,
                "030034002a0000030105f0f4bd0100000020008c4006", "a0b1c2"},
      Generated{90016ULL, 43, "030035002b00000301050886b64200015fa000599313",
                "ffeedd"},
  };

  static constexpr std::array<Generated, 3> two_unit_frames{
      Generated{44992ULL, 60,
                "030046003c000103020527f888740000afc000004a85dd5fbf2f",
                "a1a2a3a4a5a6"},
      Generated{45008ULL, 62, "030047003e0000030105ce1fc2570000afd000009601fa",
                "d0d1d2"},
      Generated{4294967328ULL, 63,
                "030048003f00000301059e857b4b000000200000fc2016", "d0d1d2"},
  };

  static constexpr std::string_view haptics_tag_two =
      "03005000500000030105b25a59cb0000afc0000213d880";

  static constexpr std::array<Generated, 2> after_fixture_frames{
      Generated{45024ULL, 10, "030011000a000003010541e147e60000afe0000004be70",
                "0a0b0c"},
      Generated{45040ULL, 11, "030012000b0000030105ebf07c940000aff000004456a3",
                "1a1b1c"},
  };

  static constexpr std::string_view fixture_audio_header =
      "02100000bb80000003c000000001";
  static constexpr std::string_view high_level_42 =
      "000000016764002aacd9405005bb01";

  static constexpr std::string_view h265_main_header =
      "0000000140010c01ffff016000000300b0000003000003007817024000000001420101"
      "01600000"
      "0300b0"
      "0000030000030078a003c0801107cb8817b916452ffcb9fc4feb016a04040401000000"
      "014401c0"
      "72f05324";
  static constexpr std::string_view h265_main10_header =
      "0000000140010c01ffff022000000300b0000003000003007817024000000001420101"
      "02200000"
      "0300b0"
      "0000030000030078a003c0801107cad8817b916452ffcb9fc4feb016a0404040100000"
      "00014401"
      "c072f0"
      "5324";
  static constexpr std::string_view h264_high_header =
      "0000000127640028ac5680780227e5400000000128ee3cb0";
  static constexpr std::size_t h265_sps_offset = 32;
  static constexpr std::size_t h265_profile_byte = h265_sps_offset + 3;
  static constexpr std::size_t h265_level_byte = h265_sps_offset + 17;

  static auto fixture_setup() -> SourceSetup {
    const auto document = libchiaking::json::strict::Value::parse(
        test_support::kakusan::Fixtures::text("control-flow.json"));
    EXPECT_TRUE(document);
    if (!document) return {};
    for (const auto& step :
         *test_support::kakusan::Fixtures::member(document.value(), "steps")
              .array()) {
      const auto canonical =
          test_support::kakusan::Fixtures::member(step, "message")
              .canonical()
              .value();
      const auto decoded =
          libchiaking::net::kakusan::control::MessageCodec::decode(canonical);
      EXPECT_TRUE(decoded);
      if (!decoded) return {};
      if (const auto* setup = std::get_if<SourceSetup>(&decoded.value()))
        return setup->clone();
    }
    ADD_FAILURE() << "control-flow.json holds no source setup";
    return {};
  }

  static auto fixture_header() -> std::vector<std::byte> {
    return fixture_setup().video_profiles[0].header;
  }

  static auto audio_header_of(std::string_view text) -> AudioHeader {
    return test_support::Bytes::unhex_array<
        libchiaking::net::kakusan::control::audio_header_size>(text);
  }

  static auto audio_header_with(std::uint8_t channels, std::uint8_t bits,
                                std::uint32_t rate, std::uint32_t frame_size,
                                std::uint32_t opaque = 1) -> AudioHeader {
    AudioHeader header{};
    header[0] = static_cast<std::byte>(channels);
    header[1] = static_cast<std::byte>(bits);
    put_u32(header, 2, rate);
    put_u32(header, 6, frame_size);
    put_u32(header, 10, opaque);
    return header;
  }

  static auto ready_info(std::size_t profiles = 1)
      -> libchiaking::media::StreamDescription {
    libchiaking::media::StreamDescription info;
    info.audio.channels = 2;
    info.audio.bits = 16;
    info.audio.rate = 48000;
    info.audio.frame_size = 960;
    for (std::size_t index = 0; index < profiles; ++index)
      info.profiles.push_back(
          libchiaking::media::VideoProfile{1280, 720, fixture_header()});
    info.audio_header = audio_header_of(fixture_audio_header);
    return info;
  }

  static auto h265_info() -> libchiaking::media::StreamDescription {
    auto info = ready_info(2);
    for (auto& profile : info.profiles) {
      profile.width = 1920;
      profile.height = 1080;
      profile.header = test_support::Bytes::unhex(h265_main_header);
    }
    return info;
  }

  static auto source_keys() -> libchiaking::play::ReceiveKeys {
    libchiaking::SecretArray<libchiaking::play::ReceiveKeys::size> key;
    libchiaking::SecretArray<libchiaking::play::ReceiveKeys::size> iv;
    for (std::size_t index = 0; index < key.size(); ++index) {
      key[index] = static_cast<std::byte>(index);
      iv[index] = static_cast<std::byte>(16 + index);
    }
    return libchiaking::play::ReceiveKeys{std::move(key), std::move(iv)};
  }

  static auto session_id() -> SessionId {
    SessionId id{};
    for (std::size_t index = 0; index < id.size(); ++index)
      id[index] = static_cast<std::byte>(0x70 + index);
    return id;
  }

  static auto sps(std::uint8_t profile, std::uint8_t level,
                  bool long_start = true, std::uint8_t constraints = 0)
      -> std::vector<std::byte> {
    std::vector<std::byte> bytes;
    if (long_start) bytes.push_back(std::byte{0});
    bytes.insert(
        bytes.end(),
        {std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0x67},
         static_cast<std::byte>(profile), static_cast<std::byte>(constraints),
         static_cast<std::byte>(level), std::byte{0xac}, std::byte{0xd9}});
    return bytes;
  }

  static auto build(std::uint32_t version,
                    const libchiaking::media::StreamDescription& description,
                    std::uint32_t video_fps,
                    const SessionId& session = session_id())
      -> libchiaking::net::kakusan::Result<SourceSetup> {
    return libchiaking::kakusan::SharedSetup::build(
        libchiaking::kakusan::CapturedKeys{version, source_keys(), 0},
        description, video_fps, session);
  }

  static auto forward(const SourceSetup& setup)
      -> libchiaking::net::kakusan::Result<
          libchiaking::kakusan::SharedSetup::Stream> {
    return libchiaking::kakusan::SharedSetup::stream(setup.clone());
  }

  static auto encoded(const SourceSetup& setup)
      -> libchiaking::net::kakusan::Result<std::string> {
    return libchiaking::net::kakusan::control::MessageCodec::encode_secret(
               libchiaking::net::kakusan::control::Message{setup.clone()})
        .transform([](const libchiaking::SecretString& text) {
          return std::string{text.view()};
        });
  }

  static auto fails_with(const auto& result,
                         libchiaking::net::kakusan::Error error)
      -> ::testing::AssertionResult {
    if (result) return ::testing::AssertionFailure() << "it succeeded";
    if (result.error() != error)
      return ::testing::AssertionFailure()
             << "it failed with "
             << libchiaking::net::kakusan::error_name(result.error());
    return ::testing::AssertionSuccess();
  }

  static auto admit_audio(std::uint32_t version, std::uint64_t position,
                          const std::vector<std::byte>& bytes) -> Verdict {
    return Verdict::classify(
        1, SourceDescriptions::describe(
               version, libchiaking::play::SharedDatagram::Media::audio,
               position, bytes));
  }

  static auto verdict(libchiaking::kakusan::Admission admission) -> Verdict {
    return Verdict{admission, libchiaking::net::kakusan::MediaKind::audio};
  }

  void open_receiver(std::uint8_t protocol = 12) {
    auto setup = fixture_setup();
    setup.source_protocol = protocol;
    auto forwarded =
        libchiaking::kakusan::SharedSetup::stream(std::move(setup));
    ASSERT_TRUE(forwarded);
    receiver_ = std::make_unique<Receiver>();
    auto opened = libchiaking::play::ForwardedStream::open(
        fixed_platform_, std::move(forwarded.value().stream),
        std::move(forwarded.value().keys), receiver_->heard);
    ASSERT_TRUE(opened);
    receiver_->stream = std::move(opened).value();
  }

  auto feed(std::uint32_t version, std::uint64_t position,
            const std::vector<std::byte>& bytes) -> ::testing::AssertionResult {
    if (admit_audio(version, position, bytes) !=
        verdict(libchiaking::kakusan::Admission::forward))
      return ::testing::AssertionFailure()
             << "the datagram at " << position << " was not forwarded";
    const auto pushed = receiver_->push(position, bytes);
    for (auto& frame : receiver_->heard.frames)
      frames_.push_back(std::move(frame));
    receiver_->heard.frames.clear();
    if (!pushed)
      return ::testing::AssertionFailure()
             << "the receiver refused the datagram at " << position;
    return ::testing::AssertionSuccess();
  }

  test_support::SoftwareCrypto crypto_;
  FixedClock fixed_clock_;
  libchiaking::platform::DefaultPlatformFactory fixed_platform_{
      libchiaking::platform::PlatformProviders{.clock = &fixed_clock_,
                                               .crypto = &crypto_}};
  std::unique_ptr<Receiver> receiver_;
  std::vector<std::vector<std::byte>> frames_;

 private:
  static void put_u32(AudioHeader& header, std::size_t offset,
                      std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index)
      header[offset + index] =
          static_cast<std::byte>((value >> (24U - 8U * index)) & 0xffU);
  }
};

}  // namespace kakusan_test
