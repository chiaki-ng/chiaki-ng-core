/// @file
/// @brief Declares SignedInUsers, the registry that signs users in and out and
/// remembers them.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/users/account_id.hpp"
#include "libchiaking/detail/users/credential_store.hpp"
#include "libchiaking/detail/users/credentials.hpp"
#include "libchiaking/detail/users/device_id.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/detail/users/qr_sign_in.hpp"
#include "libchiaking/detail/users/sign_in.hpp"
#include "libchiaking/platform/runtime/stop_token.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking {
class ChiakiNg;
}  // namespace libchiaking

namespace libchiaking::testing {
class ServiceBuilder;
}  // namespace libchiaking::testing

namespace libchiaking::platform {
class SecureRandomProvider;
}

namespace libchiaking::platform {
class ClockProvider;
}

namespace libchiaking::platform {
class ThreadingProvider;
}

namespace libchiaking::platform {
class DiagnosticsProvider;
class HttpProvider;
}  // namespace libchiaking::platform

namespace libchiaking::users {

/// @brief The set of users the library knows, and the entry point for signing
/// them in and out.
///
/// One SignedInUsers is created by the application. It owns every PsUser and
/// hands them out as `std::shared_ptr<PsUser>`; there is at most one current
/// PsUser per account. Users enter the set by being loaded from the
/// CredentialStore, signing in with an NPSSO, or being added as a home user.
///
/// The providers and the store passed to the constructor are referenced, not
/// owned, and must outlive this object. All members are safe to call from
/// several threads. sign_in(), sign_in_with_qr() and sign_in_again() talk to
/// PSN and block the calling thread.
///
/// Destroying the object wipes the secrets of every user it handed out. Nothing
/// is removed from the store by destruction.
///
/// @see PsUser
/// @see CredentialStore
/// @ingroup api_users
class SignedInUsers {
 public:
  /// @brief A moment on PSN's own clock, in milliseconds.
  using ServerInstant = std::chrono::sys_time<std::chrono::milliseconds>;

  /// @brief What welcome_back() loaded.
  struct Returned {
    /// @brief The users now known from the store: saved sign-ins first,
    /// then home users.
    std::vector<std::shared_ptr<PsUser>> users;
    /// @brief Number of stored rows that failed validation and were
    /// skipped.
    std::size_t rejected{};
  };

  /// @brief Why sign_in_again() did not renew a user's sign-in.
  class SignInAgainRefusal {
   public:
    /// @brief The kinds of refusal.
    enum class Kind : std::uint8_t {
      /// @brief The NPSSO belongs to another account. signed_in_as()
      /// names it.
      different_account,
      /// @brief The user is not one this registry currently holds.
      unknown_user,
      /// @brief The exchange with PSN failed. sign_in_error() says how.
      sign_in_failed,
    };

    /// @brief The account an NPSSO turned out to belong to.
    struct OtherAccount {
      /// @brief That account's number.
      AccountId account;
      /// @brief That account's online id.
      std::string online_id;
    };

    /// @brief Makes the refusal for an NPSSO that belongs to another
    /// account.
    ///
    /// @param signed_in_as The account the NPSSO belongs to.
    ///
    /// @return The refusal with Kind::different_account.
    static auto different_account(OtherAccount signed_in_as)
        -> SignInAgainRefusal;
    /// @brief Makes the refusal for a user this registry does not hold.
    ///
    /// @return The refusal with Kind::unknown_user.
    static auto unknown_user() -> SignInAgainRefusal;
    /// @brief Makes the refusal for a failed exchange with PSN.
    ///
    /// @param error The failure.
    ///
    /// @return The refusal with Kind::sign_in_failed carrying the error.
    static auto sign_in_failed(SignInError error) -> SignInAgainRefusal;

    /// @brief Which kind of refusal this is.
    ///
    /// @return The kind.
    auto kind() const noexcept -> Kind {
      return static_cast<Kind>(reason_.index());
    }
    /// @brief The account the NPSSO belongs to.
    ///
    /// @return The account for Kind::different_account, or null for any
    ///     other kind. The pointer lives as long as this refusal.
    auto signed_in_as() const noexcept -> const OtherAccount* {
      return std::get_if<OtherAccount>(&reason_);
    }
    /// @brief The failed exchange.
    ///
    /// @return The failure for Kind::sign_in_failed, or null for any other
    ///     kind. The pointer lives as long as this refusal.
    auto sign_in_error() const noexcept -> const SignInError* {
      return std::get_if<SignInError>(&reason_);
    }

   private:
    struct UnknownUser {};
    using Reason = std::variant<OtherAccount, UnknownUser, SignInError>;
    static_assert(std::variant_size_v<Reason> ==
                  static_cast<std::size_t>(Kind::sign_in_failed) + 1);

    explicit SignInAgainRefusal(Reason reason) : reason_(std::move(reason)) {}

    Reason reason_;
  };

