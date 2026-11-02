#include "src/core/net/transport/http/client.hpp"
#include "src/core/users/adapters/ps_user_core.hpp"
#include "src/core/users/ports/catalog_calls.hpp"
#include "src/core/users/ports/play_calls.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/psn/fakes.hpp"
#include "tests/unit/core/users/rig.hpp"

#include <libchiaking/users.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace users_test {
namespace {

using libchiaking::AccountId;
using libchiaking::users::CatalogCalls;
using libchiaking::users::CredentialKind;
using libchiaking::users::CredentialUse;
using libchiaking::users::HeldCredential;
using libchiaking::users::NotRevealed;
using libchiaking::users::PlayCalls;
using libchiaking::users::PsUser;
using libchiaking::users::SavedSignIn;
using Kept = HeldCredential::Kept;
using State = HeldCredential::State;
using Reply = test_support::psn::Reply;

static_assert(!std::is_copy_constructible_v<libchiaking::SecretString>);
static_assert(std::is_same_v<
              decltype(std::declval<const PsUser&>().reveal(
                  CredentialKind::app_token)),
              libchiaking::Expected<libchiaking::SecretString, NotRevealed>>);

class HeldCredentials : public ::testing::Test {
 protected:
  static constexpr std::string_view seed_secret = "Qz7seedWvKx1LmPd";
  static constexpr std::string_view app_secret = "Hy4appTknRb8JcXe";
  static constexpr std::string_view app_refresh_secret = "Uf2rfshGmZn6YtAo";
  static constexpr std::string_view remote_secret = "Bw9rmteCsDq3ViKj";
  static constexpr std::string_view remote_refresh_secret = "Ex5rrfhNpLg0OuMz";
  static constexpr std::string_view library_secret = "Ja1lbryTxFs7WqHc";
  static constexpr std::string_view checkout_secret = "Kd6chktRyBv2ZnPe";
  static constexpr std::array every_secret{
      seed_secret,           app_secret,     app_refresh_secret, remote_secret,
      remote_refresh_secret, library_secret, checkout_secret};

  static auto at_ms(std::int64_t milliseconds) {
    return Psn::at_ms(milliseconds);
  }

  static auto with_the_seed_only(SignedInRig& rig) -> std::shared_ptr<PsUser> {
    SavedSignIn saved{AccountId{5}, "player",
                      libchiaking::psn::auth::Npsso{seed_secret}};
    saved.device = libchiaking::users::DeviceId::parse(Psn::duid);
    return rig.signed_in.as(std::move(saved));
  }

  static auto with_everything_saved(SignedInRig& rig)
      -> std::shared_ptr<PsUser> {
    SavedSignIn saved{AccountId{5}, "player",
                      libchiaking::psn::auth::Npsso{seed_secret}};
    saved.device = libchiaking::users::DeviceId::parse(Psn::duid);
    saved.store.emplace(libchiaking::psn::auth::AppToken{app_secret},
                        at_ms(Psn::wall_start_ms + 3'600'000));
    saved.store_refresh.emplace(
        libchiaking::psn::auth::AppRefreshToken{app_refresh_secret});
    saved.remote_play.emplace(
        libchiaking::psn::auth::BearerToken{remote_secret},
        at_ms(Psn::wall_start_ms + 7'200'000));
    saved.remote_play_refresh.emplace(
        libchiaking::psn::auth::RemotePlayRefreshToken{remote_refresh_secret});
    return rig.signed_in.as(std::move(saved));
  }

  static auto fact(const std::vector<HeldCredential>& facts,
                   CredentialKind kind) -> HeldCredential {
    const auto found = std::ranges::find(facts, kind, &HeldCredential::kind);
    if (found == facts.end()) {
      ADD_FAILURE() << "no fact for credential "
                    << static_cast<int>(std::to_underlying(kind));
      return HeldCredential{.kind = kind};
    }
    return *found;
  }

