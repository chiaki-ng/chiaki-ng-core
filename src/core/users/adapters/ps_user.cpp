#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/account/client.hpp"
#include "src/core/psn/auth/authenticator.hpp"
#include "src/core/psn/auth/sign_in.hpp"
#include "src/core/psn/session_internals.hpp"
#include "src/core/users/adapters/account_claims.hpp"
#include "src/core/users/adapters/cloud_streaming_cache.hpp"
#include "src/core/users/adapters/membership_cache.hpp"
#include "src/core/users/adapters/ps_user_core.hpp"
#include "src/core/users/cloud_relaunch.hpp"
#include "src/core/users/ports/membership_calls.hpp"
#include "src/core/utils/json/json.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace libchiaking::users {

PsUser::Core::Core(const Providers& providers, AccountId account,
                   std::string online_id, DeviceId device,
                   std::unique_ptr<psn::Session> session)
    : account_(account),
      online_id_(std::move(online_id)),
      device_(std::move(device)),
      http_(providers.http),
      credentials_(providers.credentials),
      clock_(providers.clock),
      wall_(providers.wall),
      log_(providers.log),
      server_time_(providers.server_time),
      session_(std::move(session)),
      memberships_(std::make_unique<MembershipCache>(
          providers.threading, providers.clock, providers.wall)),
      in_flight_(
          std::make_unique<InFlight>(providers.threading, providers.clock)),
      cloud_streaming_(std::make_unique<CloudStreamingCache>(
          providers.threading, providers.clock)),
      cloud_relaunch_(std::make_unique<CloudRelaunch>(providers.threading,
                                                      providers.clock)),
      mutex_(providers.threading),
      profile_flight_(providers.threading),
      renewal_(providers.threading) {}

PsUser::Core::Core(const Providers& providers, AccountId account,
                   std::string name)
    : account_(account),
      online_id_(std::move(name)),
      http_(providers.http),
      credentials_(providers.credentials),
      clock_(providers.clock),
      wall_(providers.wall),
      log_(providers.log),
      server_time_(providers.server_time),
      session_(std::make_unique<psn::Session>(
          psn::auth::Npsso{std::string_view{}}, providers.threading,
          providers.clock, providers.wall)),
      memberships_(std::make_unique<MembershipCache>(
          providers.threading, providers.clock, providers.wall)),
      in_flight_(
          std::make_unique<InFlight>(providers.threading, providers.clock)),
      cloud_streaming_(std::make_unique<CloudStreamingCache>(
          providers.threading, providers.clock)),
      cloud_relaunch_(std::make_unique<CloudRelaunch>(providers.threading,
                                                      providers.clock)),
      mutex_(providers.threading),
      profile_flight_(providers.threading),
      renewal_(providers.threading) {}

PsUser::Core::~Core() = default;

auto PsUser::Core::admit(const Providers& providers, SavedHomeUser saved)
    -> Expected<std::shared_ptr<PsUser>, Invalid> {
  if (saved.account.value() == 0) return unexpected(Invalid::account_id);
  if (!valid_online_id(saved.name)) return unexpected(Invalid::online_id);
  return std::make_shared<PsUser>(
      std::make_unique<Core>(providers, saved.account, std::move(saved.name)));
}

