#include "src/core/net/kakusan/transport_frame.hpp"
#include "tests/unit/core/net/kakusan/control_message_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace net_kakusan_test {
namespace {

using namespace libchiaking::net::kakusan;

static_assert(!std::is_copy_constructible_v<control::SourceKey>);
static_assert(!std::is_copy_constructible_v<control::SourceSetup>);
static_assert(!std::is_copy_constructible_v<control::Message>);

class ControlSourceSetup : public ControlMessageTest {
 protected:
  void SetUp() override {
    const auto document = flow();
    ASSERT_TRUE(document);
    bool found = false;
    for (const auto& step : *Fixtures::member(*document, "steps").array()) {
      const auto decoded =
          Codec::decode(Fixtures::member(step, "message").canonical().value());
      ASSERT_TRUE(decoded);
      if (const auto* source =
              std::get_if<control::SourceSetup>(&decoded.value())) {
        setup = source->clone();
        found = true;
        break;
      }
    }
    ASSERT_TRUE(found);
    const auto encoded = Codec::encode_secret(Message{setup.clone()});
    ASSERT_TRUE(encoded);
    text = std::string{encoded.value().view()};
  }

  auto replaced(std::string_view from, std::string_view to) const
      -> std::string {
    auto changed = text;
    const auto position = changed.find(from);
    if (position == std::string::npos) {
      ADD_FAILURE() << from << " is not in the setup";
      return changed;
    }
    changed.replace(position, from.size(), to);
    return changed;
  }

  static auto encodes(const control::SourceSetup& candidate) -> bool {
    return static_cast<bool>(Codec::encode_secret(Message{candidate.clone()}));
  }

  static auto round_trips(const control::SourceSetup& candidate) -> bool {
    const auto encoded = Codec::encode_secret(Message{candidate.clone()});
    return encoded && Codec::decode(encoded.value().view());
  }

