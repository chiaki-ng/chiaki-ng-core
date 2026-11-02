#include "src/core/psn/auth/credentials.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace users_test {
namespace {

namespace users = libchiaking::users;
namespace psn = libchiaking::psn;
using libchiaking::AccountId;
using users::CredentialKind;
using users::HeldCredential;
using users::PsUser;
using users::SignInState;
using users::Unanswered;
using State = HeldCredential::State;
using Reply = Psn::Reply;

class CredentialDates : public ::testing::Test {
 protected:
  using Time = std::chrono::system_clock::time_point;

  static auto signed_in_at() -> Time { return Psn::at_ms(Psn::wall_start_ms); }

  static auto seed_expiry() -> Time {
    return Psn::at_ms(Psn::wall_start_ms + 5'184'000'000);
  }

  static auto renewed_at() -> Time {
    return Psn::at_ms(Psn::wall_start_ms + 86'400'000);
  }

  static auto fact(const std::shared_ptr<PsUser>& user, CredentialKind kind)
      -> std::optional<HeldCredential> {
    const auto facts = user->held_credentials();
    const auto found = std::ranges::find(facts, kind, &HeldCredential::kind);
    if (found == facts.end()) return std::nullopt;
    return *found;
  }

  static auto state_of(const std::shared_ptr<PsUser>& user, CredentialKind kind)
      -> std::optional<State> {
    const auto found = fact(user, kind);
    if (!found) return std::nullopt;
    return found->state;
  }

  auto signs_in_with_dates() {
    rig.fake.on("GET", std::string{Psn::remote_authorize_url},
                {Psn::redirect("https://remoteplay.dl.playstation.net/"
                               "remoteplay/redirect?code=remote%2Bcode")});
    rig.fake.on(
        "POST", std::string{Psn::token_url},
        {Reply{
             200,
             R"({"access_token":"remote-access","refresh_token":"old-remote","expires_in":3600,"refresh_token_expires_in":5184000,"token_type":"bearer"})"},
         Reply{
             200,
             R"({"access_token":"mobile-access","refresh_token":"old-mobile","expires_in":7200,"refresh_token_expires_in":86400})"}});
    rig.fake.on(
        "GET", std::string{Psn::account_url},
        {Reply{
            200,
            R"({"user_id":"578437695752307201","online_id":"Tokyo Player"})"}});
    return rig.signed_in.users.sign_in(psn::auth::Npsso{"synthetic-npsso"},
                                       Psn::own_device(), Psn::correlation, {},
                                       seed_expiry());
  }

  void renews_a_day_later() {
    rig.fake.seen.clear();
    rig.wall.advance_ms(86'400'000);
    rig.fake.rules.clear();
    rig.fake.on(
        "POST", std::string{Psn::token_url},
        {Reply{
            200,
            R"({"access_token":"new-mobile","refresh_token":"next-mobile","expires_in":2000,"refresh_token_expires_in":1000})"}});
    rig.fake.on(
        "POST", std::string{Psn::legacy_token_url},
        {Reply{
            200,
            R"({"access_token":"new-remote","refresh_token":"next-remote","expires_in":1000,"token_type":"Bearer"})"}});
  }

  SignedInRig rig;
};

TEST_F(CredentialDates, AreRecordedAtSignInSavedAndReported) {
  const auto signed_in = signs_in_with_dates();
  ASSERT_TRUE(signed_in);
  EXPECT_EQ(rig.fake.seen.size(), 4U);
  EXPECT_TRUE(rig.fake.unmatched.empty());
  const auto& user = signed_in.value();
  const auto kept = rig.signed_in.credentials.saved(user->account_id());
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept->npsso_obtained_at, signed_in_at());
  EXPECT_EQ(kept->npsso_expires_at, seed_expiry());
  ASSERT_TRUE(kept->store);
  EXPECT_EQ(kept->store->obtained_at, signed_in_at());
  ASSERT_TRUE(kept->store_refresh);
  EXPECT_EQ(kept->store_refresh->obtained_at, signed_in_at());
  EXPECT_EQ(kept->store_refresh->expires_at,
            Psn::at_ms(Psn::wall_start_ms + 86'400'000));
  ASSERT_TRUE(kept->remote_play);
  EXPECT_EQ(kept->remote_play->obtained_at, signed_in_at());
  ASSERT_TRUE(kept->remote_play_refresh);
  EXPECT_EQ(kept->remote_play_refresh->obtained_at, signed_in_at());
  EXPECT_EQ(kept->remote_play_refresh->expires_at, seed_expiry());