auto PsUser::Core::admit(const Providers& providers, SavedSignIn saved)
    -> Expected<std::shared_ptr<PsUser>, Invalid> {
  if (saved.account.value() == 0) return unexpected(Invalid::account_id);
  if (!valid_online_id(saved.online_id)) return unexpected(Invalid::online_id);
  if (!psn::Session::valid_npsso(saved.npsso.view()))
    return unexpected(Invalid::npsso);
  if (saved.region && !valid_region(*saved.region))
    return unexpected(Invalid::region);
  if ((saved.store_refresh && saved.store_refresh->token.empty()) ||
      (saved.store && saved.store->token.empty()))
    return unexpected(Invalid::store_access);
  if ((saved.remote_play && (saved.remote_play->token.empty() ||
                             saved.remote_play->token.view().size() >
                                 psn::account::Client::token_limit)) ||
      (saved.remote_play_refresh && saved.remote_play_refresh->token.empty()))
    return unexpected(Invalid::remote_play_access);
  if (!saved.device) saved.device = DeviceId::generate(providers.random);
  if (!saved.device) return unexpected(Invalid::device_id);
  auto session = std::make_unique<psn::Session>(
      std::move(saved.npsso), providers.threading, providers.clock,
      providers.wall);
  if (saved.store && !session->supply_app_token(saved.store->token.view(),
                                                saved.store->expires_at))
    return unexpected(Invalid::store_access);
  auto core = std::make_unique<Core>(
      providers, saved.account, std::move(saved.online_id),
      std::move(*saved.device), std::move(session));
  if (saved.store) {
    core->store_expires_at_ = saved.store->expires_at;
    core->store_obtained_at_ = saved.store->obtained_at;
  }
  core->seed_obtained_at_ = saved.npsso_obtained_at;
  core->seed_expires_at_ = saved.npsso_expires_at;
  core->store_refresh_ = std::move(saved.store_refresh);
  core->remote_play_ = std::move(saved.remote_play);
  core->remote_play_refresh_ = std::move(saved.remote_play_refresh);
  core->region_ = std::move(saved.region);
  core->context_ = std::move(saved.context);
  if (saved.store)
    core->learn_context(AccountClaims::read(saved.store->token.view()));
  return std::make_shared<PsUser>(std::move(core));
}

auto PsUser::Core::valid_online_id(std::string_view online_id) noexcept
    -> bool {
  return !online_id.empty() && online_id.size() <= online_id_limit &&
         std::ranges::none_of(online_id, [](char byte) {
           return static_cast<unsigned char>(byte) < 0x20;
         });
}

auto PsUser::Core::valid_region(const Region& region) noexcept -> bool {
  const auto usable = [](std::string_view part) {
    return !part.empty() && part.size() <= region_text_limit &&
           std::ranges::all_of(part, [](char byte) {
             return text::ascii_alnum(byte) || byte == '-';
           });
  };
  return usable(region.country) && usable(region.language);
}

auto PsUser::Core::unanswered(const psn::account::Failure& failure) noexcept
    -> Unanswered {
  using Reason = Unanswered::Reason;
  switch (failure.error) {
    case psn::account::Error::cancelled:
      return {Reason::cancelled, failure.http_status};
    case psn::account::Error::rate_limited:
      return {Reason::rate_limited, failure.http_status};
    case psn::account::Error::malformed:
      return {Reason::malformed, failure.http_status};
    case psn::account::Error::invalid_argument:
      return {Reason::rejected, failure.http_status};
    case psn::account::Error::auth:
    case psn::account::Error::unavailable:
      break;
  }
  return {Reason::unavailable, failure.http_status};
}

auto PsUser::Core::unanswered(const psn::auth::SignInError& error) noexcept
    -> Unanswered {
  using Reason = Unanswered::Reason;
  switch (error.kind()) {
    case psn::auth::SignInErrorKind::cancelled:
      return {Reason::cancelled, error.http_status()};
    case psn::auth::SignInErrorKind::rejected:
    case psn::auth::SignInErrorKind::declined:
    case psn::auth::SignInErrorKind::verification_required:
    case psn::auth::SignInErrorKind::expired:
      return {Reason::sign_in_expired, error.http_status()};
    case psn::auth::SignInErrorKind::rate_limited:
      return {Reason::rate_limited, error.http_status()};
    case psn::auth::SignInErrorKind::malformed:
      return {Reason::malformed, error.http_status()};
    case psn::auth::SignInErrorKind::invalid_request:
      return {Reason::rejected, error.http_status()};
    case psn::auth::SignInErrorKind::transport:
    case psn::auth::SignInErrorKind::unavailable:
    case psn::auth::SignInErrorKind::layout_changed:
      break;
  }
  return {Reason::unavailable, error.http_status()};
}

auto PsUser::Core::wall_now() const noexcept
    -> std::chrono::system_clock::time_point {
  return std::chrono::system_clock::time_point{
      std::chrono::microseconds{wall_.now_microseconds()}};
}

auto PsUser::Core::avatar_url() const -> std::optional<std::string> {
  platform::Lock lock{mutex_};
  return profile_ ? profile_->avatar_url : std::nullopt;
}

auto PsUser::Core::membership() const -> Membership {
  if (auto remembered = memberships_->remembered())
    return std::move(remembered->membership);
  return inferred_membership();
}