  static auto shown(const std::vector<HeldCredential>& facts) -> std::string {
    std::string text;
    for (const auto& one : facts) {
      text += std::to_string(static_cast<int>(one.kind)) + "|" +
              std::to_string(one.held) + "|" +
              std::to_string(static_cast<int>(one.kept)) + "|" +
              std::to_string(static_cast<int>(one.state)) + "|" + one.scopes +
              "|";
      if (one.obtained_at)
        text += std::to_string(one.obtained_at->time_since_epoch().count());
      text += "|";
      if (one.expires_at)
        text += std::to_string(one.expires_at->time_since_epoch().count());
      text += "|";
      if (one.last_reply_status) text += std::to_string(*one.last_reply_status);
      for (const auto use : one.used_for)
        text += "," + std::to_string(static_cast<int>(use));
      text += "\n";
    }
    return text;
  }

  static auto carries_no_secret(std::string_view text)
      -> testing::AssertionResult {
    constexpr std::size_t shortest = 4;
    for (const auto secret : every_secret)
      for (std::size_t start = 0; start + shortest <= secret.size(); ++start)
        if (const auto piece = secret.substr(start, shortest);
            text.find(piece) != std::string_view::npos)
          return testing::AssertionFailure()
                 << "\"" << piece << "\" of " << secret << " shows in " << text;
    return testing::AssertionSuccess();
  }

  static auto carries_no_secret(const std::vector<HeldCredential>& facts)
      -> testing::AssertionResult {
    return carries_no_secret(shown(facts));
  }

  static void ask_for_a_library_record(SignedInRig& rig,
                                       const std::shared_ptr<PsUser>& user) {
    libchiaking::net::http::Client http{rig.fake, Runtime::steady(), {}};
    static_cast<void>(CatalogCalls{*user}.library_record(http, "PROD"));
  }

  static void ask_the_old_service_checkout(
      SignedInRig& rig, const std::shared_ptr<PsUser>& user) {
    libchiaking::net::http::Client http{rig.fake, Runtime::steady(), {}};
    auto checkout = PlayCalls{*user}.old_service_checkout(http);
    ASSERT_TRUE(checkout);
    static_cast<void>(checkout.value().account_attributes());
  }

  static auto first_url_asked(auto&& question) -> std::string {
    SignedInRig probe;
    const auto user = with_everything_saved(probe);
    question(probe, user);
    if (probe.fake.seen.empty()) {
      ADD_FAILURE() << "the probe asked nothing";
      return {};
    }
    return probe.fake.seen.front().url;
  }

  SignedInRig rig;
};

TEST_F(HeldCredentials, HoldOnlyTheSeedForAUserWithNothingElse) {
  const auto user = with_the_seed_only(rig);
  const auto facts = user->held_credentials();
  EXPECT_EQ(facts.size(), libchiaking::users::every_credential.size());
  for (const auto kind : libchiaking::users::every_credential) {
    SCOPED_TRACE(static_cast<int>(std::to_underlying(kind)));
    const auto one = fact(facts, kind);
    EXPECT_FALSE(one.used_for.empty());
    if (kind == CredentialKind::sign_in_seed) {
      EXPECT_TRUE(one.held);
      EXPECT_EQ(one.kept, Kept::with_the_user);
      EXPECT_EQ(one.state, State::unverified);
      EXPECT_FALSE(one.obtained_at);
      EXPECT_FALSE(one.expires_at);
      EXPECT_TRUE(one.scopes.empty());
      continue;
    }
    EXPECT_FALSE(one.held);
    EXPECT_EQ(one.state, State::never_obtained);
    EXPECT_FALSE(one.obtained_at);
    EXPECT_FALSE(one.expires_at);
    const auto refused = user->reveal(kind);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().reason, NotRevealed::Reason::not_held);
  }
  EXPECT_TRUE(carries_no_secret(facts));
  EXPECT_TRUE(rig.fake.seen.empty());
}