  control::SourceSetup setup;
  std::string text;
};

TEST_F(ControlSourceSetup, DecodesItsOwnEncoding) {
  EXPECT_TRUE(Codec::decode(text));
}

TEST_F(ControlSourceSetup, RejectsAnUnsupportedVideoFormatAsACodecError) {
  EXPECT_TRUE(rejects(
      replaced("\"source_codec\":\"h264\"", "\"source_codec\":\"hevc\""),
      Error::codec));
  EXPECT_TRUE(
      rejects(replaced("\"video_fps\":60", "\"video_fps\":61"), Error::codec));
  EXPECT_TRUE(
      rejects(replaced("\"video_fps\":60", "\"video_fps\":0"), Error::codec));
  EXPECT_TRUE(
      rejects(replaced("\"width\":1280", "\"width\":1921"), Error::codec));
  EXPECT_TRUE(
      rejects(replaced("\"height\":720", "\"height\":1081"), Error::codec));
  EXPECT_TRUE(
      rejects(replaced("\"height\":720", "\"height\":0"), Error::codec));
}

TEST_F(ControlSourceSetup, RejectsAnotherEpochOrAnUnknownSourceProtocol) {
  EXPECT_TRUE(rejects(replaced("\"epoch\":1", "\"epoch\":2")));
  EXPECT_TRUE(
      rejects(replaced("\"source_protocol\":12", "\"source_protocol\":7")));
  EXPECT_TRUE(
      rejects(replaced("\"source_protocol\":12", "\"source_protocol\":21")));
}

TEST_F(ControlSourceSetup, AcceptsEveryKnownSourceProtocol) {
  for (const std::string_view version :
       {"9", "10", "11", "13", "14", "15", "16", "19", "20"}) {
    SCOPED_TRACE(version);
    std::string field{"\"source_protocol\":"};
    field.append(version);
    EXPECT_TRUE(Codec::decode(replaced("\"source_protocol\":12", field)));
  }
}

TEST_F(ControlSourceSetup, RejectsAnotherKeyPolicyAMissingEpochOrExtraKeys) {
  EXPECT_TRUE(rejects(
      replaced("shared-source-incoming-until-session-end-v1", "other-v1")));
  EXPECT_TRUE(rejects(replaced("\"epoch\":1,", "")));
  EXPECT_TRUE(rejects(
      replaced("\"epoch\":1,", "\"epoch\":1,\"master_secret\":\"AAAA\",")));
  EXPECT_TRUE(rejects(replaced("\"receive_key\":\"AAECAwQFBgcICQoLDA0ODw\"",
                               "\"receive_key\":\"AAECAwQFBgcICQoLDA0O\"")));
}

TEST_F(ControlSourceSetup, CarriesOneToEightVideoProfiles) {
  ASSERT_FALSE(setup.video_profiles.empty());
  auto empty_profiles = setup.clone();
  empty_profiles.video_profiles.clear();
  EXPECT_FALSE(encodes(empty_profiles));
  auto many_profiles = setup.clone();
  many_profiles.video_profiles.assign(8, setup.video_profiles[0]);
  EXPECT_TRUE(round_trips(many_profiles));
  many_profiles.video_profiles.push_back(setup.video_profiles[0]);
  EXPECT_FALSE(encodes(many_profiles));
}

TEST_F(ControlSourceSetup, CarriesAProfileHeaderOfOneTo512Bytes) {
  ASSERT_FALSE(setup.video_profiles.empty());
  auto empty_header = setup.clone();
  empty_header.video_profiles[0].header.clear();
  EXPECT_FALSE(encodes(empty_header));
  auto large_header = setup.clone();
  large_header.video_profiles[0].header.assign(512, std::byte{1});
  EXPECT_TRUE(round_trips(large_header));
  large_header.video_profiles[0].header.push_back(std::byte{1});
  EXPECT_FALSE(encodes(large_header));
}

TEST_F(ControlSourceSetup, FitsTheLargestSetupInOneControlMessage) {
  ASSERT_FALSE(setup.video_profiles.empty());
  auto profile = setup.video_profiles[0];
  profile.header.assign(512, std::byte{1});
  profile.width = 1920;
  profile.height = 1080;
  auto largest = setup.clone();
  largest.video_profiles.assign(8, profile);
  const auto encoded = Codec::encode_secret(Message{largest.clone()});
  ASSERT_TRUE(encoded);
  EXPECT_LE(encoded.value().size(), max_control_message_size);
}

TEST_F(ControlSourceSetup, IsWrittenOnlyIntoWipingText) {
  const auto plain = Codec::encode(Message{setup.clone()});
  ASSERT_FALSE(plain);
  EXPECT_EQ(plain.error(), Error::internal);
  EXPECT_TRUE(Codec::encode_secret(Message{setup.clone()}));
}

TEST_F(ControlSourceSetup, HashesEveryKeyByte) {
  const auto first = Codec::setup_hash(crypto, setup);
  ASSERT_TRUE(first);
  auto altered = setup.clone();
  altered.receive_iv[0] ^= std::byte{1};
  const auto second = Codec::setup_hash(crypto, altered);
  ASSERT_TRUE(second);
  EXPECT_NE(second.value(), first.value());
}

TEST_F(ControlSourceSetup, ClonesAndMovesItsKeysAndClearsTheMovedFrom) {
  auto secret = setup.clone();
  EXPECT_EQ(secret.receive_key, setup.receive_key);
  EXPECT_EQ(secret.receive_iv, setup.receive_iv);
  const auto taken = std::move(secret);
  EXPECT_EQ(taken.receive_key, setup.receive_key);
  EXPECT_EQ(taken.receive_iv, setup.receive_iv);
  EXPECT_EQ(secret.receive_key, control::SourceKey{});
  EXPECT_EQ(secret.receive_iv, control::SourceKey{});
}

}  // namespace
}  // namespace net_kakusan_test
