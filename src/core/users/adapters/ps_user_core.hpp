/// @file
/// @brief Defines PsUser::Core, the internal state of one user behind the
/// public PsUser facade: identity, secrets, renewal and what has been learned
/// from PSN, and InFlight, the questions to PSN it is asking right now.
///
/// The public class forwards to this one. The rest of the library reaches it
/// through PsUser::Core::of, mostly by way of the ports under
/// `src/core/users/ports`, to borrow the user's tokens and to tell the user
/// what PS said about them.
#pragma once

#include "libchiaking/detail/users/device_id.hpp"
#include "libchiaking/detail/users/membership.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/auth/token_cache.hpp"
#include "src/core/psn/rest/cloud_assisted_navigation/cloud_assisted_navigation.hpp"
#include "src/core/psn/rest/commerce/commerce_client.hpp"
#include "src/core/psn/rest/kamaji/kamaji_client.hpp"
#include "src/core/psn/session.hpp"
#include "src/core/psn/types.hpp"
#include "src/core/users/adapters/authorisation_refusal.hpp"
#include "src/core/users/cloud_streaming.hpp"
#include "src/core/utils/concurrency/shared_flight.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::psn::account {
struct Failure;
}

namespace libchiaking::users {

class CloudRelaunch;
class CloudStreamingCache;

/// @brief One concurrency::SharedFlight per kind of question the users ports
/// ask PSN on a user's behalf.
///
/// When a thread asks a question that another thread is already asking for the
/// same user, it waits for that request and receives the same reply instead of
/// sending its own. Nothing is remembered once a request lands: the next
/// caller sends a fresh one. A waiter whose own stop token fires gives up with
/// a cancellation; a request whose sender was cancelled is not shared, and its
/// waiters ask again. Each flight distinguishes questions by a key that
/// the ports build from the arguments, so different questions of one
/// kind run side by side.
///
/// Owned by PsUser::Core for the user's whole life and reached through
/// PsUser::Core::in_flight. Every flight is safe to use from several threads.
struct InFlight {
  /// @brief Makes every flight with nothing in the air.
  ///
  /// @param threading Provider of each flight's mutex and condition
  ///     variable; must outlive the flights.
  /// @param clock Monotonic clock the flights' waits measure on; must
  ///     outlive the flights.
  InFlight(platform::ThreadingProvider& threading,
           const platform::ClockProvider& clock)
      : account_consoles(threading, clock),
        cloud_device_check(threading, clock),
        old_service_store(threading, clock),
        library_records(threading, clock),
        library_record(threading, clock),
        membership(threading, clock),
        cloud_streaming(threading, clock) {}

  /// @brief Pages of the consoles registered to the account, keyed by
  /// response size limit and page offset.
  concurrency::SharedFlight<psn::rest::ClientsPage, Unanswered>
      account_consoles;
  /// @brief Checks that PSN lets this device stream from the cloud, keyed by
  /// streaming service.
  concurrency::SharedFlight<void, AuthorisationRefusal> cloud_device_check;
  /// @brief Sessions opened with the older LegacyCloudStream store service,
  /// keyed by response size limit.
  concurrency::SharedFlight<std::shared_ptr<const psn::rest::KamajiClient>,
                            Unanswered>
      old_service_store;
  /// @brief The account's whole list of owned entitlements, keyed by
  /// response size limit.
  concurrency::SharedFlight<psn::rest::Entitlements, Unanswered>
      library_records;
  /// @brief Single owned-entitlement lookups, keyed by response size limit
  /// and entitlement id.
  concurrency::SharedFlight<psn::rest::EntitlementLookup, Unanswered>
      library_record;
  /// @brief The PlayStation Plus membership, keyed by the lowest tier the
  /// caller will accept from memory.
  concurrency::SharedFlight<MembershipAnswer, Unanswered> membership;
  /// @brief The verdict on whether the account may stream from the cloud;
  /// one key for the whole user.
  concurrency::SharedFlight<CloudStreaming, Unanswered> cloud_streaming;
};

/// @brief One user's identity and credentials, the tokens renewed from them,
/// and what PSN has said about the account.
///
/// Two kinds of user share this class. A PSN user was signed in with an NPSSO:
/// it has a device id and a psn::Session holding the NPSSO, the app token and
/// the in-memory token cache, plus the app refresh token and both Remote Play
/// tokens kept here. A home user was added by account id and name only: it has
/// no device id and an empty session, and is off PSN from the start.
///
/// Every user is built by admit(), which validates a saved row, and is owned by
/// one PsUser, itself shared out by the UserRoster. When the roster lets the
/// user go, leave() wipes every secret and the user is off PSN for good,
/// although the object lives on for whoever still holds it. The providers in
/// Providers are referenced, not owned, and must outlive the user.
///
/// Every public member is safe to call from several threads. mutex_ guards the
/// learned facts and the tokens kept here and is only held briefly, never
/// across a request. renewal_ serialises renewals, so concurrent callers that
/// find a lapsed token renew it once; profile_flight_ does the same for
/// profile fetches. The session and the caches guard themselves.
class PsUser::Core {
 public:
  /// @brief Reaches the state behind a user, for the library's own use.
  ///
  /// @param user The user.
  /// @return Its state, which lives as long as the user.
  [[nodiscard]] static auto of(const PsUser& user) noexcept -> Core& {
    return *user.core_;
  }