auto PsUser::Core::inferred_membership() const -> Membership {
  platform::Lock lock{mutex_};
  if (!profile_) return Membership{};
  switch (profile_->plus) {
    case psn::PlusStatus::active:
      return Membership::member_of_unknown_tier();
    case psn::PlusStatus::absent:
      return Membership{Tier::none};
    case psn::PlusStatus::unknown:
      break;
  }
  return Membership{};
}

auto PsUser::Core::region() const -> std::optional<Region> {
  platform::Lock lock{mutex_};
  return region_;
}

auto PsUser::Core::account_context() const -> AccountContext {
  platform::Lock lock{mutex_};
  return context_;
}

auto PsUser::Core::sign_in_state() const -> SignInState {
  if (off_psn()) return SignInState::signed_out;
  const psn::SessionInternals held{*session_};
  {
    platform::Lock lock{mutex_};
    if (sign_in_refused_) return SignInState::expired;
  }
  if (held.tokens().npsso_refused()) return SignInState::expired;
  if (!held.app_token().lease()) return SignInState::store_access_lapsed;
  return SignInState::signed_in;
}

auto PsUser::Core::is_signed_in() const -> bool {
  const auto state = sign_in_state();
  return state == SignInState::signed_in ||
         state == SignInState::store_access_lapsed;
}

auto PsUser::Core::is_expired() const -> bool {
  return sign_in_state() == SignInState::expired;
}

auto PsUser::Core::fresh_profile() const -> std::optional<Profile> {
  {
    platform::Lock lock{mutex_};
    const auto lifetime =
        std::chrono::duration_cast<std::chrono::microseconds>(profile_lifetime);
    if (!profile_ ||
        clock_.now_microseconds() >=
            profile_->learned_us + static_cast<std::uint64_t>(lifetime.count()))
      return std::nullopt;
  }
  return Profile{online_id_, avatar_url(), membership()};
}

auto PsUser::Core::get_my_profile(const platform::StopToken& stop)
    -> Expected<Profile, Unanswered> {
  if (off_psn()) return unexpected(Unanswered{Unanswered::Reason::signed_out});
  platform::Lock flight{profile_flight_};
  if (auto known = fresh_profile()) return std::move(*known);
  auto& slot = psn::SessionInternals{*session_}.app_token();
  auto token = app_token_unless_refused(stop);
  if (!token) return unexpected(token.error());
  auto fetched = psn::account::Client{http_, clock_, stop, log_}.profile(
      token.value(), account_);
  if (!fetched) {
    if (fetched.error().error != psn::account::Error::auth)
      return unexpected(unanswered(fetched.error()));
    slot.record_use(token.value(), fetched.error().http_status, 0);
    return unexpected(Unanswered{Unanswered::Reason::store_access_lapsed,
                                 fetched.error().http_status});
  }
  {
    platform::Lock lock{mutex_};
    profile_ = KnownProfile{std::move(fetched.value().avatar_url),
                            fetched.value().plus, clock_.now_microseconds()};
  }
  return Profile{online_id_, avatar_url(), membership()};
}

auto PsUser::Core::get_my_membership(const platform::StopToken& stop)
    -> Expected<MembershipAnswer, Unanswered> {
  if (off_psn()) return unexpected(Unanswered{Unanswered::Reason::signed_out});
  net::http::Client http{http_, clock_, stop, log_};
  return MembershipCalls{*this}.answered_membership(http);
}

auto PsUser::Core::remote_play_token() const
    -> std::optional<psn::auth::BearerToken> {
  platform::Lock lock{mutex_};
  if (!remote_play_ ||
      wall_now() + remote_play_margin >= remote_play_->expires_at)
    return std::nullopt;
  return remote_play_->token.clone();
}

auto PsUser::Core::stay_signed_in(const platform::StopToken& stop)
    -> Expected<SignInState, Unanswered> {
  if (off_psn()) return unexpected(Unanswered{Unanswered::Reason::signed_out});
  platform::Lock renewal{renewal_};
  bool renewed = false;
  if (!psn::SessionInternals{*session_}.app_token().lease()) {
    if (auto store = renew_store_access(stop); !store)
      return unexpected(store.error());
    renewed = true;
  }
  bool remote_play_lapsed = false;
  {
    platform::Lock lock{mutex_};
    remote_play_lapsed = remote_play_refresh_.has_value() &&
                         (!remote_play_ || wall_now() + remote_play_margin >=
                                               remote_play_->expires_at);
  }
  if (remote_play_lapsed) {
    if (auto remote_play = renew_remote_play_access(stop); !remote_play)
      return unexpected(remote_play.error());
    renewed = true;
  }
  if (renewed) save();
  return sign_in_state();
}

