#pragma once

#include "src/core/net/kakusan/protocol_error.hpp"
#include "src/core/kakusan/adapters/invitation_book.hpp"
#include "src/core/net/kakusan/short_block.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/kakusan/fixtures.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>

namespace kakusan_test {

class InvitationBookTest : public ::testing::Test {
 protected:
  using SealedInvitation = libchiaking::net::kakusan::SealedInvitation;
  using SealedReply = libchiaking::net::kakusan::SealedReply;
  using ShortBlockCodec = libchiaking::net::kakusan::ShortBlockCodec;

  static constexpr std::uint64_t second = libchiaking::microseconds_per_second;
  static constexpr std::uint64_t start_us = 5000 * second;
  static constexpr std::uint64_t created_at = 1791115200;
  static constexpr std::uint64_t expires_at = created_at + 600;

  auto invite(std::uint8_t tag, std::uint64_t created = created_at) const
      -> SealedInvitation {
    auto body = invitation.body.clone();
    body.seed[0] = static_cast<std::byte>(tag);
    body.invite_expires_at =
        created + libchiaking::net::kakusan::invitation_lifetime_seconds;
    return sealed(ShortBlockCodec::seal(crypto, body));
  }

  auto answer(const SealedInvitation& offered, std::uint8_t seed_tag = 0) const
      -> SealedReply {
    auto body = reply.body.clone();
    body.invite_tag = ShortBlockCodec::tag_of(offered.hash);
    body.seed[0] = static_cast<std::byte>(seed_tag);
    return sealed(ShortBlockCodec::seal(crypto, body));
  }

  auto answer_with(const SealedInvitation& offered, auto&& mutate) const
      -> SealedReply {
    auto body = answer(offered).body;
    mutate(body);
    return sealed(ShortBlockCodec::seal(crypto, body));
  }

  static auto id_of(const SealedInvitation& offered)
      -> libchiaking::net::kakusan::InvitationId {
    return offered.body.invitation_id;
  }

  static auto fails_with(const libchiaking::net::kakusan::Result<
                             libchiaking::kakusan::ImportOutcome>& result,
                         libchiaking::net::kakusan::Error error)
      -> ::testing::AssertionResult {
    if (result)
      return ::testing::AssertionFailure() << "the reply was imported";
    if (result.error() != error)
      return ::testing::AssertionFailure()
             << "failed with "
             << libchiaking::net::kakusan::error_name(result.error());
    return ::testing::AssertionSuccess();
  }

  mutable test_support::SoftwareCrypto crypto;
  SealedInvitation invitation{
      ShortBlockCodec::parse_invitation(
          crypto, test_support::kakusan::Fixtures::line("invitation.txt"))
          .value()};
  SealedReply reply{
      ShortBlockCodec::parse_reply(
          crypto, test_support::kakusan::Fixtures::line("reply.txt"))
          .value()};

 private:
  template <class Sealed>
  static auto sealed(libchiaking::net::kakusan::Result<Sealed> made) -> Sealed {
    EXPECT_TRUE(made);
    return std::move(made).value();
  }
};

}  // namespace kakusan_test
