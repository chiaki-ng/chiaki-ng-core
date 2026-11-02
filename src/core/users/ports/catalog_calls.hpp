/// @file
/// @brief Declares CatalogCalls, the catalog context's port onto a user's
/// authorised calls to PS.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/graphql/authenticated/client.hpp"
#include "src/core/psn/rest/commerce/commerce_client.hpp"
#include "src/core/psn/rest/entitlement/library_entitlements.hpp"
#include "src/core/psn/rest/gamelist/gamelist.hpp"
#include "src/core/psn/rest/kamaji/kamaji_client.hpp"
#include "src/core/psn/rest/media/game_media.hpp"
#include "src/core/psn/rest/store/cloud_game_products.hpp"
#include "src/core/psn/rest/subscriptions/subscriptions_client.hpp"
#include "src/core/users/adapters/cloud_streaming_cache.hpp"
#include "src/core/users/cloud_streaming.hpp"

#include <libchiaking/detail/users/ps_user.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>

namespace libchiaking::users {

class AuthorisedRequest;

/// @brief What the catalog context may ask PS on a user's behalf: membership,
/// the store, owned entitlements, the cloud game catalogue, cloud streaming,
/// played titles and the media gallery.
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
class CatalogCalls {
 public:
  /// @brief Views a user handed out by SignedInUsers.
  ///
  /// @param user The user; must outlive this object.
  explicit CatalogCalls(PsUser& user) noexcept;
  /// @brief Views a user's core directly, for code inside the users
  /// context.
  ///
  /// @param user The user's core; must outlive this object.
  explicit CatalogCalls(PsUser::Core& user) noexcept : user_(user) {}

  /// @brief Finds out the user's PlayStation Plus membership; see
  /// AuthorisedRequest::answered_membership.
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them.
  /// @param remembered_from The lowest tier a remembered answer must show to
  ///     be reused.
  /// @return The membership and when it was learned.
  /// @retval Unanswered The failures AuthorisedRequest::answered_membership
  ///     lists.
  [[nodiscard]] auto answered_membership(net::http::Client& http,
                                         Tier remembered_from = Tier::none)
      -> Expected<MembershipAnswer, Unanswered>;

  /// @brief Opens the GraphQL client for the PlayStation Store app, signed
  /// with the user's app token.
  ///
  /// The app token is restored first unless the sign-in has expired.
  ///
  /// @param http The client the queries will be sent through; must outlive
  ///     the returned client.
  /// @return A client bound to the user's app token and GraphQL state; it
  ///     must not outlive the user.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason Any failure of restoring the app token; see
  ///     PsUser::Core::app_token_unless_refused.
  [[nodiscard]] auto store_app(net::http::Client& http)
      -> Expected<psn::graphql::AuthenticatedClient, Unanswered>;

  /// @brief Opens a session with the LegacyCloudStream (Kamaji) catalogue
  /// store.
  ///
  /// Asks the authorisation server for a catalogue code and opens a store
  /// session with it; concurrent callers with the same size limit share one
  /// session. When the store accepts the session, the country and language
  /// it reports become the user's region.
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  /// @return A store client on the opened session, its replies capped at
  ///     `maximum_response_bytes`.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::sign_in_expired The authorisation server
  ///     refused the NPSSO, or the store did not accept the session; the
  ///     sign-in is marked refused.
  /// @retval Unanswered::Reason::old_service_refused The store answered with
  ///     a status other than 200 and not a server error; the sign-in is
  ///     marked refused.
  /// @retval Unanswered::Reason::unavailable The session request did not
  ///     complete or the store answered with a server error.
  /// @retval Unanswered::Reason::cancelled The stop token fired.
  [[nodiscard]] auto old_service_store(
      net::http::Client& http, std::size_t maximum_response_bytes =
                                   net::http::default_maximum_response_bytes)
      -> Expected<psn::rest::KamajiClient, Unanswered>;

  /// @brief Reads every owned entitlement record from the commerce service,
  /// page by page.
  ///
  /// Uses a commerce token for owned entitlements from the session's token
  /// cache, renewed once if PS refuses a cached one. Concurrent callers
  /// with the same size limit share one walk.
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them.
  /// @param observe Told the outcome of each page as it lands; may be empty.
  /// @param maximum_response_bytes Largest reply accepted per page, in bytes.
  /// @return Every record.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::sign_in_expired The token was refused, or
  ///     the service refused it after renewal; the sign-in is marked refused.
  /// @retval Unanswered::Reason Any other failure; see the private
  ///     unanswered() overloads.
  [[nodiscard]] auto library_records(
      net::http::Client& http,
      const psn::rest::EntitlementPageObserver& observe = {},
      std::size_t maximum_response_bytes =
          net::http::default_maximum_response_bytes)
      -> Expected<psn::rest::Entitlements, Unanswered>;

