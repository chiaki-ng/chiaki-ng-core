#include "libchiaking/platform/crypto/openssl/provider.hpp"
#include "src/core/net/protocol/remote_play/stream/forwarded_source.hpp"
#include "src/core/utils/text/hex.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/process_threading.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace remote_play_test {
namespace {

namespace platform = libchiaking::platform;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;
using remote_play::ForwardedSourceError;
using remote_play::ForwardedSourceReceiver;
using remote_play::ForwardedSourceSetup;

class ForwardedSource : public ::testing::Test {
 protected:
  struct Vector final {
    std::uint64_t key_position;
    std::uint16_t frame;
    std::string_view datagram;
    std::string_view plaintext;
  };

  static constexpr std::array<Vector, 4> vectors{
      Vector{44992ULL, 9, "03000700090000030105c28df95d0000afc0000013d880",
             "f8fffe"},
      Vector{45008ULL, 10, "030008000a000003010542e5ccf40000afd0000047d22b",
             "010203"},
      Vector{4294967328ULL, 11,
             "030009000b0000030105586404830000002000008c4006", "a0b1c2"},
      Vector{90016ULL, 12, "03000a000c00000301057f4cd4ce00015fa00000599313",
             "ffeedd"},
  };

  static auto unhex(std::string_view text) -> std::vector<std::byte> {
    std::vector<std::byte> bytes(text.size() / 2);
    EXPECT_TRUE(libchiaking::text::decode_hex(text, bytes)) << text;
    return bytes;
  }

  static auto setup(std::uint32_t version = 12, std::byte difference = {})
      -> ForwardedSourceSetup {
    libchiaking::SecretArray<takion::source_key_size> key;
    libchiaking::SecretArray<takion::source_key_size> iv;
    for (std::size_t index = 0; index < 16; ++index) {
      key[index] = static_cast<std::byte>(index);
      iv[index] = static_cast<std::byte>(16 + index);
    }
    key[0] ^= difference;
    ForwardedSourceSetup value{
        version,
        takion::SourceReceiveKeys{std::move(key), std::move(iv)},
        {},
        {},
        false};
    value.profiles.push_back(remote_play::event::VideoProfile{
        1280, 720,
        unhex("000000016764002aacd9405005bb0110000003001000000780f18319600000"
              "000168ebe3cb22c0")});
    const auto header = unhex("02100000bb80000003c000000001");
    std::copy(header.begin(), header.end(), value.audio_header.begin());
    return value;
  }

  auto receiver(ForwardedSourceSetup value = setup()) {
    return ForwardedSourceReceiver::create(crypto, threading, std::move(value));
  }

  auto attempt(const std::vector<std::byte>& datagram, std::uint64_t position,
               takion::SourceMedia media = takion::SourceMedia::audio) {
    auto fresh = receiver();
    EXPECT_TRUE(fresh);
    const auto pushed = fresh.value()->push(media, position, datagram, 1000);
    EXPECT_FALSE(fresh.value()->take());
    return pushed;
  }

  static auto fails_with(const auto& result, ForwardedSourceError error)
      -> testing::AssertionResult {
    if (result)
      return testing::AssertionFailure() << "the datagram was accepted";
    if (result.error() != error)
      return testing::AssertionFailure()
             << "failed with " << testing::PrintToString(result.error());
    return testing::AssertionSuccess();
  }

  platform::openssl::OpenSslCryptoProvider crypto;
  platform::DefaultThreadingProvider& threading =
      test_support::process_threading();
};

TEST_F(ForwardedSource, PublishesTheValidatedStreamInfo) {
  auto created = receiver();
  ASSERT_TRUE(created);
  const auto info = created.value()->stream_info();
  EXPECT_EQ(info.audio.channels, 2U);
  EXPECT_EQ(info.audio.bits, 16U);
  EXPECT_EQ(info.audio.rate, 48000U);
  EXPECT_EQ(info.audio.frame_size, 960U);
  ASSERT_EQ(info.profiles.size(), 1U);
  EXPECT_EQ(info.profiles[0].width, 1280U);
  EXPECT_EQ(info.audio_header, setup().audio_header);
}

TEST_F(ForwardedSource, AcceptsVersionsNineToNineteen) {
  EXPECT_TRUE(receiver(setup(9)));
  EXPECT_TRUE(receiver(setup(19)));
}

TEST_F(ForwardedSource, RefusesUnsupportedVersions) {
  const auto legacy = receiver(setup(7));
  ASSERT_FALSE(legacy);
  EXPECT_EQ(legacy.error(), ForwardedSourceError::unsupported_version);
  const auto unknown = receiver(setup(4096));
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error(), ForwardedSourceError::unsupported_version);
}

TEST_F(ForwardedSource, RefusesASetupWithoutProfiles) {
  auto no_profiles = setup();
  no_profiles.profiles.clear();
  const auto empty = receiver(std::move(no_profiles));
  ASSERT_FALSE(empty);
  EXPECT_EQ(empty.error(), ForwardedSourceError::invalid_setup);
}