  const auto seed = fact(user, CredentialKind::sign_in_seed);
  ASSERT_TRUE(seed);
  EXPECT_EQ(seed->state, State::unverified);
  EXPECT_EQ(seed->obtained_at, signed_in_at());
  EXPECT_EQ(seed->expires_at, seed_expiry());
  const auto app = fact(user, CredentialKind::app_token);
  ASSERT_TRUE(app);
  EXPECT_EQ(app->state, State::usable);
  EXPECT_EQ(app->obtained_at, signed_in_at());
  EXPECT_EQ(app->expires_at, Psn::at_ms(Psn::wall_start_ms + 7'200'000));
  const auto remote = fact(user, CredentialKind::remote_play_token);
  ASSERT_TRUE(remote);
  EXPECT_EQ(remote->state, State::usable);
  EXPECT_EQ(remote->obtained_at, signed_in_at());
  const auto app_refresh = fact(user, CredentialKind::app_refresh_token);
  ASSERT_TRUE(app_refresh);
  EXPECT_EQ(app_refresh->state, State::unverified);
  EXPECT_EQ(app_refresh->obtained_at, signed_in_at());
  EXPECT_EQ(app_refresh->expires_at,
            Psn::at_ms(Psn::wall_start_ms + 86'400'000));
  const auto remote_refresh =
      fact(user, CredentialKind::remote_play_refresh_token);
  ASSERT_TRUE(remote_refresh);
  EXPECT_EQ(remote_refresh->state, State::unverified);
  EXPECT_EQ(remote_refresh->obtained_at, signed_in_at());
  EXPECT_EQ(remote_refresh->expires_at, seed_expiry());
}

TEST_F(CredentialDates, ExpireTheAppRefreshTokenWithoutAsking) {
  const auto signed_in = signs_in_with_dates();
  ASSERT_TRUE(signed_in);
  const auto& user = signed_in.value();
  rig.fake.seen.clear();
  rig.wall.advance_ms(86'400'000);
  EXPECT_EQ(state_of(user, CredentialKind::app_refresh_token), State::expired);
  EXPECT_EQ(state_of(user, CredentialKind::remote_play_refresh_token),
            State::unverified);
  EXPECT_EQ(state_of(user, CredentialKind::sign_in_seed), State::unverified);
  EXPECT_TRUE(rig.fake.seen.empty());
}

TEST_F(CredentialDates, AreRenewedWhileTheSeedKeepsItsOwn) {
  const auto signed_in = signs_in_with_dates();
  ASSERT_TRUE(signed_in);
  const auto& user = signed_in.value();
  renews_a_day_later();
  const auto renewed = user->stay_signed_in();
  ASSERT_TRUE(renewed);
  EXPECT_EQ(renewed.value(), SignInState::signed_in);
  ASSERT_EQ(rig.fake.seen.size(), 2U);
  EXPECT_TRUE(Psn::exchanges_npsso(rig.fake.seen[0]));
  EXPECT_EQ(rig.fake.seen[1].body, Psn::remote_refresh_body);
  const auto kept = rig.signed_in.credentials.saved(user->account_id());
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept->npsso_obtained_at, signed_in_at());
  EXPECT_EQ(kept->npsso_expires_at, seed_expiry());
  ASSERT_TRUE(kept->store);
  EXPECT_EQ(kept->store->obtained_at, renewed_at());
  ASSERT_TRUE(kept->store_refresh);
  EXPECT_EQ(kept->store_refresh->token.view(), "next-mobile");
  EXPECT_EQ(kept->store_refresh->obtained_at, renewed_at());
  EXPECT_EQ(kept->store_refresh->expires_at,
            Psn::at_ms(Psn::wall_start_ms + 86'400'000 + 1'000'000));
  ASSERT_TRUE(kept->remote_play);
  EXPECT_EQ(kept->remote_play->obtained_at, renewed_at());
  ASSERT_TRUE(kept->remote_play_refresh);
  EXPECT_EQ(kept->remote_play_refresh->token.view(), "next-remote");
  EXPECT_EQ(kept->remote_play_refresh->obtained_at, renewed_at());
  EXPECT_FALSE(kept->remote_play_refresh->expires_at);
  EXPECT_EQ(state_of(user, CredentialKind::app_refresh_token),
            State::unverified);
  const auto app = fact(user, CredentialKind::app_token);
  ASSERT_TRUE(app);
  EXPECT_EQ(app->obtained_at, renewed_at());
  const auto unknown = fact(user, CredentialKind::remote_play_refresh_token);
  ASSERT_TRUE(unknown);
  EXPECT_EQ(unknown->state, State::unverified);
  EXPECT_EQ(unknown->obtained_at, renewed_at());
  EXPECT_FALSE(unknown->expires_at);