  /// @brief Lists the game concepts the account owns of one package type,
  /// from the app library.
  ///
  /// Uses the app token as held; it is not renewed here.
  ///
  /// @param http The client the request is sent through.
  /// @param package_type The package type to list, such as `PSGD`.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  /// @param observe Told each page as it lands; may be empty.
  /// @return The owned concepts.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed No usable app token is
  ///     held.
  /// @retval Unanswered::Reason Any failure of the request; see the private
  ///     unanswered(const psn::rest::Error<Reasons...>&).
  [[nodiscard]] auto library_concepts(
      net::http::Client& http, std::string_view package_type,
      std::size_t maximum_response_bytes =
          net::http::default_maximum_response_bytes,
      const psn::rest::LibraryPageObserver& observe = {})
      -> Expected<psn::rest::EntitlementConcepts, Unanswered>;

  /// @brief Lists the PS5 games the account owns that stream to this kind of
  /// device, from the app library.
  ///
  /// Uses the app token as held; it is not renewed here.
  ///
  /// @param http The client the requests are sent through.
  /// @param maximum_response_bytes Largest reply accepted per page, in bytes.
  /// @param observe Told each page as it lands; may be empty.
  /// @return The owned games.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed No usable app token is
  ///     held.
  /// @retval Unanswered::Reason Any failure of the requests.
  [[nodiscard]] auto library_ps5_games(
      net::http::Client& http,
      std::size_t maximum_response_bytes =
          net::http::default_maximum_response_bytes,
      const psn::rest::LibraryPageObserver& observe = {})
      -> Expected<psn::rest::GameEntitlements, Unanswered>;

  /// @brief Re-reads one owned PS5 entitlement, as a launch check does.
  ///
  /// Uses the app token as held; it is not renewed here.
  ///
  /// @param http The client the request is sent through.
  /// @param id The entitlement id.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  /// @return The entitlement, or nothing when the account does not hold it.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed No usable app token is
  ///     held.
  /// @retval Unanswered::Reason Any failure of the request.
  [[nodiscard]] auto library_ps5_game(
      net::http::Client& http, std::string_view id,
      std::size_t maximum_response_bytes =
          net::http::default_maximum_response_bytes)
      -> Expected<std::optional<psn::rest::GameEntitlement>, Unanswered>;

  /// @brief Looks one owned entitlement record up in the commerce service.
  ///
  /// Uses the commerce token as library_records() does. Concurrent callers
  /// asking for the same id and size limit share one request.
  ///
  /// @param http The client the request is sent through; its stop token
  ///     cancels it.
  /// @param id The entitlement id.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  /// @return The lookup, which carries the reply's status.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::sign_in_expired The token was refused; the
  ///     sign-in is marked refused.
  /// @retval Unanswered::Reason Any other failure.
  [[nodiscard]] auto library_record(
      net::http::Client& http, std::string_view id,
      std::size_t maximum_response_bytes =
          net::http::default_maximum_response_bytes)
      -> Expected<psn::rest::EntitlementLookup, Unanswered>;

  /// @brief Asks the cloud game catalogue for the first product that answers
  /// a question, as the selected-title resolver does.
  ///
  /// Uses the app token as held; it is not renewed here.
  ///
  /// @param http The client the request is sent through.
  /// @param question What to look up.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  /// @return The catalogue's reply.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed No usable app token is
  ///     held.
  /// @retval Unanswered::Reason Any failure of the request.
  [[nodiscard]] auto cloud_game_product(
      net::http::Client& http, const psn::rest::CloudGameQuestion& question,
      std::size_t maximum_response_bytes =
          net::http::default_maximum_response_bytes)
      -> Expected<psn::rest::CloudGameReply, Unanswered>;

  /// @brief Judges whether the user may stream PS5 games from the cloud.
  ///
  /// With CloudStreamingCache::Freshness::held_will_do a fresh held verdict
  /// is returned without asking. Otherwise the cloud game feature is read
  /// and, when PS did not authorise streaming, the first PlayStation Plus
  /// service too, to word the refusal; concurrent callers share one read.
  /// The verdict is kept for later callers.
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them.
  /// @param freshness Whether a held verdict will do.
  /// @return The verdict. It is unknown, carrying the reason, when the user
  ///     is off PSN, no usable app token is held, or the feature could not
  ///     be read; a failed PlayStation Plus read other than cancellation
  ///     words the refusal as subscribe.
  [[nodiscard]] auto cloud_streaming(net::http::Client& http,
                                     CloudStreamingCache::Freshness freshness)
      -> CloudStreaming;

