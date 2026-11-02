#include "tests/unit/core/net/kakusan/short_block_rig.hpp"

#include <gtest/gtest.h>

#include <vector>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace net_kakusan_test {
namespace {

using libchiaking::net::kakusan::Error;
using libchiaking::net::kakusan::ShortBlockCodec;
using test_support::Bytes;

class ShortBlockInvitation : public ShortBlockTest {};

TEST_F(ShortBlockInvitation, ParsesTheReferenceBlock) {
  const auto& sample = Fixtures::member(vectors(), "invitation");
  const auto& fields = Fixtures::member(sample, "fields");
  const auto& derived = Fixtures::member(sample, "derived");
  const auto& text_block = text(sample, "block");
  const auto parsed = ShortBlockCodec::parse_invitation(crypto, text_block);
  ASSERT_TRUE(parsed);
  const auto& body = parsed.value().body;
  EXPECT_EQ(Bytes::hex(body.seed), text(fields, "seed"));
  EXPECT_EQ(body.invite_expires_at, number(fields, "invite_expires_at"));
  EXPECT_EQ(body.source_protocol, number(fields, "source_protocol"));
  EXPECT_EQ(body.capacity, number(fields, "capacity"));
  EXPECT_EQ(Bytes::hex(body.dtls_sha256), text(fields, "dtls_sha256"));
  EXPECT_EQ(Bytes::hex(body.identity_hint), text(fields, "identity_hint"));
  EXPECT_EQ(Bytes::hex(body.invitation_id), text(derived, "invitation_id"));
  EXPECT_EQ(Bytes::hex(parsed.value().hash), text(derived, "block_sha256"));
  EXPECT_EQ(parsed.value().text.view(), text_block);
  expect_candidates(body.ice, fields, derived);
}

TEST_F(ShortBlockInvitation, SealsBackToTheSameText) {
  const auto parsed =
      ShortBlockCodec::parse_invitation(crypto, block("invitation"));
  ASSERT_TRUE(parsed);
  const auto resealed = ShortBlockCodec::seal(crypto, parsed.value().body);
  ASSERT_TRUE(resealed);
  EXPECT_EQ(resealed.value().text.view(), block("invitation"));
}

TEST_F(ShortBlockInvitation, IsNotReadAsAReply) {
  EXPECT_FALSE(ShortBlockCodec::parse_reply(crypto, block("invitation")));
}

TEST_F(ShortBlockInvitation, RejectsEveryMalformedReferenceBlock) {
  const auto& cases = *Fixtures::member(vectors(), "rejected").array();
  EXPECT_EQ(cases.size(), 14U);
  for (const auto& entry : cases) {
    SCOPED_TRACE(text(entry, "block"));
    const auto parsed =
        ShortBlockCodec::parse_invitation(crypto, text(entry, "block"));
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error(), Error::invalid_block);
  }
}

TEST_F(ShortBlockInvitation, RejectsAnEmptyOrForeignBlock) {
  EXPECT_FALSE(ShortBlockCodec::parse_invitation(crypto, ""));
  EXPECT_FALSE(ShortBlockCodec::parse_invitation(crypto, "RCP1:"));
  EXPECT_FALSE(ShortBlockCodec::parse_invitation(crypto, "RCP2:UVepgduy"));
}

TEST_F(ShortBlockInvitation, AcceptsTextWrappedInWhiteSpace) {
  const auto& original = block("invitation");
  std::string wrapped{" "};
  for (std::size_t index = 0; index < original.size(); ++index) {
    wrapped.push_back(original[index]);
    if (index % 40 == 39) wrapped.append("\r\n\t");
  }
  const auto parsed = ShortBlockCodec::parse_invitation(crypto, wrapped);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value().text.view(), original);
}

TEST_F(ShortBlockInvitation, RefusesToSealCandidatesItCannotShare) {
  const auto parsed =
      ShortBlockCodec::parse_invitation(crypto, block("invitation"));
  ASSERT_TRUE(parsed);
  auto reordered = parsed.value().body.clone();
  const auto shared = reordered.ice.candidates();
  ASSERT_GE(shared.size(), 2U);
  std::vector<libchiaking::net::kakusan::Candidate> swapped{shared.begin(),
                                                           shared.end()};
  std::swap(swapped[0], swapped[1]);
  auto swapped_ice = libchiaking::net::kakusan::IceParameters::make(
      reordered.ice.credentials().clone(), std::move(swapped),
      reordered.ice.port_guesses());
  ASSERT_TRUE(swapped_ice);
  reordered.ice = std::move(swapped_ice.value());
  EXPECT_FALSE(ShortBlockCodec::seal(crypto, reordered));

  auto expired = parsed.value().body.clone();
  expired.invite_expires_at = std::uint64_t{1} << 32U;
  EXPECT_FALSE(ShortBlockCodec::seal(crypto, expired));
}

}  // namespace
}  // namespace net_kakusan_test