auto PsUser::Core::renew_store_access(const platform::StopToken& stop)
    -> Expected<void, Unanswered> {
  psn::auth::SignIn sign_in{http_, clock_, stop, log_};
  std::optional<psn::auth::AppRefreshToken> refresh;
  {
    platform::Lock lock{mutex_};
    if (store_refresh_) refresh.emplace(store_refresh_->token.clone());
  }
  const auto app_duid = device_ ? device_->view() : std::string_view{};
  auto issued = sign_in.app_tokens(session_->npsso(), app_duid);
  if (!issued && refresh &&
      issued.error().kind() == psn::auth::SignInErrorKind::rejected)
    issued = sign_in.refresh(*refresh);
  platform::Lock lock{mutex_};
  if (!issued) {
    if (issued.error().kind() == psn::auth::SignInErrorKind::rejected) {
      sign_in_refused_ = true;
      store_refresh_.reset();
    }
    return unexpected(unanswered(issued.error()));
  }
  const auto now = wall_now();
  store_expires_at_ = now + issued.value().expires_in;
  store_obtained_at_ = now;
  learn_context(AccountClaims::read(issued.value().access.view()));
  psn::SessionInternals{*session_}.app_token().supply(
      std::move(issued.value().access), store_expires_at_);
  if (issued.value().refresh.empty())
    store_refresh_.reset();
  else
    store_refresh_.emplace(std::move(issued.value().refresh), now,
                           issued.value().refresh_expires_at(now));
  sign_in_refused_ = false;
  return {};
}

auto PsUser::Core::renew_remote_play_access(const platform::StopToken& stop)
    -> Expected<void, Unanswered> {
  std::optional<psn::auth::RemotePlayRefreshToken> refresh;
  {
    platform::Lock lock{mutex_};
    if (remote_play_refresh_)
      refresh.emplace(remote_play_refresh_->token.clone());
  }
  if (!refresh)
    return unexpected(
        Unanswered{Unanswered::Reason::remote_play_access_missing});
  auto issued = psn::auth::SignIn{http_, clock_, stop, log_}.refresh(*refresh);
  platform::Lock lock{mutex_};
  if (!issued) {
    if (issued.error().kind() != psn::auth::SignInErrorKind::rejected)
      return unexpected(unanswered(issued.error()));
    remote_play_.reset();
    remote_play_refresh_.reset();
    return unexpected(Unanswered{Unanswered::Reason::remote_play_access_missing,
                                 issued.error().http_status()});
  }
  const auto now = wall_now();
  remote_play_.emplace(std::move(issued.value().access),
                       now + issued.value().expires_in, now);
  remote_play_refresh_.emplace(std::move(issued.value().refresh), now,
                               issued.value().refresh_expires_at(now));
  return {};
}

auto PsUser::Core::restore_store_access(const platform::StopToken& stop)
    -> Expected<void, Unanswered> {
  platform::Lock renewal{renewal_};
  if (psn::SessionInternals{*session_}.app_token().lease()) return {};
  if (auto renewed = renew_store_access(stop); !renewed) return renewed;
  save();
  return {};
}

auto PsUser::Core::restored_app_token(const platform::StopToken& stop)
    -> Expected<psn::auth::AppToken, Unanswered> {
  if (auto restored = restore_store_access(stop); !restored)
    return unexpected(restored.error());
  auto token = psn::SessionInternals{*session_}.app_token().lease();
  if (!token)
    return unexpected(Unanswered{Unanswered::Reason::store_access_lapsed});
  return std::move(token.value());
}

auto PsUser::Core::app_token_unless_refused(const platform::StopToken& stop)
    -> Expected<psn::auth::AppToken, Unanswered> {
  if (!is_expired()) return restored_app_token(stop);
  auto token = psn::SessionInternals{*session_}.app_token().lease();
  if (!token)
    return unexpected(Unanswered{Unanswered::Reason::sign_in_expired});
  return std::move(token.value());
}

auto PsUser::Core::restore_remote_play_access(const platform::StopToken& stop)
    -> Expected<void, Unanswered> {
  platform::Lock renewal{renewal_};
  if (remote_play_token()) return {};
  if (auto renewed = renew_remote_play_access(stop); !renewed) return renewed;
  save();
  return {};
}