  /// @brief The registry-wide services every user is built with, all
  /// referenced and owned elsewhere.
  struct Providers {
    /// @brief Carries every PSN request made for the user.
    platform::HttpProvider& http;
    /// @brief Where the user is saved after each change worth keeping.
    CredentialStore& credentials;
    /// @brief Makes the user's locks and the waits on shared requests.
    platform::ThreadingProvider& threading;
    /// @brief Monotonic clock for cache and profile lifetimes.
    const platform::ClockProvider& clock;
    /// @brief Wall clock for token issue and expiry times.
    const platform::ClockProvider& wall;
    /// @brief Generates a device id for a saved sign-in that has none.
    platform::SecureRandomProvider& random;
    /// @brief Receives diagnostics; may be null.
    platform::DiagnosticsProvider* log;
    /// @brief The registry's view of PSN's clock.
    ServerTime& server_time;
  };

  /// @brief Builds a PSN user from parts admit() has validated.
  ///
  /// @param providers The registry's services; what they refer to must
  ///     outlive the user.
  /// @param account The account's number; never zero.
  /// @param online_id The account's online id; already validated.
  /// @param device The device id the user signs in as.
  /// @param session The session holding the NPSSO and, if one was saved,
  ///     the app token.
  Core(const Providers& providers, AccountId account, std::string online_id,
       DeviceId device, std::unique_ptr<psn::Session> session);
  /// @brief Builds a home user, with no device id and an empty session.
  ///
  /// @param providers The registry's services; what they refer to must
  ///     outlive the user.
  /// @param account The account's number; never zero.
  /// @param name The name to show; already validated.
  Core(const Providers& providers, AccountId account, std::string name);
  /// @brief Not copyable; a user's secrets exist once.
  Core(const Core&) = delete;
  /// @brief Not copy-assignable; a user's secrets exist once.
  auto operator=(const Core&) -> Core& = delete;
  /// @brief Releases the user; the session and the tokens kept here wipe
  /// their secrets as they go. The store is left untouched.
  ~Core();

