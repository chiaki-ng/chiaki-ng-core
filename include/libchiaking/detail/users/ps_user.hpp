/// @file
/// @brief Declares PsUser, one person using the library, and the outcomes of
/// asking PSN on their behalf.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/detail/users/account_id.hpp"
#include "libchiaking/detail/users/credential_store.hpp"
#include "libchiaking/detail/users/held_credential.hpp"
#include "libchiaking/detail/users/membership.hpp"
#include "libchiaking/detail/users/plus_status.hpp"
#include "libchiaking/platform/runtime/stop_token.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace libchiaking::platform {
class ClockProvider;
class DiagnosticsProvider;
class HttpProvider;
class SecureRandomProvider;
}  // namespace libchiaking::platform

namespace libchiaking::users {

struct InFlight;
class MembershipCache;
class OldServiceCheckout;
class RemotePlayAccess;
class SignedInUsers;
class ServerTime;

/// @brief How a user currently stands with PSN.
enum class SignInState : std::uint8_t {
  /// @brief The sign-in is accepted and a valid app token is held.
  signed_in,
  /// @brief The sign-in is accepted but the app token is missing or expired.
  /// PsUser::stay_signed_in renews it.
  store_access_lapsed,
  /// @brief PSN refused the sign-in itself. The user must sign in again
  /// through SignedInUsers::sign_in_again.
  expired,
  /// @brief The user has no PSN sign-in: they were added without one, or they
  /// have been signed out.
  signed_out,
};

/// @brief A user's public PSN profile.
/// @ingroup api_users
struct Profile {
  /// @brief The account's online id.
  std::string online_id;
  /// @brief Address of the avatar image, when the profile has one.
  std::optional<std::string> avatar_url;
  /// @brief What is known about the user's PlayStation Plus membership.
  Membership membership;
};

/// @brief Why a question put to PSN on a user's behalf got no answer.
/// @ingroup api_users
struct Unanswered {
  /// @brief The kinds of failure.
  enum class Reason : std::uint8_t {
    /// @brief The user has no PSN sign-in, so nothing was sent.
    signed_out,
    /// @brief PSN refused the sign-in. The user must sign in again.
    sign_in_expired,
    /// @brief The app token is missing, expired or was refused, and could
    /// not be used for this call.
    store_access_lapsed,
    /// @brief No usable Remote Play token is held and none could be
    /// obtained.
    remote_play_access_missing,
    /// @brief The stop token was triggered.
    cancelled,
    /// @brief PSN is rate limiting the account or the client.
    rate_limited,
    /// @brief The request did not complete or PSN answered with an
    /// unexpected status. This is the default value.
    unavailable,
    /// @brief PSN rejected the request as invalid.
    rejected,
    /// @brief PSN's reply could not be understood.
    malformed,
    /// @brief The older PlayStation Now store service refused to open a
    /// session.
    old_service_refused,
  };

  /// @brief What went wrong.
  Reason reason{Reason::unavailable};
  /// @brief The HTTP status of the reply behind the failure, or 0 when there
  /// was none.
  std::uint16_t http_status{};

  /// @brief Builds the failure reported when the caller's stop token fired.
  ///
  /// @return A failure with Reason::cancelled and no HTTP status.
  static constexpr auto cancelled() noexcept -> Unanswered {
    return Unanswered{Reason::cancelled};
  }

  /// @brief Whether the question was abandoned because the caller's stop
  /// token fired, rather than answered with a failure.
  ///
  /// @return True when reason is Reason::cancelled.
  constexpr auto is_cancelled() const noexcept -> bool {
    return reason == Reason::cancelled;
  }
};

/// @brief One person using the library: a PSN account with its sign-in, or a
/// home user known only by account id and name.
///
/// Users are created and owned by SignedInUsers and handed out as
/// `std::shared_ptr<PsUser>`. A user who signed in holds the NPSSO and the
/// tokens obtained from it; these secrets stay inside the object in wiping
/// memory and are never returned by the ordinary accessors. A home user holds
/// no secrets, reports SignInState::signed_out, and can pair and play on the
/// home network only.
///
/// All members are safe to call from several threads. The members that ask PSN
/// (get_my_profile, get_my_membership, stay_signed_in) block the calling
/// thread.
///
/// After SignedInUsers::sign_out, or once the SignedInUsers that owns the user
/// is destroyed, the object stays valid for whoever still holds it but its
/// secrets have been wiped and it reports SignInState::signed_out.
///
/// @see SignedInUsers
/// @ingroup api_users
class PsUser {
 public:
  /// @brief Which part of a saved or supplied user was not acceptable.
  enum class Invalid : std::uint8_t {
    /// @brief The account id is zero.
    account_id,
    /// @brief The online id or name is empty, longer than online_id_limit
    /// or holds a control character.
    online_id,
    /// @brief The NPSSO is empty, too long or holds characters a cookie
    /// value cannot.
    npsso,
    /// @brief The app token or its refresh token is empty or was not
    /// accepted.
    store_access,
    /// @brief The Remote Play token is empty or too long, or its refresh
    /// token is empty.
    remote_play_access,
    /// @brief The region's country or language is empty, longer than
    /// region_text_limit or holds something other than letters, digits and
    /// hyphens.
    region,
    /// @brief No device id was saved and a new one could not be generated.
    device_id,
  };