void PsUser::Core::distrust_remote_play_access(
    const psn::auth::BearerToken& refused) {
  platform::Lock lock{mutex_};
  if (remote_play_ && remote_play_->token.view() == refused.view())
    remote_play_.reset();
}

auto PsUser::Core::saved_sign_in() const -> SavedSignIn {
  SavedSignIn saved{account_, online_id_, session_->npsso().clone()};
  if (device_) saved.device.emplace(device_->clone());
  auto store = psn::SessionInternals{*session_}.app_token().lease();
  platform::Lock lock{mutex_};
  saved.npsso_obtained_at = seed_obtained_at_;
  saved.npsso_expires_at = seed_expires_at_;
  saved.context = context_;
  if (store)
    saved.store.emplace(std::move(store.value()), store_expires_at_,
                        store_obtained_at_);
  if (store_refresh_)
    saved.store_refresh.emplace(store_refresh_->token.clone(),
                                store_refresh_->obtained_at,
                                store_refresh_->expires_at);
  if (remote_play_)
    saved.remote_play.emplace(remote_play_->token.clone(),
                              remote_play_->expires_at,
                              remote_play_->obtained_at);
  if (remote_play_refresh_)
    saved.remote_play_refresh.emplace(remote_play_refresh_->token.clone(),
                                      remote_play_refresh_->obtained_at,
                                      remote_play_refresh_->expires_at);
  saved.region = region_;
  return saved;
}

void PsUser::Core::save() {
  if (signed_out_) return;
  saved_ =
      has_psn_account()
          ? credentials_.save(saved_sign_in()).has_value()
          : credentials_.save(SavedHomeUser{account_, online_id_}).has_value();
}

void PsUser::Core::refuse_sign_in() {
  platform::Lock lock{mutex_};
  sign_in_refused_ = true;
}

void PsUser::Core::learn_region(Region region) {
  if (!valid_region(region)) return;
  {
    platform::Lock lock{mutex_};
    if (region_ == region) return;
    region_ = std::move(region);
  }
  save();
}

void PsUser::Core::learn_context(const AccountContext& stated) {
  if (stated.names_a_catalogue_audience()) context_ = stated;
}

void PsUser::Core::leave() {
  signed_out_ = true;
  session_->clear();
  memberships_->clear();
  cloud_streaming_->clear();
  platform::Lock lock{mutex_};
  profile_.reset();
  context_ = {};
  store_refresh_.reset();
  remote_play_.reset();
  remote_play_refresh_.reset();
  store_expires_at_ = {};
  store_obtained_at_.reset();
  seed_obtained_at_.reset();
  seed_expires_at_.reset();
}

auto PsUser::Core::snapshot_json() -> std::optional<std::string> {
  std::string membership{"null"};
  if (auto held = memberships_->held())
    membership = MembershipCalls::membership_json(held->first, held->second);
  return psn::SessionInternals{*session_}.snapshot_json(membership);
}

auto PsUser::Core::uses_of(CredentialKind kind) -> std::vector<CredentialUse> {
  using Use = CredentialUse;
  switch (kind) {
    case CredentialKind::sign_in_seed:
      return {Use::exchanging_tokens, Use::cloud_streaming_authorisation,
              Use::old_service_store, Use::renewing_the_app_token};
    case CredentialKind::app_token:
      return {Use::profile,          Use::membership,
              Use::storefront_check, Use::console_titles,
              Use::starting_a_title, Use::owned_copy_concepts};
    case CredentialKind::app_refresh_token:
      return {Use::renewing_the_app_token};
    case CredentialKind::remote_play_token:
      return {Use::account_consoles, Use::internet_play};
    case CredentialKind::remote_play_refresh_token:
      return {Use::renewing_the_remote_play_token};
    case CredentialKind::library_token:
      return {Use::owned_library};
    case CredentialKind::old_service_checkout_token:
      break;
  }
  return {Use::old_service_checkout};
}

auto PsUser::Core::time_at(const std::optional<std::int64_t>& unix_ms)
    -> std::optional<HeldCredential::Time> {
  if (!unix_ms) return std::nullopt;
  return HeldCredential::Time{std::chrono::milliseconds{*unix_ms}};
}