  /// @brief Validates a saved PSN sign-in and builds the user from it.
  ///
  /// Checks, in order, the account id, the online id, the NPSSO, the region
  /// when saved, the app tokens and the Remote Play tokens. A row with no
  /// device id is given a freshly generated one. The saved app token is
  /// handed to the session, and the account context it states is adopted.
  /// Nothing is sent and nothing is saved.
  ///
  /// @param providers The registry's services.
  /// @param saved The row; consumed.
  /// @return The user, not yet held by any roster.
  /// @retval Invalid::account_id The account id is zero.
  /// @retval Invalid::online_id The online id is empty, longer than
  ///     online_id_limit or holds a control character.
  /// @retval Invalid::npsso The session refuses the NPSSO.
  /// @retval Invalid::region A saved region's country or language is empty,
  ///     longer than region_text_limit or holds other than letters, digits
  ///     and hyphens.
  /// @retval Invalid::store_access The app token or its refresh token is
  ///     empty, or the session refused the app token.
  /// @retval Invalid::remote_play_access The Remote Play token is empty or
  ///     longer than psn::account::Client::token_limit, or its refresh token
  ///     is empty.
  /// @retval Invalid::device_id No device id was saved and the random source
  ///     could not generate one.
  [[nodiscard]] static auto admit(const Providers& providers, SavedSignIn saved)
      -> Expected<std::shared_ptr<PsUser>, Invalid>;
  /// @brief Validates a saved home user and builds the user from it.
  ///
  /// @param providers The registry's services.
  /// @param saved The row; consumed.
  /// @return The user, not yet held by any roster.
  /// @retval Invalid::account_id The account id is zero.
  /// @retval Invalid::online_id The name is empty, longer than
  ///     online_id_limit or holds a control character.
  [[nodiscard]] static auto admit(const Providers& providers,
                                  SavedHomeUser saved)
      -> Expected<std::shared_ptr<PsUser>, Invalid>;
  /// @brief Translates a failure of the account service into the reason a
  /// question went unanswered.
  ///
  /// @param failure The account service's failure.
  /// @return cancelled, rate_limited, malformed or rejected (for an invalid
  ///     argument) as the failure says; unavailable for an auth failure or an
  ///     unavailable service. The HTTP status is carried over.
  [[nodiscard]] static auto unanswered(
      const psn::account::Failure& failure) noexcept -> Unanswered;
  /// @brief Translates a failed sign-in or renewal exchange into the reason a
  /// question went unanswered.
  ///
  /// @param error The exchange's failure.
  /// @return sign_in_expired when PSN rejected the credential; cancelled,
  ///     rate_limited, malformed or rejected (for an invalid request) as the
  ///     error says; unavailable for a transport failure or an unavailable
  ///     service. The HTTP status is carried over.
  [[nodiscard]] static auto unanswered(
      const psn::auth::SignInError& error) noexcept -> Unanswered;

  /// @brief The account's number, which never changes.
  ///
  /// @return The account number.
  auto account_id() const noexcept -> AccountId { return account_; }
  /// @brief The online id, or a home user's name.
  ///
  /// @return The text, which never changes and lives as long as the user.
  auto online_id() const noexcept -> const std::string& { return online_id_; }
  /// @brief Whether the store holds the user's current state.
  ///
  /// @return True when the user was loaded from the store or the last save()
  ///     succeeded.
  auto is_saved() const noexcept -> bool { return saved_; }
  /// @brief Whether the user was created from a PSN sign-in.
  ///
  /// @return True for a PSN user, also after it leaves; false for a home
  ///     user.
  auto has_psn_account() const noexcept -> bool { return device_.has_value(); }
  /// @brief Whether nothing may be asked of PSN for this user.
  ///
  /// Reads no token and takes no lock.
  ///
  /// @return True for a home user and for a user that has left the registry.
  auto off_psn() const noexcept -> bool { return !device_ || signed_out_; }
  /// @brief The session holding the NPSSO, the app token and the in-memory
  /// token cache.
  ///
  /// @return The session, which lives as long as the user; empty for a home
  ///     user and after leave().
  auto session() const noexcept -> psn::Session& { return *session_; }
  /// @brief The user's memory of its PlayStation Plus membership.
  ///
  /// @return The cache, which lives as long as the user.
  auto memberships() const noexcept -> MembershipCache& {
    return *memberships_;
  }
  /// @brief The user's table of questions being asked of PSN right now.
  ///
  /// @return The table, which lives as long as the user.
  auto in_flight() const noexcept -> InFlight& { return *in_flight_; }
  /// @brief The user's memory of its cloud streaming verdict.
  ///
  /// @return The cache, which lives as long as the user.
  auto cloud_streaming() const noexcept -> CloudStreamingCache& {
    return *cloud_streaming_;
  }
  /// @brief The user's memory of when its last cloud stream ended.
  ///
  /// @return The memory, which lives as long as the user.
  auto cloud_relaunch() const noexcept -> CloudRelaunch& {
    return *cloud_relaunch_;
  }
  /// @brief The device id the user signs in and streams as. Only for a PSN
  /// user: calling it for a home user is undefined.
  ///
  /// @return The device id, which lives as long as the user.
  auto device() const noexcept -> const DeviceId& { return *device_; }
  /// @brief The wall clock the user's token times are reckoned in.
  ///
  /// @return The registry's wall clock.
  auto wall() const noexcept -> const platform::ClockProvider& { return wall_; }
  /// @brief The registry's view of PSN's clock.
  ///
  /// @return The clock, shared by every user of the registry.
  auto server_time() const noexcept -> ServerTime& { return server_time_; }