  rig.fake.seen.clear();
  rig.wall.advance_ms(5'184'000'000);
  EXPECT_EQ(state_of(user, CredentialKind::sign_in_seed), State::expired);
  EXPECT_EQ(state_of(user, CredentialKind::app_refresh_token), State::expired);
  EXPECT_EQ(state_of(user, CredentialKind::remote_play_refresh_token),
            State::unverified);
  EXPECT_TRUE(rig.fake.seen.empty());
}

TEST_F(CredentialDates, SurviveARenewalPsAnswersWithoutARefreshToken) {
  auto saved = rig.saved(9, "NPSSO-A", "old-access");
  saved.remote_play_refresh.emplace(
      psn::auth::RemotePlayRefreshToken{"old-remote"}, signed_in_at(),
      seed_expiry());
  const auto holder = rig.signed_in.as(std::move(saved));
  rig.fake.on(
      "POST", std::string{Psn::legacy_token_url},
      {Reply{
          200,
          R"({"access_token":"new-remote","expires_in":1000,"token_type":"Bearer"})"}});
  const auto unrenewed = holder->stay_signed_in();
  ASSERT_FALSE(unrenewed);
  EXPECT_EQ(unrenewed.error().reason, Unanswered::Reason::malformed);
  const auto untouched =
      fact(holder, CredentialKind::remote_play_refresh_token);
  ASSERT_TRUE(untouched);
  EXPECT_TRUE(untouched->held);
  EXPECT_EQ(untouched->obtained_at, signed_in_at());
  EXPECT_EQ(untouched->expires_at, seed_expiry());
}

TEST_F(CredentialDates, OfAPastedSeedHaveNoExpiry) {
  rig.answers_a_sign_in_as_tokyo_player();
  const auto without = rig.signed_in.users.sign_in(
      psn::auth::Npsso{"synthetic-npsso"}, Psn::own_device(), Psn::correlation);
  ASSERT_TRUE(without);
  const auto bare =
      rig.signed_in.credentials.saved(without.value()->account_id());
  ASSERT_TRUE(bare);
  EXPECT_EQ(bare->npsso_obtained_at, signed_in_at());
  EXPECT_FALSE(bare->npsso_expires_at);
  ASSERT_TRUE(bare->store_refresh);
  EXPECT_EQ(bare->store_refresh->obtained_at, signed_in_at());
  EXPECT_FALSE(bare->store_refresh->expires_at);
  ASSERT_TRUE(bare->remote_play_refresh);
  EXPECT_FALSE(bare->remote_play_refresh->expires_at);
  const auto pasted_seed = fact(without.value(), CredentialKind::sign_in_seed);
  ASSERT_TRUE(pasted_seed);
  EXPECT_EQ(pasted_seed->obtained_at, signed_in_at());
  EXPECT_FALSE(pasted_seed->expires_at);
}

}  // namespace
}  // namespace users_test
