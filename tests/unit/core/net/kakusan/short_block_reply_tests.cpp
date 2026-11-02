#include "src/core/utils/bytes/byte_io.hpp"
#include "tests/unit/core/net/kakusan/short_block_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

namespace net_kakusan_test {
namespace {

using libchiaking::net::kakusan::Hash;
using libchiaking::net::kakusan::ShortBlockCodec;
using test_support::Bytes;

class ShortBlockReply : public ShortBlockTest {};

TEST_F(ShortBlockReply, ParsesTheReferenceBlock) {
  const auto& sample = Fixtures::member(vectors(), "reply");
  const auto& fields = Fixtures::member(sample, "fields");
  const auto& derived = Fixtures::member(sample, "derived");
  const auto parsed = ShortBlockCodec::parse_reply(crypto, block("reply"));
  ASSERT_TRUE(parsed);
  const auto& body = parsed.value().body;
  EXPECT_EQ(Bytes::hex(body.seed), text(fields, "seed"));
  EXPECT_EQ(Bytes::hex(body.invite_tag), text(fields, "invite_tag"));
  EXPECT_EQ(Bytes::hex(body.dtls_sha256), text(fields, "dtls_sha256"));
  EXPECT_EQ(Bytes::hex(body.identity_hint), text(fields, "identity_hint"));
  EXPECT_EQ(Bytes::hex(body.reply_nonce), text(derived, "reply_nonce"));
  EXPECT_EQ(Bytes::hex(parsed.value().hash), text(derived, "block_sha256"));
  expect_candidates(body.ice, fields, derived);
}

TEST_F(ShortBlockReply, SealsBackToTheSameText) {
  const auto parsed = ShortBlockCodec::parse_reply(crypto, block("reply"));
  ASSERT_TRUE(parsed);
  const auto resealed = ShortBlockCodec::seal(crypto, parsed.value().body);
  ASSERT_TRUE(resealed);
  EXPECT_EQ(resealed.value().text.view(), block("reply"));
}

TEST_F(ShortBlockReply, IsNotReadAsAnInvitation) {
  EXPECT_FALSE(ShortBlockCodec::parse_invitation(crypto, block("reply")));
}

class ShortBlockReplyBinding : public ShortBlockTest {};

TEST_F(ShortBlockReplyBinding, AnswersItsOwnInvitation) {
  const auto invitation =
      ShortBlockCodec::parse_invitation(crypto, block("invitation"));
  const auto reply = ShortBlockCodec::parse_reply(crypto, block("reply"));
  ASSERT_TRUE(invitation);
  ASSERT_TRUE(reply);
  EXPECT_TRUE(ShortBlockCodec::answers(invitation.value(), reply.value()));
}

TEST_F(ShortBlockReplyBinding, HashesBothBlocksIntoTheReferenceTranscript) {
  const auto invitation =
      ShortBlockCodec::parse_invitation(crypto, block("invitation"));
  const auto reply = ShortBlockCodec::parse_reply(crypto, block("reply"));
  ASSERT_TRUE(invitation);
  ASSERT_TRUE(reply);
  std::vector<std::byte> input;
  libchiaking::bytes::Appender writer{input};
  writer.text("RCP/1/transcript");
  writer.u8(0);
  writer.bytes(invitation.value().hash);
  writer.bytes(reply.value().hash);
  const auto transcript = Hash::sha256(crypto, input);
  ASSERT_TRUE(transcript);
  EXPECT_EQ(Bytes::hex(transcript.value()), text(vectors(), "transcript"));
}

TEST_F(ShortBlockReplyBinding, DoesNotAnswerAnInvitationWithAnotherTag) {
  const auto invitation =
      ShortBlockCodec::parse_invitation(crypto, block("invitation"));
  const auto reply = ShortBlockCodec::parse_reply(crypto, block("reply"));
  ASSERT_TRUE(invitation);
  ASSERT_TRUE(reply);
  auto stranger = reply.value().body.clone();
  stranger.invite_tag[0] ^= std::byte{1};
  const auto unrelated = ShortBlockCodec::seal(crypto, stranger);
  ASSERT_TRUE(unrelated);
  EXPECT_FALSE(ShortBlockCodec::answers(invitation.value(), unrelated.value()));
}

}  // namespace
}  // namespace net_kakusan_test