  /// @brief Address of the user's avatar image.
  ///
  /// @return The address from the last profile fetched, however old, or
  ///     nothing before a profile has been fetched, when it had no avatar, or
  ///     after leave().
  auto avatar_url() const -> std::optional<std::string>;
  /// @brief What is known about the user's PlayStation Plus membership,
  /// without asking PSN.
  ///
  /// @return The membership remembered by memberships() while it is fresh;
  ///     otherwise inferred_membership().
  auto membership() const -> Membership;
  /// @brief The membership as far as the last fetched profile tells it.
  ///
  /// @return A member of unknown tier when the profile said the user has
  ///     PlayStation Plus; Tier::none when it said they have not; a
  ///     membership stating nothing when no profile is held or it did not
  ///     say.
  auto inferred_membership() const -> Membership;
  /// @brief The account's storefront region.
  ///
  /// @return The region saved or learned from the LegacyCloudStream store, or
  /// nothing
  ///     until one is known.
  auto region() const -> std::optional<Region>;
  /// @brief What the app token last stated about the account's country,
  /// locale and age.
  ///
  /// @return The context adopted by learn_context(); one stating nothing for
  ///     a home user, before any token stated it, and after leave().
  auto account_context() const -> AccountContext;
  /// @brief How the user stands with PSN, judged from what is held. Nothing
  /// is sent.
  ///
  /// @return SignInState::signed_out when off PSN; SignInState::expired when
  ///     PSN refused the sign-in, whether through refuse_sign_in() or the
  ///     session's token cache; SignInState::store_access_lapsed when no
  ///     usable app token is held; SignInState::signed_in otherwise.
  auto sign_in_state() const -> SignInState;
  /// @brief Whether PSN still accepts the sign-in.
  ///
  /// @return True for SignInState::signed_in and
  ///     SignInState::store_access_lapsed.
  auto is_signed_in() const -> bool;
  /// @brief Whether PSN has refused the sign-in.
  ///
  /// @return True for SignInState::expired.
  auto is_expired() const -> bool;

