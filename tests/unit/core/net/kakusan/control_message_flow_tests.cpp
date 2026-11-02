#include "src/core/net/kakusan/controller_frame.hpp"
#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "tests/support/bytes.hpp"
#include "tests/unit/core/net/kakusan/control_message_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <variant>

namespace net_kakusan_test {
namespace {

using namespace libchiaking::net::kakusan;
using test_support::Bytes;

class ControlMessageFlow : public ControlMessageTest {};

TEST_F(ControlMessageFlow, RoundTripsEveryReferenceStepOnTheWire) {
  const auto document = flow();
  ASSERT_TRUE(document);
  const auto& steps = *Fixtures::member(*document, "steps").array();
  EXPECT_EQ(steps.size(), 18U);

  std::size_t seen = 0;
  Digest256 expected_setup_hash{};
  Digest256 announced_setup_hash{};
  for (const auto& step : steps) {
    const auto canonical =
        Fixtures::member(step, "message").canonical().value();
    SCOPED_TRACE(canonical);
    const auto decoded = Codec::decode(canonical);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(Codec::type_name(decoded.value()),
              Fixtures::string_of(
                  Fixtures::member(Fixtures::member(step, "message"), "type")));
    const auto encoded = Codec::encode_secret(decoded.value());
    ASSERT_TRUE(encoded);
    EXPECT_EQ(encoded.value().view(), canonical);
    seen |= std::size_t{1} << decoded.value().index();

    const auto id = static_cast<std::uint32_t>(
        *Fixtures::member(step, "control_id").integer());
    const auto fragments = TransportFrameCodec::encode_control_fragments(
        id, Hash::bytes_of(encoded.value().view()));
    ASSERT_TRUE(fragments);
    const auto& expected =
        *Fixtures::member(step, "dtls_plaintext_hex").array();
    ASSERT_EQ(fragments.value().size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index)
      EXPECT_EQ(Bytes::hex(fragments.value()[index]),
                Fixtures::string_of(expected[index]));

    if (const auto* setup =
            std::get_if<control::SourceSetup>(&decoded.value())) {
      const auto hash = Codec::setup_hash(crypto, *setup);
      ASSERT_TRUE(hash);
      expected_setup_hash = hash.value();
      EXPECT_EQ(setup->source_protocol, 12);
      EXPECT_EQ(setup->video_fps, 60);
      ASSERT_EQ(setup->video_profiles.size(), 1U);
      EXPECT_EQ(setup->video_profiles[0].width, 1280);
      EXPECT_EQ(setup->video_profiles[0].height, 720);
      EXPECT_EQ(Bytes::hex(setup->receive_key),
                "000102030405060708090a0b0c0d0e0f");
      EXPECT_EQ(Bytes::hex(setup->receive_iv),
                "101112131415161718191a1b1c1d1e1f");
      EXPECT_EQ(Bytes::hex(setup->audio_header),
                "02100000bb80000003c000000001");
    }
    if (const auto* ready = std::get_if<control::SourceReady>(&decoded.value()))
      announced_setup_hash = ready->setup_hash;
    if (const auto* grant =
            std::get_if<control::SeatGranted>(&decoded.value())) {
      EXPECT_EQ(grant->binding, (SeatBinding{3, 7}));
      EXPECT_EQ(grant->capacity, 4);
      EXPECT_EQ(grant->local_seats, 2);
      EXPECT_EQ(grant->allowed_buttons, button_mask);
    }
  }
  EXPECT_EQ(expected_setup_hash, announced_setup_hash);
  EXPECT_NE(expected_setup_hash, Digest256{});
  EXPECT_NE(seen, 0U);
}

}  // namespace
}  // namespace net_kakusan_test