auto PsUser::Core::lasting(
    CredentialKind kind, bool held,
    const std::optional<HeldCredential::Time>& obtained_at,
    const std::optional<HeldCredential::Time>& expires_at) const
    -> HeldCredential {
  using State = HeldCredential::State;
  HeldCredential facts{.kind = kind,
                       .held = held,
                       .kept = HeldCredential::Kept::with_the_user,
                       .used_for = uses_of(kind)};
  if (!held) return facts;
  facts.obtained_at = obtained_at;
  facts.expires_at = expires_at;
  facts.state = expires_at && wall_now() >= *expires_at ? State::expired
                                                        : State::unverified;
  return facts;
}

auto PsUser::Core::exchanged(CredentialKind kind,
                             const std::optional<psn::auth::TokenStats>& stats)
    -> HeldCredential {
  using State = HeldCredential::State;
  HeldCredential facts{.kind = kind, .used_for = uses_of(kind)};
  if (!stats) return facts;
  switch (stats->state) {
    case psn::auth::TokenState::none:
      facts.state =
          net::HttpStatus{stats->usage.last_exchange_status}.is_auth_refused()
              ? State::refused
              : State::never_obtained;
      break;
    case psn::auth::TokenState::valid:
      facts.state = State::usable;
      break;
    case psn::auth::TokenState::expired:
      facts.state = State::expired;
      break;
    case psn::auth::TokenState::invalidated:
      facts.state = State::refused;
      break;
  }
  facts.held = stats->state == psn::auth::TokenState::valid ||
               stats->state == psn::auth::TokenState::expired;
  facts.obtained_at = time_at(stats->obtained_at_ms);
  facts.expires_at = time_at(stats->expires_at_ms);
  facts.scopes = stats->scope;
  if (stats->usage.last_use_status != 0)
    facts.last_reply_status = stats->usage.last_use_status;
  else if (stats->usage.last_exchange_status != 0)
    facts.last_reply_status = stats->usage.last_exchange_status;
  return facts;
}

auto PsUser::Core::held_credentials() const -> std::vector<HeldCredential> {
  using Kept = HeldCredential::Kept;
  using State = HeldCredential::State;
  std::vector<HeldCredential> all;
  if (off_psn()) {
    for (const auto kind : every_credential)
      all.push_back(HeldCredential{.kind = kind, .used_for = uses_of(kind)});
    return all;
  }
  const psn::SessionInternals held{*session_};
  const bool seed_refused = held.tokens().npsso_refused();
  auto app = exchanged(CredentialKind::app_token, held.app_token().stats());
  app.kept = Kept::with_the_user;
  app.held = held.app_token().held().has_value();
  switch (held.app_token().leasable()) {
    case psn::auth::TokenState::none:
      app.state = State::never_obtained;
      break;
    case psn::auth::TokenState::valid:
      app.state = State::usable;
      break;
    case psn::auth::TokenState::expired:
      app.state = State::expired;
      break;
    case psn::auth::TokenState::invalidated:
      app.state = State::refused;
      break;
  }
  auto library = exchanged(
      CredentialKind::library_token,
      held.tokens().standing(psn::auth::Authenticator::library_grant()));
  library.held =
      held.tokens().held(psn::auth::Authenticator::library_grant()).has_value();
  auto checkout =
      exchanged(CredentialKind::old_service_checkout_token,
                held.tokens().standing(
                    psn::auth::Authenticator::old_service_checkout_grant()));
  checkout.held =
      held.tokens()
          .held(psn::auth::Authenticator::old_service_checkout_grant())
          .has_value();

  platform::Lock lock{mutex_};
  auto seed = lasting(CredentialKind::sign_in_seed, true, seed_obtained_at_,
                      seed_expires_at_);
  if (sign_in_refused_ || seed_refused) seed.state = State::refused;
  all.push_back(std::move(seed));
  app.obtained_at = app.held ? store_obtained_at_ : std::nullopt;
  all.push_back(std::move(app));
  all.push_back(
      store_refresh_
          ? lasting(CredentialKind::app_refresh_token, true,
                    store_refresh_->obtained_at, store_refresh_->expires_at)
          : lasting(CredentialKind::app_refresh_token, false, {}, {}));
  HeldCredential remote_play{
      .kind = CredentialKind::remote_play_token,
      .held = remote_play_.has_value(),
      .kept = Kept::with_the_user,
      .used_for = uses_of(CredentialKind::remote_play_token)};
  if (remote_play_) {
    remote_play.obtained_at = remote_play_->obtained_at;
    remote_play.expires_at = remote_play_->expires_at;
    remote_play.state =
        wall_now() + remote_play_margin >= remote_play_->expires_at
            ? State::expired
            : State::usable;
  }
  all.push_back(std::move(remote_play));
  all.push_back(
      remote_play_refresh_
          ? lasting(CredentialKind::remote_play_refresh_token, true,
                    remote_play_refresh_->obtained_at,
                    remote_play_refresh_->expires_at)
          : lasting(CredentialKind::remote_play_refresh_token, false, {}, {}));
  all.push_back(std::move(library));
  all.push_back(std::move(checkout));
  return all;
}