  /// @brief Returns the user's profile, fetching it when the held one is
  /// older than profile_lifetime.
  ///
  /// Concurrent calls are serialised, so only one request is made. A fetch
  /// first restores the app token, unless the sign-in is expired, in which
  /// case an app token still held is used as it is. A fetched profile is held
  /// and also updates inferred_membership().
  ///
  /// Blocks for the renewal and the profile request when one is needed, and
  /// while another thread's fetch runs.
  ///
  /// @param stop Cancels the requests.
  /// @return The profile.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::sign_in_expired PSN refused the sign-in and
  ///     no app token is held, or a renewal was refused.
  /// @retval Unanswered::Reason::store_access_lapsed PSN refused the app
  ///     token; the refusal is recorded against it.
  /// @retval Unanswered::Reason The renewal or the request failed otherwise;
  ///     see unanswered().
  [[nodiscard]] auto get_my_profile(const platform::StopToken& stop)
      -> Expected<Profile, Unanswered>;
  /// @brief Finds out the user's PlayStation Plus tier; see
  /// MembershipCalls::answered_membership.
  ///
  /// Blocks for the request when nothing fresh is held.
  ///
  /// @param stop Cancels the request.
  /// @return The membership and when it was learned.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason Any failure answered_membership() reports.
  [[nodiscard]] auto get_my_membership(const platform::StopToken& stop)
      -> Expected<MembershipAnswer, Unanswered>;
  /// @brief Renews whichever tokens have lapsed and saves the user if
  /// anything was renewed.
  ///
  /// The app token is renewed when no usable one is held. The Remote Play
  /// token is renewed when a refresh token is held and the token is missing
  /// or within remote_play_margin of expiry. Serialised with every other
  /// renewal of this user.
  ///
  /// Blocks for up to three exchanges (refresh, NPSSO fallback, Remote Play)
  /// and the store write.
  ///
  /// @param stop Cancels the exchanges.
  /// @return The state after renewal.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::sign_in_expired PSN refused both the app
  ///     refresh token and the NPSSO; the sign-in is now expired.
  /// @retval Unanswered::Reason::remote_play_access_missing PSN refused the
  ///     Remote Play refresh token; both Remote Play tokens are dropped.
  /// @retval Unanswered::Reason Any other failure of an exchange; see
  ///     unanswered().
  [[nodiscard]] auto stay_signed_in(const platform::StopToken& stop)
      -> Expected<SignInState, Unanswered>;
  /// @brief Describes the user's session for diagnostics: token standing,
  /// request counts and the held membership.
  ///
  /// @return Compact JSON text, the membership `null` when none is held;
  ///     nothing when the document could not be written.
  [[nodiscard]] auto snapshot_json() -> std::optional<std::string>;
  /// @brief Describes every credential the user can hold, without revealing
  /// any secret.
  ///
  /// @return One entry per CredentialKind: the sign-in seed, app token, app
  ///     refresh token, Remote Play token, Remote Play refresh token, library
  ///     token and LegacyCloudStream checkout token, in that order. Off PSN,
  ///     every entry says not held.
  auto held_credentials() const -> std::vector<HeldCredential>;
  /// @brief Copies one credential's secret, for a developer to inspect.
  ///
  /// @param kind Which credential.
  /// @return The secret, in wiping memory.
  /// @retval NotRevealed::Reason::signed_out The user is off PSN.
  /// @retval NotRevealed::Reason::not_held The credential is not held or is
  ///     empty.
  [[nodiscard]] auto reveal(CredentialKind kind) const
      -> Expected<SecretString, NotRevealed>;

