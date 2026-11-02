/// @file
/// @brief Declares OldServiceCheckout, the checkout steps a LegacyCloudStream
/// launch takes on a user's behalf to make sure the account holds the title's
/// free licence, and OldServiceRefusal, why a step of starting a title on the
/// older PlayStation Now service failed.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/net/transport/http/failure.hpp"
#include "src/core/psn/auth/auth_error.hpp"
#include "src/core/psn/auth/authenticator.hpp"
#include "src/core/psn/rest/accounts/accounts_client.hpp"
#include "src/core/psn/rest/commerce/commerce_client.hpp"
#include "src/core/psn/rest/kamaji/kamaji_client.hpp"

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/users/ps_user.hpp>
#include "src/core/psn/auth/credentials.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace libchiaking::users {

/// @brief A failed step of the LegacyCloudStream launch sequence, naming the
/// step and what went wrong in it.
///
/// Launching on the older service walks the user through several PS services
/// in turn: authorisation codes, the Kamaji store sessions, a commerce checkout
/// token, an account attribute check and the free-licence checkout. Each step
/// that can fail is a Step, and the Cause records which of those services
/// answered and how. PlayCalls and OldServiceCheckout produce it; the PS
/// Now launch in the play context turns it into a launch failure.
///
/// An authorisation refusal that means the NPSSO is no longer accepted
/// (expires_sign_in()) has already marked the user's sign-in as refused by the
/// time the caller sees it.
///
/// A value type; copy it freely and read it from any thread.
class OldServiceRefusal {
 public:
  /// @brief The step of the LegacyCloudStream launch that failed.
  enum class Step : std::uint8_t {
    /// @brief Getting the authorisation code that opens the title store.
    title_code,
    /// @brief Opening the Kamaji title store session with that code.
    title_store,
    /// @brief Getting the commerce bearer token used for the checkout
    /// steps that follow.
    checkout_token,
    /// @brief Checking that the account has the attributes, such as
    /// accepted privacy terms, the service requires.
    account_attributes,
    /// @brief Asking whether the account already holds the title's licence.
    licence_status,
    /// @brief Previewing the checkout of the title's SKU, to learn whether
    /// it is free and which SKU to buy.
    preview,
    /// @brief Checking out the free SKU, which grants the licence.
    buy,
    /// @brief Getting the authorisation code that opens the account's
    /// session on the streaming platform.
    account_code,
    /// @brief Opening the Kamaji account session with that code.
    account_session,
  };

  /// @brief The user had no PSN sign-in, so nothing was sent.
  struct SignedOut {};

  /// @brief The Kamaji store replied but opened no session.
  struct NoStoreSession {
    /// @brief The HTTP status of that reply.
    std::uint16_t http_status{};
  };

  /// @brief What went wrong in the step: no sign-in, a refusal by the
  /// authorisation server, a request that did not complete, an error reply
  /// from the Kamaji store, an error reply from the accounts service, or a
  /// store reply with no session.
  using Cause = std::variant<SignedOut, psn::auth::AuthError,
                             net::http::Failure, psn::rest::StoreError,
                             psn::rest::AccountsError, NoStoreSession>;

  /// @brief Records a failed step.
  ///
  /// @param step The step that failed.
  /// @param cause What went wrong in it.
  OldServiceRefusal(Step step, Cause cause);

  /// @brief Makes the refusal for a failed request to the Kamaji store.
  ///
  /// @param step The step that made the request.
  /// @param error The store's failure.
  /// @return A refusal whose cause is the transport failure when the request
  ///     did not complete, or the store error when the store replied.
  [[nodiscard]] static auto from(Step step, const psn::rest::StoreError& error)
      -> OldServiceRefusal;
  /// @brief Makes the refusal for a failed request to the accounts service.
  ///
  /// @param step The step that made the request.
  /// @param error The accounts service's failure; consumed.
  /// @return A refusal whose cause is the transport failure when the request
  ///     did not complete, or the accounts error, with any missing
  ///     attributes, when the service replied.
  [[nodiscard]] static auto from(Step step, psn::rest::AccountsError error)
      -> OldServiceRefusal;