  /// @brief Not copyable; there is one registry and it owns its users.
  SignedInUsers(const SignedInUsers&) = delete;
  /// @brief Not copy-assignable; there is one registry and it owns its users.
  auto operator=(const SignedInUsers&) -> SignedInUsers& = delete;
  /// @brief Wipes the secrets of every user and releases the registry. The
  /// store is left untouched.
  ~SignedInUsers();

  /// @brief Loads the saved users from the CredentialStore.
  ///
  /// Call it once at start-up. Nothing is sent to PSN; loaded users report
  /// whatever state their saved tokens imply, and PsUser::stay_signed_in
  /// renews them. A user that is already known is kept as the same object. A
  /// saved sign-in without a device id is given a new one and saved again.
  ///
  /// @return The users loaded and the number of rows skipped.
  /// @retval CredentialStore::Error::unavailable The store could not be read.
  [[nodiscard]] auto welcome_back()
      -> Expected<Returned, CredentialStore::Error>;
  /// @brief Signs a user in with an NPSSO obtained from PSN's web sign-in.
  ///
  /// The NPSSO is exchanged for Remote Play and app tokens and the account is
  /// looked up, which takes several requests. The resulting user is saved to
  /// the CredentialStore; a failed save does not fail the sign-in and shows
  /// as PsUser::is_saved being false. If a user for the same account is
  /// already known, it is replaced: the old object has its secrets wiped and
  /// reports SignInState::signed_out, and the returned object takes its
  /// place.
  ///
  /// @param npsso The sign-in seed. It is copied into the new user; the
  ///     caller's copy is untouched.
  /// @param device The device id to sign in as. Reuse the saved one for a
  ///     returning account.
  /// @param correlation_id An identifier for the authorisation request, in
  ///     UUID form.
  /// @param stop Cancels the exchange.
  /// @param npsso_expires_at When the NPSSO stops being accepted, if the
  ///     application knows. Recorded with the sign-in and reported by
  ///     PsUser::held_credentials.
  /// @return The signed-in user.
  /// @retval SignInErrorKind::rejected PSN refused the NPSSO.
  /// @retval SignInErrorKind::malformed A reply could not be read,
  ///     or what PSN issued did not pass validation.
  /// @retval SignInError Any other failure of the exchange; see
  ///     the sign-in service.
  [[nodiscard]] auto sign_in(
      const Npsso& npsso, const DeviceId& device,
      std::string_view correlation_id, const platform::StopToken& stop = {},
      std::optional<SavedSignIn::Time> npsso_expires_at = std::nullopt)
      -> Expected<std::shared_ptr<PsUser>, SignInError>;
  /// @brief Signs a user in without a password, by a code another device
  /// approves.
  ///
  /// The library reads PS's hosted sign-in registration afresh, obtains an
  /// application token and asks PSN for a code. The code is shown through the
  /// observer as an approval URL to render as a QR code, with six digits the
  /// approving device shows beside it. The library then asks PSN, at the
  /// interval PSN sets, whether the code was approved. A code that expires,
  /// or that PSN invalidates, is replaced by a new one under the next
  /// attempt; at most ten codes are issued. Once approved, the account is
  /// signed in and kept exactly as sign_in() keeps it. The NPSSO the approval
  /// yields is never handed to the caller, and nothing the observer sees is
  /// secret.
  ///
  /// Blocks the calling thread until the sign-in ends, which can take as long
  /// as the codes last. The observer is called on this thread at every
  /// change of state.
  ///
  /// @param device The device id to sign in as. Reuse the saved one for a
  ///     returning account.
  /// @param correlation_id An identifier for the authorisation request, in
  ///     UUID form.
  /// @param stop Cancels the sign-in, including the pause between two
  ///     questions to PSN.
  /// @param observer Told every QrSignInState, in order; may be empty.
  /// @param options Optional choices, such as
  ///     `QrSignInOptions{}.device_name("Living room Switch")` for the name
  ///     the approving device shows; QrSignInOptions::default_device_name
  ///     when left alone.
  /// @return The signed-in user.
  /// @retval SignInErrorKind::invalid_request The correlation id, or the
  ///     device name chosen in `options`, is not usable. Nothing was sent.
  /// @retval SignInErrorKind::declined The sign-in was refused on the
  ///     approving device.
  /// @retval SignInErrorKind::expired Every code expired before it was
  ///     approved.
  /// @retval SignInErrorKind::verification_required PSN kept asking for
  ///     additional verification until no code was left.
  /// @retval SignInErrorKind::layout_changed PS's hosted sign-in page
  ///     changed, so the registration could not be read.
  /// @retval SignInErrorKind::cancelled The stop token was triggered.
  /// @retval SignInError Any other failure of the exchange; see sign_in().
  [[nodiscard]] auto sign_in_with_qr(const DeviceId& device,
                                     std::string_view correlation_id,
                                     const platform::StopToken& stop,
                                     QrSignInObserver observer,
                                     const QrSignInOptions& options = {})
      -> Expected<std::shared_ptr<PsUser>, SignInError>;
  /// @brief Renews the sign-in of a known user with a fresh NPSSO, typically
  /// after it expired.
  ///
  /// It works as sign_in() does, with the added check that the NPSSO belongs
  /// to the same account as `user`. On success the user is replaced by a new
  /// object: the one passed in has its secrets wiped and reports
  /// SignInState::signed_out, so callers must switch to the returned pointer.
  ///
  /// @param user The user to renew. Must be the object this registry
  ///     currently holds for its account.
  /// @param npsso The fresh sign-in seed.
  /// @param device The device id to sign in as.
  /// @param correlation_id An identifier for the authorisation request, in
  ///     UUID form.
  /// @param stop Cancels the exchange.
  /// @param npsso_expires_at When the NPSSO stops being accepted, if the
  ///     application knows.
  /// @return The renewed user, a different object from `user`.
  /// @retval SignInAgainRefusal::Kind::unknown_user `user` is not held by
  ///     this registry, or stopped being held while the exchange ran.
  /// @retval SignInAgainRefusal::Kind::sign_in_failed The exchange with PSN
  ///     failed.
  /// @retval SignInAgainRefusal::Kind::different_account The NPSSO belongs to
  ///     another account. Nothing changes.
  [[nodiscard]] auto sign_in_again(
      const PsUser& user, const Npsso& npsso, const DeviceId& device,
      std::string_view correlation_id, const platform::StopToken& stop = {},
      std::optional<SavedSignIn::Time> npsso_expires_at = std::nullopt)
      -> Expected<std::shared_ptr<PsUser>, SignInAgainRefusal>;
  /// @brief Adds a user who has no PSN sign-in, known only by account id and
  /// name.
  ///
  /// Such a user can pair with a console and play on the home network. The
  /// user is saved to the CredentialStore. If the account is already known
  /// through a PSN sign-in, that user is returned and nothing is added.
  ///
  /// @param account The PSN account id the user pairs as.
  /// @param name The name to show for the user.
  /// @return The user.
  /// @retval PsUser::Invalid::account_id The account id is zero.
  /// @retval PsUser::Invalid::online_id The name is empty, longer than
  ///     PsUser::online_id_limit or holds a control character.
  [[nodiscard]] auto add_home_user(AccountId account, std::string_view name)
      -> Expected<std::shared_ptr<PsUser>, PsUser::Invalid>;
  /// @brief Signs a user out and deletes what was saved for them.
  ///
  /// The user leaves the registry and has its secrets wiped at once; holders
  /// of the pointer see SignInState::signed_out. The saved row is then
  /// deleted from the CredentialStore. Pairings are not touched: they belong
  /// to consoles::KnownConsoles.
  ///
  /// @param account The account to sign out. An account that is not known is
  ///     still deleted from the store.
  /// @return Nothing on success.
  /// @retval CredentialStore::Error::unavailable The store refused the
  ///     deletion. The user has been removed from memory all the same, and
  ///     the saved row will come back at the next welcome_back().
  [[nodiscard]] auto sign_out(AccountId account)
      -> Expected<void, CredentialStore::Error>;

