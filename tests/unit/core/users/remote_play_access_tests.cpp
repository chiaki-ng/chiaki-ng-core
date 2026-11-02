#include "src/core/users/ports/membership_calls.hpp"
#include "src/core/users/ports/play_calls.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace users_test {
namespace {

using libchiaking::AccountId;
using libchiaking::users::MembershipCalls;
using libchiaking::users::PlayCalls;
using libchiaking::users::PsUser;
using libchiaking::users::RemotePlayAccess;
using libchiaking::users::SignInState;
using Reason = libchiaking::users::Unanswered::Reason;
using Reply = Psn::Reply;

static_assert(!std::is_copy_constructible_v<RemotePlayAccess> &&
              !std::is_copy_assignable_v<RemotePlayAccess>);
static_assert(std::is_move_constructible_v<RemotePlayAccess> &&
              !std::is_default_constructible_v<RemotePlayAccess>);
static_assert(!std::is_constructible_v<RemotePlayAccess, PsUser&,
                                       libchiaking::psn::auth::BearerToken>);
static_assert(!std::is_convertible_v<RemotePlayAccess, std::string> &&
              !std::is_convertible_v<RemotePlayAccess, std::string_view>);

class RemotePlayAccessRig : public SignedInRig {
 public:
  static constexpr std::string_view renewed_tokens =
      R"({"access_token":"new-remote","refresh_token":"next-remote","expires_in":1000,"token_type":"Bearer"})";

  auto bare(std::uint64_t account) -> std::shared_ptr<PsUser> {
    return signed_in.as(saved(account, "NPSSO-A", "app-a"));
  }

  auto remote(std::int64_t expires_in_ms = 3'600'000)
      -> std::shared_ptr<PsUser> {
    auto held = saved(30, "NPSSO-A", "app-a");
    held.remote_play.emplace(
        libchiaking::psn::auth::BearerToken{"remote-access"},
        Psn::at_ms(Psn::wall_start_ms + expires_in_ms));
    held.remote_play_refresh.emplace(
        libchiaking::psn::auth::RemotePlayRefreshToken{"old-remote"});
    return signed_in.as(std::move(held));
  }

  void renews_remote_play(Reply reply) {
    fake.on("POST", std::string{Psn::legacy_token_url}, {std::move(reply)});
  }

  void renews_remote_play() {
    renews_remote_play(Reply{200, std::string{renewed_tokens}});
  }
};

class RemotePlayAccessLease : public ::testing::Test {
 protected:
  RemotePlayAccessRig rig;
};

TEST_F(RemotePlayAccessLease, IsLeasedFromTheSignIn) {
  const auto user = rig.remote();
  auto lease = PlayCalls{*user}.remote_play_access();
  ASSERT_TRUE(lease);
  EXPECT_EQ(lease.value().bearer().view(), "remote-access");
  EXPECT_TRUE(rig.fake.seen.empty());
  const auto again = PlayCalls{*user}.remote_play_access();
  ASSERT_TRUE(again);
  EXPECT_EQ(again.value().bearer().view(), "remote-access");
  EXPECT_TRUE(rig.fake.seen.empty());
  const RemotePlayAccess moved{std::move(lease.value())};
  EXPECT_EQ(moved.bearer().view(), "remote-access");
  EXPECT_EQ(user->snapshot_json().value().find("remote-access"),
            std::string::npos);
}

TEST_F(RemotePlayAccessLease, IsRenewedOnceWhenItHasLapsed) {
  const auto user = rig.remote(30'000);
  rig.renews_remote_play();
  const auto saves = rig.signed_in.credentials.saves;
  const auto lease = PlayCalls{*user}.remote_play_access();
  ASSERT_TRUE(lease);
  EXPECT_EQ(lease.value().bearer().view(), "new-remote");
  ASSERT_EQ(rig.fake.seen.size(), 1U);
  EXPECT_EQ(rig.fake.seen[0].url, Psn::legacy_token_url);
  EXPECT_EQ(rig.fake.seen[0].body, Psn::remote_refresh_body);
  EXPECT_EQ(rig.signed_in.credentials.saves, saves + 1);
  const auto stored = rig.signed_in.credentials.saved(AccountId{30});
  ASSERT_TRUE(stored);
  ASSERT_TRUE(stored->remote_play);
  EXPECT_EQ(stored->remote_play->token.view(), "new-remote");
  ASSERT_TRUE(stored->remote_play_refresh);
  EXPECT_EQ(stored->remote_play_refresh->token.view(), "next-remote");
  const auto next = PlayCalls{*user}.remote_play_access();
  ASSERT_TRUE(next);
  EXPECT_EQ(next.value().bearer().view(), "new-remote");
  EXPECT_EQ(rig.fake.seen.size(), 1U);
}

TEST_F(RemotePlayAccessLease, IsRenewedForTheNextLeaseOncePsRefusesIt) {
  const auto user = rig.remote();
  rig.renews_remote_play();
  auto refused = PlayCalls{*user}.remote_play_access();
  ASSERT_TRUE(refused);
  EXPECT_TRUE(rig.fake.seen.empty());
  refused.value().refused_by_service();
  EXPECT_TRUE(rig.fake.seen.empty());
  const auto fresh = PlayCalls{*user}.remote_play_access();
  ASSERT_TRUE(fresh);
  EXPECT_EQ(fresh.value().bearer().view(), "new-remote");
  EXPECT_EQ(rig.fake.seen.size(), 1U);
  refused.value().refused_by_service();
  const auto kept = PlayCalls{*user}.remote_play_access();
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept.value().bearer().view(), "new-remote");
  EXPECT_EQ(rig.fake.seen.size(), 1U);
  EXPECT_EQ(user->sign_in_state(), SignInState::signed_in);
}

TEST_F(RemotePlayAccessLease, IsMissingWithoutARemotePlayToken) {
  const auto without = rig.bare(30);
  const auto missing = PlayCalls{*without}.remote_play_access();
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().reason, Reason::remote_play_access_missing);
  EXPECT_TRUE(rig.fake.seen.empty());
}