  /// @brief The step that failed.
  ///
  /// @return The step.
  auto step() const noexcept -> Step { return step_; }
  /// @brief What went wrong in the step.
  ///
  /// @return The cause, which lives as long as this refusal.
  auto cause() const noexcept -> const Cause& { return cause_; }
  /// @brief Whether nothing was sent because the user has no PSN sign-in.
  ///
  /// @return True when the cause is SignedOut.
  auto is_signed_out() const noexcept -> bool {
    return std::holds_alternative<SignedOut>(cause_);
  }
  /// @brief Whether the step was abandoned because the caller's stop token
  /// fired.
  ///
  /// @return True for a cancelled authorisation request or a cancelled
  ///     transport failure; false otherwise.
  auto is_cancelled() const noexcept -> bool;
  /// @brief Whether this refusal means the user must sign in again.
  ///
  /// @return True when the cause is an authorisation failure for which
  ///     AuthorisationRefusal::expires_sign_in holds; false for every other
  ///     cause.
  auto expires_sign_in() const noexcept -> bool;
  /// @brief The HTTP status of the reply behind the refusal.
  ///
  /// @return The status of the authorisation, store, accounts or sessionless
  ///     store reply, or 0 for a signed-out refusal or a request that did not
  ///     complete.
  auto http_status() const noexcept -> std::uint16_t;
  /// @brief The authorisation server's failure.
  ///
  /// @return The failure when the cause is one, otherwise nothing.
  auto authorisation() const noexcept -> std::optional<psn::auth::AuthError>;
  /// @brief The failure of a request that did not complete.
  ///
  /// @return The transport failure when the cause is one, otherwise nothing.
  auto failure() const noexcept -> std::optional<net::http::Failure>;
  /// @brief The Kamaji store's error reply.
  ///
  /// @return The store error when the cause is one, otherwise nothing.
  auto store() const noexcept -> std::optional<psn::rest::StoreError>;
  /// @brief The account attributes the accounts service said are missing,
  /// which the user must supply on PSN, typically by accepting updated
  /// privacy terms.
  ///
  /// @return The names of the missing attributes, viewed in this refusal and
  ///     living as long as it; empty when the cause is not an accounts error
  ///     or the service named none.
  auto missing_account_attributes() const noexcept
      -> std::span<const std::string>;

 private:
  /// @brief The step that failed; fixed at construction.
  Step step_;
  /// @brief What went wrong in it; fixed at construction.
  Cause cause_;
};

/// @brief One LegacyCloudStream launch's checkout conversation with PS's
/// commerce, accounts and Kamaji services, carried on one commerce bearer
/// token.
///
/// The older service streams a title only when the account holds a licence
/// for it; for titles in the user's plan that licence is free and is taken by
/// checking out the title's SKU. The launch asks, in order,
/// account_attributes() (optional), licence_status(), and when the licence is
/// missing preview() and buy().
///
/// The commerce bearer token is obtained on first use through the user's own
/// token cache, so a token from an earlier launch may be reused, and every
/// reply is recorded against it in that cache. When the token cannot be
/// obtained, the refusal is remembered and every later step fails with it
/// without asking again. A refused token that came from the cache is renewed
/// once for account_attributes() and licence_status().
///
/// Made only by PlayCalls::old_service_checkout. Move-only. It refers to
/// the user's core and to the HTTP client it was made with without owning
/// them, so it must not outlive either. Every step blocks on the network, and
/// one object must be used by one thread at a time.
class OldServiceCheckout {
 public:
  /// @brief Takes over another checkout's token and clients.
  OldServiceCheckout(OldServiceCheckout&&) noexcept;
  /// @brief Not assignable; a checkout stays bound to its user and client.
  auto operator=(OldServiceCheckout&&) -> OldServiceCheckout& = delete;
  /// @brief Releases the token; its secret is wiped. The cached copy in the
  /// user's token cache is kept.
  ~OldServiceCheckout();

  /// @brief Checks that the account has every attribute the older service
  /// requires, such as accepted privacy terms.
  ///
  /// Blocks for one request, or two when a cached token is refused and
  /// renewed.
  ///
  /// @return The accepting reply's HTTP status.
  /// @retval OldServiceRefusal::Step::checkout_token The commerce token could
  ///     not be obtained or renewed.
  /// @retval OldServiceRefusal::Step::account_attributes The request failed or
  ///     the service refused; OldServiceRefusal::missing_account_attributes
  ///     names what the user must supply.
  [[nodiscard]] auto account_attributes()
      -> Expected<psn::rest::AttributesAccepted, OldServiceRefusal>;
  /// @brief Asks whether the account already holds an entitlement.
  ///
  /// Blocks for one request, or two when a cached token is refused (401 or
  /// 403) and renewed.
  ///
  /// @param entitlement The entitlement id of the title's licence.
  /// @return The HTTP status of the lookup's reply, whatever it is: 200 when
  ///     the account holds the licence, 404 when it does not, anything else
  ///     for the caller to judge.
  /// @retval OldServiceRefusal::Step::checkout_token The commerce token could
  ///     not be obtained or renewed.
  /// @retval OldServiceRefusal::Step::licence_status The lookup did not
  ///     complete.
  [[nodiscard]] auto licence_status(std::string_view entitlement)
      -> Expected<std::uint16_t, OldServiceRefusal>;
  /// @brief Previews the checkout of a SKU, to learn whether it is free and
  /// which SKU the store would actually sell.
  ///
  /// Blocks for one request. Not retried.
  ///
  /// @param store A Kamaji title store with an open session.
  /// @param sku The SKU to preview.
  /// @return The preview: whether it is free, its displayed price and the SKU
  ///     to check out.
  /// @retval OldServiceRefusal::Step::checkout_token The commerce token could
  ///     not be obtained.
  /// @retval OldServiceRefusal::Step::preview The request failed or the store
  ///     refused.
  [[nodiscard]] auto preview(psn::rest::KamajiClient& store,
                             std::string_view sku)
      -> Expected<psn::rest::CheckoutPreview, OldServiceRefusal>;
  /// @brief Checks out a SKU, granting the account its licence.
  ///
  /// The caller must have confirmed through preview() that the SKU is free;
  /// this call does not check. Blocks for one request. Not retried.
  ///
  /// @param store A Kamaji title store with an open session.
  /// @param sku The SKU to check out.
  /// @return Nothing once the store accepted the checkout.
  /// @retval OldServiceRefusal::Step::checkout_token The commerce token could
  ///     not be obtained.
  /// @retval OldServiceRefusal::Step::buy The request failed or the store
  ///     refused.
  [[nodiscard]] auto buy(psn::rest::KamajiClient& store, std::string_view sku)
      -> Expected<void, OldServiceRefusal>;