  /// @brief Returns the held cloud streaming verdict without asking PS.
  ///
  /// @return The verdict while it is fresh; nothing when none is held, it
  ///     is stale, or the user is off PSN.
  [[nodiscard]] auto held_cloud_streaming() const
      -> std::optional<CloudStreaming>;

  /// @brief Learns the region the account's token states when no region has
  /// been learned yet.
  ///
  /// The token's legal country and language are what the LegacyCloudStream
  /// session reports, so an anonymous store walk can stand in for the session.
  /// Nothing is sent, and a region already learned is kept.
  ///
  /// @return The region now known, or std::nullopt when neither the user nor
  ///     the token states one.
  auto settle_region() -> std::optional<Region>;

  /// @brief Reads one page of the titles the account has played recently,
  /// signed with the app token.
  ///
  /// The app token is restored first and renewed once if PS refuses it; see
  /// AuthorisedRequest::played_titles.
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them.
  /// @param offset Index of the first title on the page.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  /// @return The page.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto played_titles(
      net::http::Client& http, std::size_t offset,
      std::size_t maximum_response_bytes =
          net::http::default_maximum_response_bytes)
      -> Expected<psn::rest::PlayedTitlesPage, Unanswered>;

  /// @brief Finds where a capture in the account's cloud media gallery can
  /// be downloaded, signed with the app token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them.
  /// @param gallery_id The capture's id in the gallery.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  /// @return Where to download it.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto gallery_capture(
      net::http::Client& http, std::string_view gallery_id,
      std::size_t maximum_response_bytes =
          net::http::default_maximum_response_bytes)
      -> Expected<psn::rest::GalleryCapture, Unanswered>;

  /// @brief Reads whether the account wants captures taken in PS5 cloud
  /// games downloaded to the device streaming them, signed with the app
  /// token.
  ///
  /// Renews the app token as installed_titles() does.
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  /// @return Whether the setting is on.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::store_access_lapsed PS refused the app
  ///     token after renewal.
  /// @retval Unanswered::Reason Any failure of the renewal or the request.
  [[nodiscard]] auto cloud_capture_download(
      net::http::Client& http, std::size_t maximum_response_bytes =
                                   net::http::default_maximum_response_bytes)
      -> Expected<bool, Unanswered>;

 private:
  /// @brief Reads the cloud game feature and, when needed, the first
  /// PlayStation Plus service, and judges them.
  ///
  /// @param http The client the requests are sent through.
  /// @return The verdict.
  /// @retval Unanswered The feature could not be read, or the PlayStation
  ///     Plus read was cancelled.
  [[nodiscard]] auto read_cloud_streaming(net::http::Client& http)
      -> Expected<CloudStreaming, Unanswered>;

  /// @brief Turns the cloud game feature reply into benefits, stamped with
  /// the wall clock's time now.
  ///
  /// @param feature The reply.
  /// @return The benefits; empty plan ids are dropped.
  [[nodiscard]] auto benefits_of(
      const psn::rest::CloudGameFeature& feature) const -> CloudBenefits;

  /// @brief Reads PS's `authorization` field.
  ///
  /// @param stated The field, if present.
  /// @return granted for true, denied for false, unstated when absent.
  [[nodiscard]] static auto authorisation_of(
      const std::optional<bool>& stated) noexcept
      -> CloudBenefits::Authorisation;

  /// @brief Turns the subscriptions reply's service state into a
  /// PlusService.
  ///
  /// @param state The service as PS described it.
  /// @return The service.
  [[nodiscard]] static auto plus_service_of(
      const psn::rest::PlusServiceState& state) -> PlusService;

  /// @brief Opens a LegacyCloudStream catalogue store session; the request
  /// behind old_service_store().
  ///
  /// @param http The client the requests are sent through.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  /// @return The store on its open session, shared with concurrent callers.
  /// @retval Unanswered The failures old_service_store() lists.
  [[nodiscard]] auto opened_old_service_store(
      net::http::Client& http, std::size_t maximum_response_bytes)
      -> Expected<std::shared_ptr<const psn::rest::KamajiClient>, Unanswered>;

  /// @brief Opens the shared request helpers on this user.
  ///
  /// @return A view of the same user.
  auto request() const noexcept -> AuthorisedRequest;

  /// @brief The user the calls are made for.
  PsUser::Core& user_;
};

}  // namespace libchiaking::users