TEST_F(RemotePlayAccessLease, IsMissingOnceARenewalIsRefusedAndNotAskedAgain) {
  const auto unwelcome = rig.remote(30'000);
  rig.renews_remote_play(Reply{400, R"({"error":"invalid_grant"})"});
  const auto nothing = PlayCalls{*unwelcome}.remote_play_access();
  ASSERT_FALSE(nothing);
  EXPECT_EQ(nothing.error().reason, Reason::remote_play_access_missing);
  EXPECT_EQ(nothing.error().http_status, 400);
  EXPECT_EQ(rig.fake.seen.size(), 1U);
  EXPECT_EQ(unwelcome->sign_in_state(), SignInState::signed_in);
  const auto still = PlayCalls{*unwelcome}.remote_play_access();
  ASSERT_FALSE(still);
  EXPECT_EQ(still.error().reason, Reason::remote_play_access_missing);
  EXPECT_EQ(rig.fake.seen.size(), 1U);
}

TEST_F(RemotePlayAccessLease, IsUnavailableWhenTheRenewalCannotBeReached) {
  const auto waiting = rig.remote(30'000);
  rig.renews_remote_play(Reply::transport());
  const auto unreachable = PlayCalls{*waiting}.remote_play_access();
  ASSERT_FALSE(unreachable);
  EXPECT_EQ(unreachable.error().reason, Reason::unavailable);
}

TEST_F(RemotePlayAccessLease, IsCancelledByAFiredStopToken) {
  const auto leaving = rig.remote(30'000);
  const auto stopped = std::make_shared<test_support::StopState>();
  stopped->stopped = true;
  const auto cancelled = PlayCalls{*leaving}.remote_play_access(
      libchiaking::platform::StopToken{stopped});
  ASSERT_FALSE(cancelled);
  EXPECT_EQ(cancelled.error().reason, Reason::cancelled);
}

TEST_F(RemotePlayAccessLease, IsNeverLeasedToAHomeUser) {
  const auto added =
      rig.signed_in.users.add_home_user(AccountId{40}, "Living Room Player");
  ASSERT_TRUE(added);
  PlayCalls calls{*added.value()};
  const auto lease = calls.remote_play_access();
  ASSERT_FALSE(lease);
  EXPECT_EQ(lease.error().reason, Reason::signed_out);
  EXPECT_EQ(lease.error().http_status, 0);
  calls.membership_rejected_by_service();
  EXPECT_FALSE(added.value()->membership().tier());
  EXPECT_FALSE(MembershipCalls{*added.value()}.remembered_membership());
  EXPECT_TRUE(rig.fake.seen.empty());
  EXPECT_EQ(added.value()->sign_in_state(), SignInState::signed_out);
}

}  // namespace
}  // namespace users_test
