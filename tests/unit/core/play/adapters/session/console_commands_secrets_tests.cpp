#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/console_commands_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <type_traits>

namespace play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace command = remote_play::command;
using libchiaking::AccountId;

static_assert(!std::is_copy_constructible_v<command::KeyboardSetText>);
static_assert(!std::is_copy_constructible_v<command::CouchPasscode>);
static_assert(!std::is_copy_constructible_v<remote_play::StreamCommand>);
static_assert(std::is_same_v<decltype(command::KeyboardSetText::text),
                             libchiaking::SecretString>);
static_assert(std::is_same_v<decltype(command::CouchPasscode::passcode),
                             libchiaking::play::AccessCode>);
static_assert(std::is_same_v<decltype(command::CouchSetAccountId::account_id),
                             AccountId>);
static_assert(AccountId::base64_limit == 23);

TEST(ConsoleSecretCommands, CopyTypedTextOnlyThroughClone) {
  const command::KeyboardSetText typed{libchiaking::SecretString{"hunter2"}};
  EXPECT_EQ(typed.clone().text.view(), "hunter2");
}

TEST(ConsoleSecretCommands, CopyAPadPasscodeOnlyThroughClone) {
  const command::CouchPasscode entered{2, ConsoleRig::code("0427")};
  const auto copy = entered.clone();
  EXPECT_EQ(copy.pad, 2);
  EXPECT_EQ(copy.passcode.text(), "0427");
}

TEST(ConsoleAccountIds, DecodeEightBytesOfBase64) {
  const auto account = AccountId::from_base64("AQIDBAUGBwg=");
  ASSERT_TRUE(account);
  EXPECT_EQ(account->value(), 0x0807060504030201ULL);
}

TEST(ConsoleAccountIds, RefuseEmptyMalformedMisSizedAndOversizedText) {
  EXPECT_FALSE(AccountId::from_base64(""));
  EXPECT_FALSE(AccountId::from_base64("not-an-account"));
  EXPECT_FALSE(AccountId::from_base64("AQIDBAUGBw=="));
  EXPECT_FALSE(AccountId::from_base64("AQIDBAUGBwgJ"));
  EXPECT_FALSE(AccountId::from_base64(std::string{"AQIDBAUGBwg=\0", 13}));
  EXPECT_FALSE(
      AccountId::from_base64(std::string(AccountId::base64_limit + 1, 'A')));
}

}  // namespace
}  // namespace play_test