TEST_F(HeldCredentials, DescribeAndRevealEverySavedCredentialExactly) {
  const auto user = with_everything_saved(rig);
  const auto facts = user->held_credentials();

  const auto seed = fact(facts, CredentialKind::sign_in_seed);
  EXPECT_TRUE(seed.held);
  EXPECT_EQ(seed.kept, Kept::with_the_user);
  EXPECT_EQ(seed.state, State::unverified);
  const auto app = fact(facts, CredentialKind::app_token);
  EXPECT_TRUE(app.held);
  EXPECT_EQ(app.kept, Kept::with_the_user);
  EXPECT_EQ(app.state, State::usable);
  EXPECT_EQ(app.expires_at, at_ms(Psn::wall_start_ms + 3'600'000));
  EXPECT_EQ(app.scopes, "psn:mobile.v2.core psn:clientapp");
  EXPECT_EQ(std::ranges::count(app.used_for, CredentialUse::storefront_check),
            1);
  const auto app_refresh = fact(facts, CredentialKind::app_refresh_token);
  EXPECT_TRUE(app_refresh.held);
  EXPECT_EQ(app_refresh.kept, Kept::with_the_user);
  EXPECT_EQ(app_refresh.state, State::unverified);
  EXPECT_FALSE(seed.obtained_at);
  EXPECT_FALSE(seed.expires_at);
  EXPECT_FALSE(app.obtained_at);
  EXPECT_FALSE(app_refresh.obtained_at);
  EXPECT_FALSE(app_refresh.expires_at);
  const auto remote = fact(facts, CredentialKind::remote_play_token);
  EXPECT_TRUE(remote.held);
  EXPECT_EQ(remote.kept, Kept::with_the_user);
  EXPECT_EQ(remote.state, State::usable);
  EXPECT_EQ(remote.expires_at, at_ms(Psn::wall_start_ms + 7'200'000));
  const auto remote_refresh =
      fact(facts, CredentialKind::remote_play_refresh_token);
  EXPECT_TRUE(remote_refresh.held);
  EXPECT_EQ(remote_refresh.state, State::unverified);
  EXPECT_FALSE(remote.obtained_at);
  EXPECT_FALSE(remote_refresh.obtained_at);
  EXPECT_FALSE(remote_refresh.expires_at);
  EXPECT_FALSE(fact(facts, CredentialKind::library_token).held);
  EXPECT_EQ(fact(facts, CredentialKind::library_token).kept, Kept::in_memory);
  EXPECT_FALSE(fact(facts, CredentialKind::old_service_checkout_token).held);

  const std::array saved{
      std::pair{CredentialKind::sign_in_seed, seed_secret},
      std::pair{CredentialKind::app_token, app_secret},
      std::pair{CredentialKind::app_refresh_token, app_refresh_secret},
      std::pair{CredentialKind::remote_play_token, remote_secret},
      std::pair{CredentialKind::remote_play_refresh_token,
                remote_refresh_secret}};
  for (const auto& [kind, secret] : saved) {
    const auto revealed = user->reveal(kind);
    ASSERT_TRUE(revealed) << static_cast<int>(std::to_underlying(kind));
    EXPECT_EQ(revealed.value().view(), secret);
  }
  EXPECT_FALSE(user->reveal(CredentialKind::library_token));
  EXPECT_FALSE(user->reveal(CredentialKind::old_service_checkout_token));

  EXPECT_TRUE(carries_no_secret(facts));
  EXPECT_TRUE(carries_no_secret(user->held_credentials()));
  EXPECT_TRUE(carries_no_secret(user->snapshot_json().value()));
  EXPECT_TRUE(rig.fake.seen.empty());
  EXPECT_TRUE(rig.fake.unmatched.empty());
}

