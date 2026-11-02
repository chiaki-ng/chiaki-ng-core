#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/adapters/shared_session/shared_session_core_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>

namespace kakusan_test::session_core {
namespace {

class SharedSessionCoreCodes : public SharedSessionCoreTest {};
class SharedSessionCoreReplies : public SharedSessionCoreTest {};
class SharedSessionCoreInvitation : public SharedSessionCoreTest {};

TEST_F(SharedSessionCoreCodes, AWrongCodeConsumesTheInvitationAndCoolsDown) {
  CoreRig rig;
  ASSERT_TRUE(rig.session);
  std::optional<Party> parked;
  for (std::uint8_t attempt = 0; attempt < 3; ++attempt) {
    SCOPED_TRACE(attempt);
    if (attempt == 2) {
      ASSERT_NO_FATAL_FAILURE(rig.invite_guest(0x4f));
      parked = rig.party;
      ASSERT_NO_FATAL_FAILURE(rig.reply(*parked));
      ASSERT_NO_FATAL_FAILURE(rig.verify(*parked));
    }
    ASSERT_NO_FATAL_FAILURE(
        rig.invite_guest(static_cast<std::uint8_t>(0x40 + attempt)));
    const auto party = rig.party;
    ASSERT_NO_FATAL_FAILURE(rig.reply(party));
    ASSERT_NO_FATAL_FAILURE(rig.verify(party));
    ASSERT_NO_FATAL_FAILURE(rig.exchange_codes(party, false));
    ASSERT_TRUE(rig.run_until(
        [&] { return rig.host_state(party.id) == Progress::gone; }));
    ASSERT_TRUE(rig.last_state(party.id));
    EXPECT_EQ(rig.last_state(party.id)->error, Error::pairing_failed);
    EXPECT_EQ(party.guest->error, Error::pairing_failed);
    const auto again =
        rig.session->import_reply(party.guest->reply_block, rig.clock());
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error(), Error::invalid_block);
    EXPECT_EQ(rig.count<CoreRig::Core::JoinRequested>(), 0U);
  }
  ASSERT_TRUE(parked);
  EXPECT_EQ(rig.host_state(parked->id), Progress::verifying);
  const auto cooling =
      rig.session->enter_code(parked->id, text(*parked->guest->code), rig.now);
  ASSERT_FALSE(cooling);
  EXPECT_EQ(cooling.error(), Error::pairing_failed);
  EXPECT_EQ(rig.host_state(parked->id), Progress::verifying);
  ASSERT_NO_FATAL_FAILURE(rig.invite_guest(0x50));
  const auto party = rig.party;
  const auto block = party.guest->answer(*rig.block_for(party.id));
  const auto refused = rig.session->import_reply(block, rig.clock());
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), Error::pairing_failed);
  rig.run(31, 10 * one_second);
  const auto accepted = rig.session->import_reply(block, rig.clock());
  ASSERT_TRUE(accepted);
  EXPECT_EQ(accepted.value(), party.id);
}

TEST_F(SharedSessionCoreReplies, AreCheckedAgainstTheInvitation) {
  CoreRig rig;
  ASSERT_TRUE(rig.session);
  ASSERT_NO_FATAL_FAILURE(rig.invite_guest(0xa1));
  const auto party = rig.party;
  const auto offered = *rig.block_for(party.id);

  EXPECT_FALSE(rig.session->import_reply("RCP1:AAAA", rig.clock()));
  EXPECT_FALSE(rig.session->import_reply(offered, rig.clock()));

  auto foreign =
      ShortBlockCodec::parse_invitation(crypto(), offered).value().body;
  foreign.seed[0] ^= std::byte{0x5a};
  foreign.identity_hint =
      ShortBlockCodec::hint_of(identity(0x77).key().bytes());
  const auto reissued = ShortBlockCodec::seal(crypto(), foreign);
  ASSERT_TRUE(reissued);
  const auto foreign_reply =
      build_reply(std::string{reissued.value().text.view()},
                  party.guest->identity, *party.guest->description, 1);
  ASSERT_TRUE(foreign_reply);
  const auto rejected =
      rig.session->import_reply(foreign_reply->block, rig.clock());
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), Error::invalid_block);
  EXPECT_EQ(rig.count<CoreRig::Core::GuestIdentified>(), 0U);

  ASSERT_NO_FATAL_FAILURE(rig.reply(party));
  const auto duplicate =
      rig.session->import_reply(party.guest->reply_block, rig.clock());
  ASSERT_TRUE(duplicate);
  EXPECT_EQ(duplicate.value(), party.id);
  rig.drain();
  const auto rival = build_reply(offered, party.guest->identity,
                                 *party.guest->description, 0xee);
  ASSERT_TRUE(rival);
  const auto conflicting = rig.session->import_reply(rival->block, rig.clock());
  ASSERT_FALSE(conflicting);
  EXPECT_EQ(conflicting.error(), Error::invalid_block);

  ASSERT_NO_FATAL_FAILURE(rig.verify(party));
  EXPECT_EQ(rig.host_state(party.id), Progress::verifying);
}

TEST_F(SharedSessionCoreInvitation, ExpiresUnusedOnTheSteadyClock) {
  CoreRig rig;
  ASSERT_TRUE(rig.session);
  ASSERT_NO_FATAL_FAILURE(rig.invite_guest(0x91));
  const auto party = rig.party;
  ASSERT_TRUE(
      rig.run_until([&] { return rig.host_state(party.id) == Progress::gone; },
                    100, 10 * one_second));
  ASSERT_TRUE(rig.last_state(party.id));
  EXPECT_EQ(rig.last_state(party.id)->error, Error::expired);
  const auto block = party.guest->answer(*rig.block_for(party.id));
  EXPECT_FALSE(rig.session->import_reply(block, rig.clock()));
  EXPECT_FALSE(rig.session->next_deadline_us());
}

TEST_F(SharedSessionCoreInvitation, ExpiresUnusedOnTheWallClock) {
  CoreRig rig;
  ASSERT_TRUE(rig.session);
  ASSERT_NO_FATAL_FAILURE(rig.invite_guest(0x91));
  const auto expired = rig.party;
  ASSERT_TRUE(rig.run_until(
      [&] { return rig.host_state(expired.id) == Progress::gone; }, 100,
      10 * one_second));
  ASSERT_NO_FATAL_FAILURE(rig.invite_guest(0x92));
  const auto wall_bound = rig.party;
  rig.run(10);
  EXPECT_EQ(rig.host_state(wall_bound.id), Progress::waiting);
  rig.wall_offset = 600;
  rig.step();
  EXPECT_EQ(rig.host_state(wall_bound.id), Progress::gone);
  ASSERT_TRUE(rig.last_state(wall_bound.id));
  EXPECT_EQ(rig.last_state(wall_bound.id)->error, Error::expired);
  EXPECT_FALSE(rig.session->next_deadline_us());
}

}  // namespace
}  // namespace kakusan_test::session_core