  /// @brief Copies the Remote Play token while it is usable.
  ///
  /// Nothing is sent and nothing is renewed.
  ///
  /// @return The token, or nothing when none is held or it is within
  ///     remote_play_margin of expiry by the wall clock.
  [[nodiscard]] auto remote_play_token() const
      -> std::optional<psn::auth::BearerToken>;
  /// @brief Makes sure a usable app token is held, renewing it and saving
  /// the user if not.
  ///
  /// Serialised with every other renewal of this user. Blocks for up to two
  /// exchanges and the store write when a renewal is needed.
  ///
  /// @param stop Cancels the exchanges.
  /// @return Nothing once a usable app token is held.
  /// @retval Unanswered::Reason::sign_in_expired PSN refused the refresh
  ///     token and the NPSSO.
  /// @retval Unanswered::Reason Any other failure of the exchange; see
  ///     unanswered().
  [[nodiscard]] auto restore_store_access(const platform::StopToken& stop)
      -> Expected<void, Unanswered>;
  /// @brief Returns a usable app token, renewing it first if needed.
  ///
  /// Blocks as restore_store_access() does.
  ///
  /// @param stop Cancels the exchanges.
  /// @return A copy of the app token.
  /// @retval Unanswered::Reason::store_access_lapsed The renewal succeeded
  ///     but no usable token could be taken from the session.
  /// @retval Unanswered::Reason Any failure restore_store_access() reports.
  [[nodiscard]] auto restored_app_token(const platform::StopToken& stop)
      -> Expected<psn::auth::AppToken, Unanswered>;
  /// @brief Returns the app token, renewing it only while PSN still accepts
  /// the sign-in.
  ///
  /// Once the sign-in is expired no renewal is tried: a still-usable token
  /// is returned as it is, so calls that work without a fresh sign-in keep
  /// working until it lapses.
  ///
  /// Blocks as restore_store_access() does, and not at all once the sign-in
  /// is expired.
  ///
  /// @param stop Cancels the exchanges.
  /// @return A copy of the app token.
  /// @retval Unanswered::Reason::sign_in_expired The sign-in is expired and
  ///     no usable app token is held.
  /// @retval Unanswered::Reason Any failure restored_app_token() reports.
  [[nodiscard]] auto app_token_unless_refused(const platform::StopToken& stop)
      -> Expected<psn::auth::AppToken, Unanswered>;
  /// @brief Makes sure a usable Remote Play token is held, renewing it and
  /// saving the user if not.
  ///
  /// Serialised with every other renewal of this user. Blocks for one
  /// exchange and the store write when a renewal is needed.
  ///
  /// @param stop Cancels the exchange.
  /// @return Nothing once a usable Remote Play token is held.
  /// @retval Unanswered::Reason::remote_play_access_missing No Remote Play
  ///     refresh token is held, or PSN refused it and both Remote Play tokens
  ///     were dropped.
  /// @retval Unanswered::Reason Any other failure of the exchange; see
  ///     unanswered().
  [[nodiscard]] auto restore_remote_play_access(const platform::StopToken& stop)
      -> Expected<void, Unanswered>;
  /// @brief Drops the Remote Play token after PS refused it, so the next
  /// restore_remote_play_access() renews it.
  ///
  /// A token that has already been replaced is left alone. Not saved.
  ///
  /// @param refused The token PS refused.
  void distrust_remote_play_access(const psn::auth::BearerToken& refused);
  /// @brief Writes the user's current state to the store and records whether
  /// that worked in is_saved().
  ///
  /// A PSN user is saved as a SavedSignIn with every token held; a home user
  /// as a SavedHomeUser. Does nothing once the user has left. Blocks on the
  /// store write.
  void save();
  /// @brief Marks the user as matching the store without writing, for a user
  /// just loaded from it.
  void kept_already() noexcept { saved_ = true; }
  /// @brief Records that PSN refused the sign-in, so the user reports
  /// SignInState::expired until an app token is renewed from it again.
  void refuse_sign_in();
  /// @brief Adopts the region a LegacyCloudStream store session reported,
  /// saving the user when it changed.
  ///
  /// An invalid region is ignored. Blocks on the store write when the region
  /// changed.
  ///
  /// @param region The country and language PS reported.
  void learn_region(Region region);
  /// @brief Adopts the account context an app token stated, when it states
  /// country, locale and age together; anything less is ignored.
  ///
  /// Takes no lock: the caller holds mutex_, or owns the user exclusively as
  /// admit() does.
  ///
  /// @param stated The context read from a token.
  void learn_context(const AccountContext& stated);
  /// @brief Takes the user off PSN for good and wipes everything secret or
  /// learned.
  ///
  /// The session, both caches, the profile, the account context, the tokens
  /// kept here and their times are cleared; the identity and region stay.
  /// Called by the roster when the user is replaced, forgotten or the
  /// registry is destroyed. The store is left untouched.
  void leave();

 private:
  /// @brief The parts of the last fetched profile that are not identity.
  struct KnownProfile {
    /// @brief Address of the avatar image, if the profile had one.
    std::optional<std::string> avatar_url;
    /// @brief What the profile said about PlayStation Plus.
    psn::PlusStatus plus{psn::PlusStatus::unknown};
    /// @brief When it was fetched, in microseconds on the monotonic clock.
    std::uint64_t learned_us{};
  };