  /// @brief Longest online id or home user name accepted, in bytes.
  static constexpr std::size_t online_id_limit = 256;
  /// @brief Longest country or language code accepted in a Region, in bytes.
  static constexpr std::size_t region_text_limit = 8;
  /// @brief How long a fetched profile is reused before get_my_profile() asks
  /// PSN again.
  static constexpr std::chrono::hours profile_lifetime{1};
  /// @brief How long before its stated expiry a Remote Play token is already
  /// treated as expired.
  static constexpr std::chrono::seconds remote_play_margin{60};

  /// @brief Not copyable; a user is one shared object owned by SignedInUsers.
  PsUser(const PsUser&) = delete;
  /// @brief Not copy-assignable; a user is one shared object owned by
  /// SignedInUsers.
  auto operator=(const PsUser&) -> PsUser& = delete;
  /// @brief Releases the user and wipes the secrets it holds. The
  /// CredentialStore is left untouched.
  ~PsUser();

  /// @brief The account's number, which identifies the user and never
  /// changes.
  ///
  /// @return The account number.
  auto account_id() const noexcept -> AccountId;
  /// @brief The account's online id, or the name a home user was added with.
  ///
  /// @return The online id, which lives as long as this user; never changes
  /// after creation.
  auto online_id() const noexcept -> const std::string&;
  /// @brief Address of the user's avatar image.
  ///
  /// @return The address from the last profile fetched, or nothing before
  ///     get_my_profile() has succeeded or when the profile has no avatar.
  auto avatar_url() const -> std::optional<std::string>;
  /// @brief What is currently known about the user's PlayStation Plus
  /// membership, without asking PSN.
  ///
  /// The last answer from get_my_membership() is used while it is remembered.
  /// Failing that, the membership is inferred from the last fetched profile,
  /// which can say that the user is a member but not the tier. Otherwise
  /// nothing is known.
  ///
  /// @return The best current knowledge, which may state nothing about the tier
  /// or whether the user is a member.
  auto membership() const -> Membership;
  /// @brief The account's storefront region.
  ///
  /// @return The region, or nothing until the library has learned it.
  auto region() const -> std::optional<Region>;
  /// @brief What the user's app token states about the account's country,
  /// locale and age. States nothing for a home user.
  ///
  /// @return The facts stated by the token; none when no token states them.
  auto account_context() const -> AccountContext;

  /// @brief How the user currently stands with PSN, judged from what is held.
  /// Nothing is sent.
  ///
  /// @return The state.
  auto sign_in_state() const -> SignInState;
  /// @brief Whether the sign-in is accepted: the state is
  /// SignInState::signed_in or SignInState::store_access_lapsed.
  ///
  /// @return True for SignInState::signed_in or
  /// SignInState::store_access_lapsed, false otherwise.
  auto is_signed_in() const -> bool;
  /// @brief Whether PSN has refused the sign-in, so that the user must sign
  /// in again.
  ///
  /// @return True when PSN has refused the sign-in, false otherwise.
  auto is_expired() const -> bool;
  /// @brief Whether the user's current state is in the CredentialStore: it
  /// was loaded from there or the last save succeeded.
  ///
  /// @return True when the CredentialStore holds the current state, false
  /// otherwise.
  auto is_saved() const noexcept -> bool;
  /// @brief Whether the user was created from a PSN sign-in.
  ///
  /// False for a home user. It stays true after the user is signed out; use
  /// sign_in_state() to tell whether the sign-in is still usable.
  ///
  /// @return True for a user created from a PSN sign-in, false for a home user.
  auto has_psn_account() const noexcept -> bool;
  /// @brief Whether the user cannot ask PSN anything: a home user, or a PSN
  /// user who has been signed out.
  ///
  /// Equivalent to sign_in_state() returning SignInState::signed_out, but
  /// reads no token. Safe to call from any thread; never blocks.
  ///
  /// @return True when no PSN sign-in is usable at all.
  auto is_off_psn() const noexcept -> bool;