TEST_F(HeldCredentials, ReadExpiredYetStillRevealSavedTokensPastTheirTime) {
  const auto user = with_everything_saved(rig);
  rig.wall.advance_ms(3'600'000 - 29'000);
  auto facts = user->held_credentials();
  EXPECT_EQ(fact(facts, CredentialKind::app_token).state, State::expired);
  EXPECT_TRUE(fact(facts, CredentialKind::app_token).held);
  EXPECT_EQ(fact(facts, CredentialKind::remote_play_token).state,
            State::usable);
  rig.wall.advance_ms(3'600'000);
  facts = user->held_credentials();
  EXPECT_EQ(fact(facts, CredentialKind::remote_play_token).state,
            State::expired);
  EXPECT_TRUE(fact(facts, CredentialKind::remote_play_token).held);
  EXPECT_EQ(fact(facts, CredentialKind::sign_in_seed).state, State::unverified);
  EXPECT_EQ(fact(facts, CredentialKind::app_refresh_token).state,
            State::unverified);
  const auto app = user->reveal(CredentialKind::app_token);
  ASSERT_TRUE(app);
  EXPECT_EQ(app.value().view(), app_secret);
  const auto remote = user->reveal(CredentialKind::remote_play_token);
  ASSERT_TRUE(remote);
  EXPECT_EQ(remote.value().view(), remote_secret);
  EXPECT_TRUE(carries_no_secret(facts));
  EXPECT_TRUE(rig.fake.seen.empty());
}

TEST_F(HeldCredentials, RestoreSavedDatesAndReadAPassedExpiryAsExpired) {
  const auto obtained = at_ms(Psn::wall_start_ms - 86'400'000);
  SavedSignIn saved{AccountId{5}, "player",
                    libchiaking::psn::auth::Npsso{seed_secret}};
  saved.device = libchiaking::users::DeviceId::parse(Psn::duid);
  saved.npsso_obtained_at = obtained;
  saved.npsso_expires_at = at_ms(Psn::wall_start_ms + 3'000);
  saved.store.emplace(libchiaking::psn::auth::AppToken{app_secret},
                      at_ms(Psn::wall_start_ms + 3'600'000), obtained);
  saved.store_refresh.emplace(
      libchiaking::psn::auth::AppRefreshToken{app_refresh_secret}, obtained,
      at_ms(Psn::wall_start_ms + 1'000));
  saved.remote_play.emplace(libchiaking::psn::auth::BearerToken{remote_secret},
                            at_ms(Psn::wall_start_ms + 7'200'000), obtained);
  saved.remote_play_refresh.emplace(
      libchiaking::psn::auth::RemotePlayRefreshToken{remote_refresh_secret},
      obtained, at_ms(Psn::wall_start_ms + 2'000));
  const auto user = rig.signed_in.as(std::move(saved));
  auto facts = user->held_credentials();
  for (const auto kind :
       {CredentialKind::sign_in_seed, CredentialKind::app_token,
        CredentialKind::app_refresh_token, CredentialKind::remote_play_token,
        CredentialKind::remote_play_refresh_token})
    EXPECT_EQ(fact(facts, kind).obtained_at, obtained)
        << static_cast<int>(std::to_underlying(kind));
  EXPECT_EQ(fact(facts, CredentialKind::sign_in_seed).expires_at,
            at_ms(Psn::wall_start_ms + 3'000));
  EXPECT_EQ(fact(facts, CredentialKind::app_refresh_token).expires_at,
            at_ms(Psn::wall_start_ms + 1'000));
  EXPECT_EQ(fact(facts, CredentialKind::remote_play_refresh_token).expires_at,
            at_ms(Psn::wall_start_ms + 2'000));
  EXPECT_EQ(fact(facts, CredentialKind::sign_in_seed).state, State::unverified);
  EXPECT_EQ(fact(facts, CredentialKind::app_refresh_token).state,
            State::unverified);
  EXPECT_EQ(fact(facts, CredentialKind::remote_play_refresh_token).state,
            State::unverified);

  rig.wall.advance_ms(1'000);
  facts = user->held_credentials();
  EXPECT_EQ(fact(facts, CredentialKind::app_refresh_token).state,
            State::expired);
  EXPECT_EQ(fact(facts, CredentialKind::remote_play_refresh_token).state,
            State::unverified);
  rig.wall.advance_ms(2'000);
  facts = user->held_credentials();
  EXPECT_EQ(fact(facts, CredentialKind::remote_play_refresh_token).state,
            State::expired);
  EXPECT_EQ(fact(facts, CredentialKind::sign_in_seed).state, State::expired);
  EXPECT_TRUE(fact(facts, CredentialKind::app_refresh_token).held);
  EXPECT_EQ(user->sign_in_state(), libchiaking::users::SignInState::signed_in);
  const auto stayed = user->stay_signed_in();
  ASSERT_TRUE(stayed);
  EXPECT_EQ(stayed.value(), libchiaking::users::SignInState::signed_in);
  const auto revealed = user->reveal(CredentialKind::app_refresh_token);
  ASSERT_TRUE(revealed);
  EXPECT_EQ(revealed.value().view(), app_refresh_secret);
  EXPECT_TRUE(carries_no_secret(facts));
  EXPECT_TRUE(rig.fake.seen.empty());
  EXPECT_TRUE(rig.fake.unmatched.empty());

  const auto kept = rig.signed_in.credentials.saved(AccountId{5});
  ASSERT_TRUE(kept);
  EXPECT_EQ(kept->npsso_obtained_at, obtained);
  EXPECT_EQ(kept->npsso_expires_at, at_ms(Psn::wall_start_ms + 3'000));
  ASSERT_TRUE(kept->store_refresh);
  EXPECT_EQ(kept->store_refresh->obtained_at, obtained);
  EXPECT_EQ(kept->store_refresh->expires_at, at_ms(Psn::wall_start_ms + 1'000));
}

TEST_F(HeldCredentials, ReportAnAppTokenPsRefusedAsRefused) {
  const auto user = with_the_seed_only(rig);
  {
    SavedSignIn saved{AccountId{6}, "other",
                      libchiaking::psn::auth::Npsso{seed_secret}};
    saved.store.emplace(libchiaking::psn::auth::AppToken{app_secret},
                        at_ms(Psn::wall_start_ms + 3'600'000));
    const auto holder = rig.signed_in.as(std::move(saved));
    rig.fake.on("GET", std::string{Psn::subscriptions_url}, {Reply{401, "{}"}});
    EXPECT_FALSE(holder->get_my_membership());
    const auto asked = rig.fake.seen.size();
    const auto facts = holder->held_credentials();
    const auto app = fact(facts, CredentialKind::app_token);
    EXPECT_TRUE(app.held);
    EXPECT_EQ(app.state, State::refused);
    EXPECT_EQ(app.last_reply_status, std::uint16_t{401});
    const auto revealed = holder->reveal(CredentialKind::app_token);
    ASSERT_TRUE(revealed);
    EXPECT_EQ(revealed.value().view(), app_secret);
    EXPECT_TRUE(carries_no_secret(facts));
    EXPECT_EQ(rig.fake.seen.size(), asked);
  }
  EXPECT_EQ(fact(user->held_credentials(), CredentialKind::app_token).state,
            State::never_obtained);
}

TEST_F(HeldCredentials,
       KeepAnExchangedLibraryTokenInMemoryUntilPsRefusesTheSeed) {
  const auto authorize_url = first_url_asked(ask_for_a_library_record);
  SignedInRig probe;
  const auto prober = with_everything_saved(probe);
  probe.fake.on("GET", authorize_url,
                {Psn::redirect("https://x/#access_token=" +
                               std::string{library_secret})});
  ask_for_a_library_record(probe, prober);
  ASSERT_EQ(probe.fake.seen.size(), 2U);
  const auto record_url = probe.fake.seen.back().url;

  const auto user = with_everything_saved(rig);
  rig.fake.on("GET", authorize_url,
              {Psn::redirect("https://x/#access_token=" +
                             std::string{library_secret} + "&expires_in=600")});
  rig.fake.on("GET", record_url,
              {Reply{200, R"({"id":"PROD"})"}, Reply{401, "{}"}});
  ask_for_a_library_record(rig, user);
  auto asked = rig.fake.seen.size();
  auto facts = user->held_credentials();
  {
    const auto library = fact(facts, CredentialKind::library_token);
    EXPECT_TRUE(library.held);
    EXPECT_EQ(library.kept, Kept::in_memory);
    EXPECT_EQ(library.state, State::usable);
    ASSERT_TRUE(library.obtained_at);
    ASSERT_TRUE(library.expires_at);
    EXPECT_GT(*library.expires_at, *library.obtained_at);
    EXPECT_LE(*library.expires_at - *library.obtained_at,
              std::chrono::seconds{600});
    EXPECT_FALSE(library.scopes.empty());
    EXPECT_EQ(library.last_reply_status, std::uint16_t{200});
  }
  auto revealed = user->reveal(CredentialKind::library_token);
  ASSERT_TRUE(revealed);
  EXPECT_EQ(revealed.value().view(), library_secret);
  EXPECT_TRUE(carries_no_secret(facts));
  EXPECT_TRUE(carries_no_secret(user->snapshot_json().value()));
  EXPECT_EQ(rig.fake.seen.size(), asked);

  rig.clock.advance_ms(601'000);
  rig.wall.advance_ms(601'000);
  facts = user->held_credentials();
  EXPECT_EQ(fact(facts, CredentialKind::library_token).state, State::expired);
  EXPECT_TRUE(fact(facts, CredentialKind::library_token).held);
  revealed = user->reveal(CredentialKind::library_token);
  ASSERT_TRUE(revealed);
  EXPECT_EQ(revealed.value().view(), library_secret);
  EXPECT_EQ(rig.fake.seen.size(), asked);

  ask_for_a_library_record(rig, user);
  EXPECT_EQ(user->sign_in_state(), libchiaking::users::SignInState::expired);
  asked = rig.fake.seen.size();
  facts = user->held_credentials();
  EXPECT_EQ(fact(facts, CredentialKind::sign_in_seed).state, State::refused);
  EXPECT_TRUE(fact(facts, CredentialKind::sign_in_seed).held);
  EXPECT_EQ(fact(facts, CredentialKind::library_token).state, State::refused);
  const auto seed = user->reveal(CredentialKind::sign_in_seed);
  ASSERT_TRUE(seed);
  EXPECT_EQ(seed.value().view(), seed_secret);
  EXPECT_TRUE(carries_no_secret(facts));
  EXPECT_TRUE(carries_no_secret(user->snapshot_json().value()));
  EXPECT_EQ(rig.fake.seen.size(), asked);
}

TEST_F(HeldCredentials, DescribeTheOldServiceCheckoutTokenOnceExchanged) {
  const auto authorize_url = first_url_asked(ask_the_old_service_checkout);
  const auto user = with_everything_saved(rig);
  rig.fake.on("GET", authorize_url,
              {Psn::redirect("https://psnow.playstation.com/#access_token=" +
                             std::string{checkout_secret})});
  ask_the_old_service_checkout(rig, user);
  const auto asked = rig.fake.seen.size();
  const auto facts = user->held_credentials();
  const auto checkout = fact(facts, CredentialKind::old_service_checkout_token);
  EXPECT_TRUE(checkout.held);
  EXPECT_EQ(checkout.kept, Kept::in_memory);
  EXPECT_EQ(checkout.state, State::usable);
  EXPECT_EQ(checkout.used_for,
            std::vector{CredentialUse::old_service_checkout});
  EXPECT_FALSE(fact(facts, CredentialKind::library_token).held);
  const auto revealed =
      user->reveal(CredentialKind::old_service_checkout_token);
  ASSERT_TRUE(revealed);
  EXPECT_EQ(revealed.value().view(), checkout_secret);
  EXPECT_FALSE(user->reveal(CredentialKind::library_token));
  EXPECT_TRUE(carries_no_secret(facts));
  EXPECT_TRUE(carries_no_secret(user->snapshot_json().value()));
  EXPECT_EQ(rig.fake.seen.size(), asked);
}

TEST_F(HeldCredentials, AreNeitherHeldNorRevealedForAUserWhoLeft) {
  const auto user = with_everything_saved(rig);
  EXPECT_TRUE(rig.signed_in.users.sign_out(AccountId{5}));
  const auto facts = user->held_credentials();
  EXPECT_EQ(facts.size(), libchiaking::users::every_credential.size());
  for (const auto kind : libchiaking::users::every_credential) {
    SCOPED_TRACE(static_cast<int>(std::to_underlying(kind)));
    EXPECT_FALSE(fact(facts, kind).held);
    EXPECT_EQ(fact(facts, kind).state, State::never_obtained);
    const auto refused = user->reveal(kind);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().reason, NotRevealed::Reason::signed_out);
  }
  EXPECT_TRUE(carries_no_secret(facts));
  EXPECT_TRUE(rig.fake.seen.empty());
}

}  // namespace
}  // namespace users_test