  /// @brief Whether an online id or home user name is acceptable.
  ///
  /// @param online_id The text to check.
  /// @return True when it is not empty, at most online_id_limit bytes and has
  ///     no control character below 0x20.
  static auto valid_online_id(std::string_view online_id) noexcept -> bool;
  /// @brief Whether a region is acceptable.
  ///
  /// @param region The region to check.
  /// @return True when its country and language are each 1 to
  ///     region_text_limit ASCII letters, digits or hyphens.
  static auto valid_region(const Region& region) noexcept -> bool;

  /// @brief Reads the wall clock.
  ///
  /// @return The wall clock's current time.
  auto wall_now() const noexcept -> std::chrono::system_clock::time_point;
  /// @brief The held profile while it is younger than profile_lifetime.
  ///
  /// @return The profile with the current membership(), or nothing when none
  ///     is held or it is too old.
  [[nodiscard]] auto fresh_profile() const -> std::optional<Profile>;
  /// @brief Obtains a new app token. The caller holds renewal_.
  ///
  /// Exchanges the NPSSO through the ``sso_token`` grant, which gives an app
  /// token with no refresh token. An app refresh token kept from the older
  /// grant is used only when PSN rejects the NPSSO, and an NPSSO exchange drops
  /// it, so an upgraded sign-in moves to the NPSSO-issued token at its next
  /// renewal. On success the new token goes to the session, any new refresh
  /// token is kept, the times are stamped, the account context is learned
  /// and any earlier refusal of the sign-in is forgotten. Not saved.
  ///
  /// Blocks for one or two exchanges.
  ///
  /// @param stop Cancels the exchanges.
  /// @return Nothing once the token is held.
  /// @retval Unanswered::Reason::sign_in_expired PSN rejected the NPSSO and
  ///     any kept refresh token; the sign-in is marked refused and the app
  ///     refresh token dropped.
  /// @retval Unanswered::Reason Any other failure; see unanswered().
  [[nodiscard]] auto renew_store_access(const platform::StopToken& stop)
      -> Expected<void, Unanswered>;
  /// @brief Obtains a new Remote Play token from its refresh token. The
  /// caller holds renewal_.
  ///
  /// On success both Remote Play tokens are replaced and stamped. Not saved.
  /// Blocks for one exchange.
  ///
  /// @param stop Cancels the exchange.
  /// @return Nothing once the token is held.
  /// @retval Unanswered::Reason::remote_play_access_missing No refresh token
  ///     is held (nothing is sent), or PSN rejected it and both Remote Play
  ///     tokens were dropped.
  /// @retval Unanswered::Reason Any other failure; see unanswered().
  [[nodiscard]] auto renew_remote_play_access(const platform::StopToken& stop)
      -> Expected<void, Unanswered>;
  /// @brief Gathers the user's current state as the row save() writes.
  ///
  /// @return A saved sign-in with copies of the NPSSO, the device id, every
  ///     token held with its times, the account context and the region.
  auto saved_sign_in() const -> SavedSignIn;
  /// @brief The purposes a credential is spent on.
  ///
  /// @param kind Which credential.
  /// @return Every CredentialUse that kind serves; never empty.
  static auto uses_of(CredentialKind kind) -> std::vector<CredentialUse>;
  /// @brief Describes a credential held only in the session's token cache
  /// from the cache's own statistics.
  ///
  /// @param kind Which credential.
  /// @param stats The cache's statistics, or nothing when it never saw one.
  /// @return The description, kept in memory; refused when the cache saw it
  ///     invalidated or its last exchange refused, otherwise usable, expired
  ///     or never obtained as the cache says. The last reply status prefers
  ///     the last use over the last exchange.
  static auto exchanged(CredentialKind kind,
                        const std::optional<psn::auth::TokenStats>& stats)
      -> HeldCredential;
  /// @brief Converts a cache timestamp to a credential time.
  ///
  /// @param unix_ms Milliseconds since the Unix epoch, if known.
  /// @return The time, or nothing when unknown.
  static auto time_at(const std::optional<std::int64_t>& unix_ms)
      -> std::optional<HeldCredential::Time>;
  /// @brief Describes a credential kept with the saved sign-in, which PSN
  /// has not confirmed since.
  ///
  /// @param kind Which credential.
  /// @param held Whether it is held.
  /// @param obtained_at When it was obtained, if known.
  /// @param expires_at When it expires, if known.
  /// @return The description, kept with the user; expired when past its
  ///     expiry by the wall clock, unverified otherwise, and bare when not
  ///     held.
  auto lasting(CredentialKind kind, bool held,
               const std::optional<HeldCredential::Time>& obtained_at,
               const std::optional<HeldCredential::Time>& expires_at) const
      -> HeldCredential;

