#include "src/core/play/model/start_policy.hpp"
#include "tests/support/enum_printing.hpp"

#include <libchiaking/detail/play/outcome.hpp>

#include <gtest/gtest.h>

#include <initializer_list>
#include <optional>

namespace play_test {
namespace {

namespace play = libchiaking::play;
namespace users = libchiaking::users;
using play::StartPolicy;
using play::StartRefusal;
using play::Where;
using Standing = StartPolicy::Standing;

class StartPolicyRefusal : public ::testing::Test {
 protected:
  static auto kind(Where where, const Standing& standing)
      -> std::optional<StartRefusal::Kind> {
    const auto refused = StartPolicy::refusal(where, standing);
    return refused ? std::optional{refused->kind()} : std::nullopt;
  }
};

TEST_F(StartPolicyRefusal, LetsOneSessionPlayAtATime) {
  const Standing signed_in{.has_psn_account = true,
                           .sign_in = users::SignInState::signed_in};
  for (const auto where : {Where::home, Where::internet, Where::cloud}) {
    SCOPED_TRACE(testing::PrintToString(where));
    EXPECT_FALSE(kind(where, signed_in));
    auto playing = signed_in;
    playing.session_live = true;
    EXPECT_EQ(kind(where, playing), StartRefusal::Kind::busy);
    auto pairing = signed_in;
    pairing.pairing = true;
    EXPECT_EQ(kind(where, pairing), StartRefusal::Kind::busy);
  }
}

TEST_F(StartPolicyRefusal, NeedsASignInForAnythingButHomePlay) {
  const Standing home_user{};
  EXPECT_FALSE(kind(Where::home, home_user));
  EXPECT_EQ(kind(Where::internet, home_user),
            StartRefusal::Kind::needs_psn_sign_in);
  EXPECT_EQ(kind(Where::cloud, home_user),
            StartRefusal::Kind::needs_psn_sign_in);

  Standing lapsed{.has_psn_account = true,
                  .sign_in = users::SignInState::expired};
  EXPECT_FALSE(kind(Where::home, lapsed));
  EXPECT_EQ(kind(Where::internet, lapsed), StartRefusal::Kind::sign_in_expired);
  EXPECT_EQ(kind(Where::cloud, lapsed), StartRefusal::Kind::sign_in_expired);
  lapsed.sign_in = users::SignInState::signed_out;
  EXPECT_EQ(kind(Where::internet, lapsed),
            StartRefusal::Kind::needs_psn_sign_in);
  lapsed.sign_in = users::SignInState::store_access_lapsed;
  EXPECT_FALSE(kind(Where::internet, lapsed));
  EXPECT_FALSE(kind(Where::cloud, lapsed));
  lapsed.session_live = true;
  lapsed.sign_in = users::SignInState::expired;
  EXPECT_EQ(kind(Where::cloud, lapsed), StartRefusal::Kind::busy);
}

}  // namespace
}  // namespace play_test