 private:
  friend class PlayCalls;

  /// @brief The launch steps a refusal can name.
  using Step = OldServiceRefusal::Step;

  /// @brief Label under which the commerce token is logged and recorded.
  static constexpr std::string_view bearer_credential = "commerce bearer";

  /// @brief Starts a checkout for a signed-in user. No request is sent yet.
  ///
  /// @param user The user to check out as; must have a PSN sign-in and
  ///     outlive this object.
  /// @param http The client every step is sent through; must outlive this
  ///     object. The commerce and accounts clients cap replies at
  ///     json::max_document_bytes and send the LegacyCloudStream user agent.
  OldServiceCheckout(PsUser::Core& user, net::http::Client& http);

  /// @brief Makes sure a commerce token is held, obtaining one on first use.
  ///
  /// Blocks for the token exchange the first time only.
  ///
  /// @return Nothing once a token is held.
  /// @retval OldServiceRefusal::Step::checkout_token The token could not be
  ///     obtained, now or on an earlier call; the same refusal is returned
  ///     every time.
  [[nodiscard]] auto token() -> Expected<void, OldServiceRefusal>;
  /// @brief Replaces the held token after PS refused it.
  ///
  /// Blocks for the token exchange.
  ///
  /// @param refused_status The HTTP status PS refused the token with.
  /// @return Nothing once the fresh token is held.
  /// @retval OldServiceRefusal::Step::checkout_token The token could not be
  ///     renewed.
  [[nodiscard]] auto renewed(std::uint16_t refused_status)
      -> Expected<void, OldServiceRefusal>;
  /// @brief Turns a token failure into a checkout_token refusal, marking the
  /// user's sign-in refused when the failure means the NPSSO is no longer
  /// accepted.
  ///
  /// @param error The authorisation failure.
  /// @return The refusal for Step::checkout_token.
  [[nodiscard]] auto refused(const psn::auth::AuthError& error)
      -> OldServiceRefusal;
  /// @brief Whether a reply refused the token and the token can be renewed.
  ///
  /// @param status The reply's HTTP status.
  /// @return True when the status is 401 or 403 and the token came from the
  ///     cache and has not been renewed yet.
  auto renewable(std::uint16_t status) const noexcept -> bool;
  /// @brief A copy of the held token to send as a bearer. A token must be
  /// held.
  ///
  /// @return The copy, labelled as the commerce bearer.
  auto bearer() const -> psn::auth::BearerToken;
  /// @brief Records a reply against the held token in the user's token
  /// cache. A token must be held.
  ///
  /// @param status The reply's HTTP status.
  void used(std::uint16_t status);

  /// @brief The user checking out; never null.
  PsUser::Core* user_;
  /// @brief Obtains and renews the commerce token through the user's token
  /// cache.
  std::unique_ptr<psn::auth::Authenticator> auth_;
  /// @brief Looks entitlements up for licence_status().
  psn::rest::CommerceClient commerce_;
  /// @brief Checks account attributes for account_attributes().
  psn::rest::AccountsClient accounts_;
  /// @brief The commerce bearer token; empty until the first step obtains
  /// it.
  std::optional<psn::auth::BearerToken> token_;
  /// @brief Why the token could not be obtained; once set, every step fails
  /// with it.
  std::optional<OldServiceRefusal> unobtainable_;
};

}  // namespace libchiaking::users