  /// @brief The account's number; never zero, never changes.
  AccountId account_;
  /// @brief The online id or home user name; never changes.
  std::string online_id_;
  /// @brief The device id; set for every PSN user, empty for a home user.
  std::optional<DeviceId> device_;
  /// @brief Carries every PSN request made for the user.
  platform::HttpProvider& http_;
  /// @brief Where save() writes.
  CredentialStore& credentials_;
  /// @brief Monotonic clock for profile and cache lifetimes.
  const platform::ClockProvider& clock_;
  /// @brief Wall clock for token times.
  const platform::ClockProvider& wall_;
  /// @brief Receives diagnostics; may be null.
  platform::DiagnosticsProvider* log_;
  /// @brief The registry's view of PSN's clock.
  ServerTime& server_time_;
  /// @brief The NPSSO, the app token and the in-memory token cache; never
  /// null.
  std::unique_ptr<psn::Session> session_;
  /// @brief The remembered membership; never null.
  std::unique_ptr<MembershipCache> memberships_;
  /// @brief Questions being asked of PSN right now; never null.
  std::unique_ptr<InFlight> in_flight_;
  /// @brief The remembered cloud streaming verdict; never null.
  std::unique_ptr<CloudStreamingCache> cloud_streaming_;
  /// @brief When the last cloud stream ended; never null.
  std::unique_ptr<CloudRelaunch> cloud_relaunch_;

  /// @brief Guards every member from profile_ to sign_in_refused_.
  mutable platform::Mutex mutex_;
  /// @brief The last fetched profile; empty until one is fetched and after
  /// leave().
  std::optional<KnownProfile> profile_;
  /// @brief The storefront region, saved or learned; empty until known.
  std::optional<Region> region_;
  /// @brief What an app token last stated about the account's audience.
  AccountContext context_;
  /// @brief When the NPSSO was obtained, by the wall clock, if known.
  std::optional<HeldCredential::Time> seed_obtained_at_;
  /// @brief When the NPSSO stops being accepted, if the application said.
  std::optional<HeldCredential::Time> seed_expires_at_;
  /// @brief When the app token expires, by the wall clock; the epoch while
  /// none has been held.
  std::chrono::system_clock::time_point store_expires_at_{};
  /// @brief When the app token was obtained, by the wall clock, if known.
  std::optional<HeldCredential::Time> store_obtained_at_;
  /// @brief The app refresh token and its times; empty when none is held or
  /// PSN rejected the NPSSO.
  std::optional<SavedSignIn::StoreRefresh> store_refresh_;
  /// @brief The Remote Play token and its times; empty when none is held or
  /// PS refused it.
  std::optional<SavedSignIn::RemotePlayAccess> remote_play_;
  /// @brief The Remote Play refresh token and its times; empty when none is
  /// held or PSN rejected it.
  std::optional<SavedSignIn::RemotePlayRefresh> remote_play_refresh_;
  /// @brief Set when PSN refused the sign-in; cleared when an app token is
  /// next issued.
  bool sign_in_refused_{};

  /// @brief Serialises profile fetches.
  platform::Mutex profile_flight_;
  /// @brief Serialises token renewals.
  platform::Mutex renewal_;
  /// @brief Set once by leave(); never cleared.
  std::atomic<bool> signed_out_{};
  /// @brief Whether the store holds the current state.
  std::atomic<bool> saved_{};
};

}  // namespace libchiaking::users