  /// @brief Looks a user up by account.
  ///
  /// @param account The account's number.
  /// @return The user, or null when none with that account is known.
  auto find(AccountId account) const -> std::shared_ptr<PsUser>;
  /// @brief Returns every known user as a snapshot.
  ///
  /// @return A copy of the list at the time of the call; empty when no user is
  /// known. Later changes do not affect it.
  auto everyone() const -> std::vector<std::shared_ptr<PsUser>>;
  /// @brief The current time by PSN's clock, as learned from the replies of
  /// PSN's servers.
  ///
  /// @return The time, or nothing until a reply has been seen.
  auto server_time() const noexcept -> std::optional<ServerInstant>;

  /// @brief The registry's internal state. Defined inside the library; not
  /// for frontends.
  class Core;

  /// @brief Reaches the internal state. For the library's own use.
  ///
  /// @return The state, which lives as long as this registry.
  auto core() const noexcept -> Core& { return *core_; }

 private:
  friend class libchiaking::ChiakiNg;
  friend class libchiaking::testing::ServiceBuilder;

  /// @brief Creates the registry.
  ///
  /// @param http Carries the PSN requests.
  /// @param credentials Persists the users.
  /// @param threading Makes the registry's locks and the waits on shared
  ///     requests. Must outlive the registry.
  /// @param clock Monotonic clock, used for cache lifetimes. Must outlive the
  ///     registry.
  /// @param wall Wall clock, used for token issue and expiry times. Must
  ///     outlive the registry.
  /// @param random Generates a device id for a saved sign-in that has none.
  /// @param log Receives diagnostics. May be null.
  SignedInUsers(platform::HttpProvider& http, CredentialStore& credentials,
                platform::ThreadingProvider& threading,
                const platform::ClockProvider& clock,
                const platform::ClockProvider& wall,
                platform::SecureRandomProvider& random,
                platform::DiagnosticsProvider* log = nullptr);

  std::unique_ptr<Core> core_;
};

}  // namespace libchiaking::users