TEST_F(ForwardedSource, RefusesAProfileWithoutAVideoHeader) {
  auto no_header = setup();
  no_header.profiles[0].video_header.clear();
  const auto headerless = receiver(std::move(no_header));
  ASSERT_FALSE(headerless);
  EXPECT_EQ(headerless.error(), ForwardedSourceError::invalid_setup);
}

TEST_F(ForwardedSource, AuthenticatesAndDecryptsTheSourceVectors) {
  auto created = receiver();
  ASSERT_TRUE(created);
  std::vector<std::vector<std::byte>> frames;
  for (const auto& vector : vectors) {
    const auto pushed =
        created.value()->push(takion::SourceMedia::audio, vector.key_position,
                              unhex(vector.datagram), 1000);
    EXPECT_TRUE(pushed) << "frame " << vector.frame;
    while (auto media = created.value()->take()) {
      EXPECT_EQ(media->kind, remote_play::EncodedMediaKind::audio);
      frames.push_back(std::move(media->bytes));
    }
  }
  ASSERT_EQ(frames.size(), vectors.size());
  for (std::size_t index = 0; index < vectors.size(); ++index)
    EXPECT_EQ(frames[index], unhex(vectors[index].plaintext))
        << "frame " << vectors[index].frame;
  EXPECT_FALSE(created.value()->take_idr_request());
}

TEST_F(ForwardedSource, DeliversReorderedAndRepeatedVectorsOnceInOrder) {
  auto reordered = receiver();
  ASSERT_TRUE(reordered);
  std::vector<std::vector<std::byte>> frames;
  for (const std::size_t index :
       {std::size_t{2}, std::size_t{0}, std::size_t{1}, std::size_t{1},
        std::size_t{3}}) {
    EXPECT_TRUE(reordered.value()->push(takion::SourceMedia::audio,
                                        vectors[index].key_position,
                                        unhex(vectors[index].datagram), 1000))
        << "vector " << index;
    while (auto media = reordered.value()->take())
      frames.push_back(std::move(media->bytes));
  }
  ASSERT_EQ(frames.size(), vectors.size());
  for (std::size_t index = 0; index < vectors.size(); ++index)
    EXPECT_EQ(frames[index], unhex(vectors[index].plaintext))
        << "frame " << vectors[index].frame;
}

TEST_F(ForwardedSource, AcceptsAnIntactDatagram) {
  EXPECT_TRUE(attempt(unhex(vectors[0].datagram), vectors[0].key_position));
}

TEST_F(ForwardedSource, RefusesMalformedDatagrams) {
  const auto valid = unhex(vectors[0].datagram);
  const auto position = vectors[0].key_position;
  EXPECT_TRUE(fails_with(attempt({}, position),
                         ForwardedSourceError::malformed_datagram));
  EXPECT_TRUE(fails_with(attempt(valid, position, takion::SourceMedia::video),
                         ForwardedSourceError::malformed_datagram));
  EXPECT_TRUE(fails_with(attempt(valid, position + 1),
                         ForwardedSourceError::malformed_datagram));
  auto truncated = valid;
  truncated.resize(12);
  EXPECT_TRUE(fails_with(attempt(truncated, position),
                         ForwardedSourceError::malformed_datagram));
}

TEST_F(ForwardedSource, RefusesADatagramAtTheWrongKeyEpoch) {
  EXPECT_TRUE(
      fails_with(attempt(unhex(vectors[0].datagram),
                         vectors[0].key_position + (std::uint64_t{1} << 32U)),
                 ForwardedSourceError::authentication_failed));
}

TEST_F(ForwardedSource, RefusesATamperedDatagram) {
  const auto valid = unhex(vectors[0].datagram);
  for (const std::size_t offset :
       {std::size_t{1}, std::size_t{9}, std::size_t{10}, std::size_t{13},
        valid.size() - 1}) {
    auto tampered = valid;
    tampered[offset] ^= std::byte{1};
    EXPECT_TRUE(fails_with(attempt(tampered, vectors[0].key_position),
                           ForwardedSourceError::authentication_failed))
        << "offset " << offset;
  }
}

TEST_F(ForwardedSource, RefusesADatagramUnderOtherKeys) {
  auto created = receiver(setup(12, std::byte{1}));
  ASSERT_TRUE(created);
  EXPECT_TRUE(fails_with(
      created.value()->push(takion::SourceMedia::audio, vectors[0].key_position,
                            unhex(vectors[0].datagram), 1000),
      ForwardedSourceError::authentication_failed));
}

TEST_F(ForwardedSource, HasNoDeadlineWhileIdle) {
  auto created = receiver();
  ASSERT_TRUE(created);
  EXPECT_FALSE(created.value()->next_deadline_us());
  EXPECT_TRUE(created.value()->poll(5000));
  EXPECT_FALSE(created.value()->take());
}

}  // namespace
}  // namespace remote_play_test