  /// @brief Fetches the user's PSN profile.
  ///
  /// A profile fetched within profile_lifetime is returned without asking
  /// PSN. Otherwise the app token is renewed first if it has lapsed, and the
  /// profile is fetched. Concurrent calls wait for one another, so only one
  /// request is made.
  ///
  /// @param stop Cancels the request.
  /// @return The profile.
  /// @retval Unanswered::Reason::signed_out The user has no PSN sign-in.
  /// @retval Unanswered::Reason::sign_in_expired PSN has refused the sign-in.
  /// @retval Unanswered::Reason::store_access_lapsed PSN refused the app
  ///     token.
  /// @retval Unanswered::Reason::cancelled The stop token was triggered.
  /// @retval Unanswered::Reason::rate_limited PSN is rate limiting.
  /// @retval Unanswered::Reason::rejected PSN rejected the request.
  /// @retval Unanswered::Reason::malformed PSN's reply could not be read.
  /// @retval Unanswered::Reason::unavailable The request did not complete or
  ///     PSN answered with an unexpected status.
  [[nodiscard]] auto get_my_profile(const platform::StopToken& stop = {})
      -> Expected<Profile, Unanswered>;
  /// @brief Finds out the user's PlayStation Plus tier.
  ///
  /// The answer may come from the library's cache; MembershipAnswer::cached
  /// says so. Concurrent calls share one request. When PSN cannot be asked
  /// but the last profile showed the user to be a member, that weaker answer
  /// is returned instead of an error, with the tier unknown.
  ///
  /// @param stop Cancels the request.
  /// @return The membership and when it was learned.
  /// @retval Unanswered::Reason::signed_out The user has no PSN sign-in.
  /// @retval Unanswered::Reason::cancelled The stop token was triggered.
  /// @retval Unanswered::Reason::store_access_lapsed The app token is
  ///     missing, expired or was refused.
  /// @retval Unanswered::Reason::rejected PSN rejected the request.
  /// @retval Unanswered::Reason::malformed PSN's reply could not be read.
  /// @retval Unanswered::Reason::unavailable The request did not complete or
  ///     PSN answered with an unexpected status.
  [[nodiscard]] auto get_my_membership(const platform::StopToken& stop = {})
      -> Expected<MembershipAnswer, Unanswered>;
  /// @brief Renews whichever of the user's tokens have lapsed, so the user
  /// stays signed in without interaction.
  ///
  /// The app token is renewed when it is missing or expired, with its refresh
  /// token or, failing that, the NPSSO. The Remote Play token is renewed when
  /// a refresh token is held and the token is missing or within
  /// remote_play_margin of expiry. If anything was renewed the user is saved
  /// to the CredentialStore.
  ///
  /// @param stop Cancels the requests.
  /// @return The user's state after renewal.
  /// @retval Unanswered::Reason::signed_out The user has no PSN sign-in.
  /// @retval Unanswered::Reason::sign_in_expired PSN refused to issue a new
  ///     app token for the refresh token and for the NPSSO. The state becomes
  ///     SignInState::expired.
  /// @retval Unanswered::Reason::remote_play_access_missing PSN refused the
  ///     Remote Play refresh token. Both Remote Play tokens are discarded.
  /// @retval Unanswered::Reason::cancelled The stop token was triggered.
  /// @retval Unanswered::Reason::rate_limited PSN is rate limiting.
  /// @retval Unanswered::Reason::rejected A request was not usable.
  /// @retval Unanswered::Reason::malformed PSN's reply could not be read.
  /// @retval Unanswered::Reason::unavailable A request did not complete or
  ///     PSN answered with an unexpected status.
  [[nodiscard]] auto stay_signed_in(const platform::StopToken& stop = {})
      -> Expected<SignInState, Unanswered>;

  /// @brief Returns a JSON document describing the user's PSN session for
  /// diagnostics: token standing, request counts and the held membership.
  ///
  /// @return The document as compact JSON text; nothing when it could not be
  ///     written.
  [[nodiscard]] auto snapshot_json() -> std::optional<std::string>;

  /// @brief Describes every credential the user holds or could hold, without
  /// revealing any.
  ///
  /// @return One entry per CredentialKind, in the order of every_credential.
  ///     For a user with no PSN sign-in every entry reports the credential as
  ///     not held.
  /// @see reveal
  auto held_credentials() const -> std::vector<HeldCredential>;
  /// @brief Hands out a copy of one of the user's secrets.
  ///
  /// @warning This is the one member that lets a secret leave the user. Keep
  ///     the result in the SecretString it arrives in, which wipes itself,
  ///     and never log it.
  ///
  /// @param kind Which credential to reveal.
  /// @return A copy of the secret in wiping memory.
  /// @retval NotRevealed::Reason::signed_out The user has no PSN sign-in.
  /// @retval NotRevealed::Reason::not_held The user does not hold that
  ///     credential.
  [[nodiscard]] auto reveal(CredentialKind kind) const
      -> Expected<SecretString, NotRevealed>;

  /// @brief The user's internal state. Defined inside the library; not for
  /// frontends.
  class Core;

  /// @brief Wraps internal state in a user. Used by SignedInUsers; frontends
  /// do not construct users.
  ///
  /// @param core The state to own. Must not be null.
  explicit PsUser(std::unique_ptr<Core> core);

 private:
  std::unique_ptr<Core> core_;
};

}  // namespace libchiaking::users
