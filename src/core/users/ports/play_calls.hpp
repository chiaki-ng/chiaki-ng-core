/// @file
/// @brief Declares PlayCalls, the play context's port onto a user's authorised
/// calls to PS.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/authenticator.hpp"
#include "src/core/psn/rest/kamaji/kamaji_client.hpp"
#include "src/core/psn/types.hpp"
#include "src/core/users/adapters/authorisation_refusal.hpp"
#include "src/core/users/adapters/old_service_checkout.hpp"
#include "src/core/users/ports/remote_play_access.hpp"

#include <libchiaking/detail/users/device_id.hpp>
#include <libchiaking/detail/users/ps_user.hpp>
#include <libchiaking/platform/runtime/stop_token.hpp>

#include <functional>

namespace libchiaking::users {

/// @brief What the play context may ask PS on a user's behalf: the cloud
/// device and its check, cloud streaming codes, the LegacyCloudStream store
/// and checkout, Remote Play access, and telling the user that PS rejected
/// its membership.
///
/// A user that is off PSN is answered at once and nothing is sent. A failure
/// that means PSN no longer accepts the NPSSO marks the user's sign-in as
/// refused, so the user reports SignInState::expired.
///
/// The object is a short-lived view: it only refers to the user's core, holds
/// no state of its own, and is made afresh wherever a call is needed. It must
/// not outlive the PsUser it was made for. Every call blocks the calling
/// thread for its requests, each bounded by the HTTP client's timeouts and its
/// stop token; calls may be made from several threads at once, the user's core
/// providing the locking.
class PlayCalls {
 public:
  /// @brief Views a user handed out by SignedInUsers.
  ///
  /// @param user The user; must outlive this object.
  explicit PlayCalls(PsUser& user) noexcept;

  /// @brief The device id a cloud launch identifies itself with. Nothing is
  /// sent.
  ///
  /// @return A copy of the user's device id.
  /// @retval AuthorisationRefusal::signed_out The user is off PSN.
  [[nodiscard]] auto cloud_device() const
      -> Expected<DeviceId, AuthorisationRefusal>;

  /// @brief Asks the authorisation server whether this device may stream
  /// from a cloud service.
  ///
  /// Concurrent callers for the same service share one request.
  ///
  /// @param http The client the request is sent through; its stop token
  ///     cancels it.
  /// @param service The streaming service to check.
  /// @return Nothing when the device may stream.
  /// @retval AuthorisationRefusal::signed_out The user is off PSN.
  /// @retval AuthorisationRefusal The server refused or could not be asked;
  ///     when the refusal expires the sign-in, the sign-in is marked refused.
  [[nodiscard]] auto cloud_device_check(net::http::Client& http,
                                        psn::StreamingService service)
      -> Expected<void, AuthorisationRefusal>;

  /// @brief Obtains the GK authorisation codes a cloud session is
  /// allocated with.
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them.
  /// @param service The streaming service.
  /// @param platform The platform to stream.
  /// @param ids The GK client ids the codes are issued to.
  /// @param before Told each step just before it starts; may be empty.
  /// @return The codes.
  /// @retval AuthorisationRefusal::signed_out The user is off PSN.
  /// @retval AuthorisationRefusal The server refused or could not be asked;
  ///     when the refusal expires the sign-in, the sign-in is marked refused.
  [[nodiscard]] auto cloud_streaming_codes(
      net::http::Client& http, psn::StreamingService service,
      psn::StreamingPlatform platform, const psn::auth::GkClientIds& ids,
      const std::function<void(psn::auth::GkStep)>& before = {})
      -> Expected<psn::auth::GkCodes, AuthorisationRefusal>;

  /// @brief Opens a LegacyCloudStream title store session: a title code from
  /// the authorisation server, then a Kamaji session with it.
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them. Replies are capped at json::max_document_bytes.
  /// @param before Told OldServiceRefusal::Step::title_code and then
  ///     OldServiceRefusal::Step::title_store just before each starts; may be
  ///     empty.
  /// @return A store client with an open session.
  /// @retval OldServiceRefusal::Step::title_code The user is off PSN, or the
  ///     code could not be obtained.
  /// @retval OldServiceRefusal::Step::title_store The session request failed,
  ///     or the store replied without opening a session.
  [[nodiscard]] auto old_service_title_store(
      net::http::Client& http,
      const std::function<void(OldServiceRefusal::Step)>& before = {})
      -> Expected<psn::rest::KamajiClient, OldServiceRefusal>;

  /// @brief Opens the account's LegacyCloudStream session on a store: an
  /// account code for the platform, then a Kamaji session with it.
  ///
  /// @param http The client the code request is sent through; its stop
  ///     token cancels it.
  /// @param store The store to open the session on.
  /// @param platform The platform to stream.
  /// @param before Told OldServiceRefusal::Step::account_code and then
  ///     OldServiceRefusal::Step::account_session just before each starts;
  ///     may be empty.
  /// @return The account session.
  /// @retval OldServiceRefusal::Step::account_code The user is off PSN, or
  ///     the code could not be obtained.
  /// @retval OldServiceRefusal::Step::account_session The session request
  ///     failed.
  [[nodiscard]] auto old_service_account_session(
      net::http::Client& http, psn::rest::KamajiClient& store,
      psn::StreamingPlatform platform,
      const std::function<void(OldServiceRefusal::Step)>& before = {})
      -> Expected<psn::rest::KamajiSession, OldServiceRefusal>;

  /// @brief Starts the checkout steps of a LegacyCloudStream launch. Nothing is
  /// sent yet.
  ///
  /// @param http The client the checkout steps are sent through; must
  ///     outlive the checkout.
  /// @return The checkout, which must not outlive the user or `http`.
  /// @retval OldServiceRefusal::Step::checkout_token The user is off PSN.
  [[nodiscard]] auto old_service_checkout(net::http::Client& http)
      -> Expected<OldServiceCheckout, OldServiceRefusal>;

  /// @brief Lends the Remote Play token for an internet start, renewing it
  /// first if it is missing or near expiry.
  ///
  /// @param stop Cancels the renewal.
  /// @return The lent token.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::remote_play_access_missing No usable Remote
  ///     Play token could be obtained.
  /// @retval Unanswered::Reason Any other failure of the renewal.
  [[nodiscard]] auto remote_play_access(const platform::StopToken& stop = {})
      -> Expected<RemotePlayAccess, Unanswered>;

  /// @brief Forgets the remembered membership after PS rejected it, so the
  /// next question asks PSN; see
  /// AuthorisedRequest::membership_rejected_by_service.
  void membership_rejected_by_service();

 private:
  /// @brief Wraps an authorisation server failure for a cloud streaming
  /// step, marking the sign-in refused when the failure expires it.
  ///
  /// @param error The failure.
  /// @return The refusal.
  [[nodiscard]] auto unauthorised(const psn::auth::AuthError& error)
      -> AuthorisationRefusal;

  /// @brief Wraps an authorisation server failure for a LegacyCloudStream step,
  /// marking the sign-in refused when the failure expires it.
  ///
  /// @param step The step that failed.
  /// @param error The failure.
  /// @return The refusal for that step.
  [[nodiscard]] auto old_service_refused(OldServiceRefusal::Step step,
                                         const psn::auth::AuthError& error)
      -> OldServiceRefusal;

  /// @brief The user the calls are made for.
  PsUser::Core& user_;
};

}  // namespace libchiaking::users