auto PsUser::Core::reveal(CredentialKind kind) const
    -> Expected<SecretString, NotRevealed> {
  using Reason = NotRevealed::Reason;
  if (off_psn()) return unexpected(NotRevealed{Reason::signed_out});
  const psn::SessionInternals held{*session_};
  std::optional<SecretString> secret;
  switch (kind) {
    case CredentialKind::sign_in_seed:
      secret = session_->npsso().secret().clone();
      break;
    case CredentialKind::app_token:
      secret = held.app_token().held();
      break;
    case CredentialKind::library_token:
      secret = held.tokens().held(psn::auth::Authenticator::library_grant());
      break;
    case CredentialKind::old_service_checkout_token:
      secret = held.tokens().held(
          psn::auth::Authenticator::old_service_checkout_grant());
      break;
    case CredentialKind::app_refresh_token:
    case CredentialKind::remote_play_token:
    case CredentialKind::remote_play_refresh_token: {
      platform::Lock lock{mutex_};
      if (kind == CredentialKind::app_refresh_token && store_refresh_)
        secret = store_refresh_->token.secret().clone();
      if (kind == CredentialKind::remote_play_token && remote_play_)
        secret = remote_play_->token.secret().clone();
      if (kind == CredentialKind::remote_play_refresh_token &&
          remote_play_refresh_)
        secret = remote_play_refresh_->token.secret().clone();
      break;
    }
  }
  if (!secret || secret->empty())
    return unexpected(NotRevealed{Reason::not_held});
  return std::move(*secret);
}

PsUser::PsUser(std::unique_ptr<Core> core) : core_(std::move(core)) {}

PsUser::~PsUser() = default;

auto PsUser::account_id() const noexcept -> AccountId {
  return core_->account_id();
}

auto PsUser::online_id() const noexcept -> const std::string& {
  return core_->online_id();
}

auto PsUser::avatar_url() const -> std::optional<std::string> {
  return core_->avatar_url();
}

auto PsUser::membership() const -> Membership { return core_->membership(); }

auto PsUser::region() const -> std::optional<Region> { return core_->region(); }

auto PsUser::account_context() const -> AccountContext {
  return core_->account_context();
}

auto PsUser::held_credentials() const -> std::vector<HeldCredential> {
  return core_->held_credentials();
}

auto PsUser::reveal(CredentialKind kind) const
    -> Expected<SecretString, NotRevealed> {
  return core_->reveal(kind);
}

auto PsUser::sign_in_state() const -> SignInState {
  return core_->sign_in_state();
}

auto PsUser::is_signed_in() const -> bool { return core_->is_signed_in(); }

auto PsUser::is_expired() const -> bool { return core_->is_expired(); }

auto PsUser::is_saved() const noexcept -> bool { return core_->is_saved(); }

auto PsUser::has_psn_account() const noexcept -> bool {
  return core_->has_psn_account();
}

auto PsUser::is_off_psn() const noexcept -> bool { return core_->off_psn(); }

auto PsUser::get_my_profile(const platform::StopToken& stop)
    -> Expected<Profile, Unanswered> {
  return core_->get_my_profile(stop);
}

auto PsUser::get_my_membership(const platform::StopToken& stop)
    -> Expected<MembershipAnswer, Unanswered> {
  return core_->get_my_membership(stop);
}

auto PsUser::stay_signed_in(const platform::StopToken& stop)
    -> Expected<SignInState, Unanswered> {
  return core_->stay_signed_in(stop);
}

auto PsUser::snapshot_json() -> std::optional<std::string> {
  return core_->snapshot_json();
}

}  // namespace libchiaking::users
