/// @file
/// @brief Declares Authenticator, which turns a user's NPSSO into the OAuth
/// tokens and authorisation codes the commerce service, the LegacyCloudStream
/// (Kamaji) store and GK cloud streaming each accept.
///
/// Each credential is an authorize request to `ca.account.sony.com` with the
/// `npsso` cookie and `prompt=none`; the credential comes back in the 302's
/// `Location`. Tokens may be cached in the session's TokenCache and
/// renewed once when a service refuses them; codes are single-use and always
/// asked afresh.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/auth_error.hpp"
#include "src/core/psn/auth/authorization.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/auth/duid.hpp"
#include "src/core/psn/auth/gk_ids.hpp"
#include "src/core/psn/auth/token_cache.hpp"
#include "src/core/psn/types.hpp"

#include <libchiaking/detail/expected.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace libchiaking::psn::auth {

/// @brief The scopes of the commerce token that reads the account's owned
/// entitlement records (library_grant()).
inline constexpr std::string_view owned_entitlements_scope =
    "kamaji:get_internal_entitlements user:account.attributes.validate";

/// @brief The two authorisation codes a GK cloud session is started with.
/// Plain data; both are secret and single-use.
struct GkCodes {
  /// @brief The code issued to GK's `gkClientId`, proving the user to the
  /// streaming client.
  AuthCode client;
  /// @brief The code issued to the stream server (`streamServerClientId` on
  /// PS5 cloud streaming, `ps3GkClientId` on LegacyCloudStream).
  AuthCode server;
};

/// @brief Which of the two GK codes is about to be asked for, so a launch
/// can report its progress and attribute a failure.
enum class GkStep : std::uint8_t {
  /// @brief The client code.
  client,
  /// @brief The server code.
  server,
};

/// @brief One user's authorisation requests, spending one NPSSO.
///
/// Short-lived: made for one task with a copy of the user's NPSSO and, when
/// tokens should be shared across tasks, the session's TokenCache. It
/// remembers every token it handed out so that a service's refusal of one can
/// be answered with refresh(), record_use() or forget().
///
/// Every request blocks the calling thread for one round trip, within the HTTP
/// client's timeouts, and stops early when its stop token fires. Not
/// thread-safe: use one object from one thread at a time; the cache it refers
/// to is thread-safe and may be shared.
class Authenticator {
 public:
  /// @brief Prepares the requests for one user.
  ///
  /// @param client The client requests and logs go through; must outlive this
  ///     object.
  /// @param npsso The user's NPSSO.
  /// @param cache The session's token cache, which must outlive this object;
  ///     when null every token is exchanged afresh and none can be renewed.
  Authenticator(net::http::Client& client, Npsso npsso,
                TokenCache* cache = nullptr);
  Authenticator(const Authenticator&) = delete;
  auto operator=(const Authenticator&) -> Authenticator& = delete;
  /// @brief Wipes the NPSSO and the remembered tokens.
  ~Authenticator();

  /// @brief Names the grant of the owned-entitlements commerce token, to look
  /// it up in a TokenCache.
  ///
  /// @return The LegacyCloudStream commerce client with
  /// owned_entitlements_scope, as a
  ///     token.
  static auto library_grant() -> Grant;
  /// @brief Names the grant of the LegacyCloudStream checkout token, to look it
  /// up in a TokenCache.
  ///
  /// @return The LegacyCloudStream commerce client with the checkout scopes, as
  /// a token.
  static auto old_service_checkout_grant() -> Grant;
  /// @brief Obtains a commerce token for the LegacyCloudStream commerce client.
  ///
  /// GET `/api/v1/oauth/authorize?response_type=token` with the given scopes
  /// and the LegacyCloudStream redirect URI, sending the browser user agent
  /// after the cookie; the token is the `access_token` of the redirect. Served
  /// from the cache when a fresh token covers the scopes. The token is
  /// remembered for refresh().
  ///
  /// @param scope The scopes, such as owned_entitlements_scope.
  /// @return The token.
  /// @retval AuthErrorKind::cancelled The stop token fired.
  /// @retval AuthErrorKind::invalid_npsso The NPSSO is unusable.
  /// @retval AuthErrorKind::invalid_request The URL is too long or not
  ///     printable ASCII.
  /// @retval AuthErrorKind::refused The server redirected with an `error`.
  /// @retval AuthErrorKind::rejected The server answered other than 302 and
  ///     not with a server error.
  /// @retval AuthErrorKind::server_error The server answered with a 5xx.
  /// @retval AuthErrorKind::malformed The redirect carried no readable token,
  ///     or one longer than token_capacity less one bytes.
  /// @retval AuthErrorKind::transport The request did not complete.
  [[nodiscard]] auto commerce_token(std::string_view scope)
      -> Expected<BearerToken, AuthError>;
  /// @brief Obtains the token the LegacyCloudStream checkout (subscription
  /// purchase) reads entitlements, privacy settings and Premium status with.
  ///
  /// The same commerce client as commerce_token(), but with the checkout
  /// scopes, the LegacyCloudStream sign-in page and popup parameters, the DUID,
  /// values sent verbatim, and the LegacyCloudStream user agent sent first.
  /// Cached and remembered like commerce_token().
  ///
  /// @param duid The device's DUID, sent as given.
  /// @return The token.
  /// @retval AuthError The failures commerce_token() lists.
  [[nodiscard]] auto legacy_cloud_stream_checkout_token(std::string_view duid)
      -> Expected<BearerToken, AuthError>;
  /// @brief Asks for an authorisation code that opens a LegacyCloudStream
  /// (Kamaji) catalogue store session.
  ///
  /// GET `/api/v1/oauth/authorize?response_type=code` for the LegacyCloudStream
  /// client with the store scopes, the sign-in page parameters and the DUID,
  /// values percent-encoded; the code is the `code` of the redirect. Never
  /// cached.
  ///
  /// @param duid The device's DUID.
  /// @return The code.
  /// @retval AuthError The failures commerce_token() lists, malformed meaning
  ///     the redirect does not lead to the LegacyCloudStream redirect URI with
  ///     a query carrying exactly one readable code.
  [[nodiscard]] auto legacy_cloud_stream_catalog_code(std::string_view duid)
      -> Expected<AuthCode, AuthError>;
  /// @brief Asks for an authorisation code that opens the Kamaji store
  /// session a LegacyCloudStream title launch starts from.
  ///
  /// As legacy_cloud_stream_catalog_code(), but with the values sent verbatim,
  /// as the PS Now app sends them.
  ///
  /// @param duid The device's DUID.
  /// @return The code.
  /// @retval AuthError The failures legacy_cloud_stream_catalog_code() lists.
  [[nodiscard]] auto legacy_cloud_stream_title_code(std::string_view duid)
      -> Expected<AuthCode, AuthError>;
  /// @brief Asks for the authorisation code that opens the account's Kamaji
  /// session for a LegacyCloudStream launch.
  ///
  /// As legacy_cloud_stream_title_code() without the sign-in page parameters; a
  /// PS3 game asks only `kamaji:commerce_native`.
  ///
  /// @param duid The device's DUID.
  /// @param platform The console the game runs on.
  /// @return The code.
  /// @retval AuthError The failures legacy_cloud_stream_catalog_code() lists.
  [[nodiscard]] auto legacy_cloud_stream_account_code(
      std::string_view duid, StreamingPlatform platform)
      -> Expected<AuthCode, AuthError>;
  /// @brief Asks for the client and server codes a GK cloud session
  /// starts with; gk_codes() without progress reports.
  ///
  /// @param service The streaming service.
  /// @param platform The console the game runs on.
  /// @param ids The client ids GK assigned.
  /// @param duid The device's DUID.
  /// @return The two codes.
  /// @retval AuthError The failures of the five-argument overload.
  [[nodiscard]] auto gk_codes(StreamingService service,
                              StreamingPlatform platform,
                              const GkClientIds& ids, std::string_view duid)
      -> Expected<GkCodes, AuthError>;
  /// @brief Asks for the client and server codes a GK cloud session
  /// starts with, one after the other.
  ///
  /// Both are `response_type=code` authorize requests with the service's
  /// route: PS5 cloud streaming uses the v3 endpoint, the `gaikai://local`
  /// redirect, its user agent and the `qlite` application; LegacyCloudStream
  /// uses the v1 endpoint, its redirect, its user agent, the
  /// `legacy_cloud_stream` application and its sign-in page parameters. The
  /// client code is issued to `gk_client_id`; the server code to
  /// `stream_server_client_id` (PS5 cloud streaming, scope including
  /// `oauth:create_authn_ticket_for_cloud_console_signin`) or
  /// `ps3_gk_client_id` (LegacyCloudStream, scope `sso:none`, or
  /// `kamaji:commerce_native` and no DUID for a PS3 game). The replies are
  /// kept even if the stop token fires mid-request.
  ///
  /// @param service The streaming service.
  /// @param platform The console the game runs on; PS5 cloud streaming
  ///     supports only PS5.
  /// @param ids The client ids GK assigned.
  /// @param duid The device's DUID.
  /// @param before Called with each step just before its request; may be
  ///     empty.
  /// @return The two codes.
  /// @retval AuthErrorKind::invalid_request The service does not stream the
  ///     platform, the DUID is malformed, or a client id is empty, longer
  ///     than gk_id_max_length or not URL-unreserved characters, or a URL
  ///     is too long; nothing was sent.
  /// @retval AuthError The failures legacy_cloud_stream_catalog_code() lists,
  ///     with the service's redirect URI, for whichever code failed; the
  ///     server code is not asked when the client code fails.
  [[nodiscard]] auto gk_codes(StreamingService service,
                              StreamingPlatform platform,
                              const GkClientIds& ids, std::string_view duid,
                              const std::function<void(GkStep)>& before)
      -> Expected<GkCodes, AuthError>;
  /// @brief Asks the authorisation server whether it would authorise this
  /// device for the service without an interactive sign-in, before a cloud
  /// launch spends codes.
  ///
  /// POST `/api/authz/v3/oauth/authorizeCheck` with the `npsso` cookie and a
  /// JSON body naming the service's client id, scopes, redirect URI,
  /// `response_type` `code`, the PSN service entity and the DUID. Any 2xx is
  /// a yes; the body is not read. The reply is kept even if the stop token
  /// fires mid-request.
  ///
  /// @param service The streaming service.
  /// @param duid The device's DUID.
  /// @return Nothing, when the server answered with a success status.
  /// @retval AuthErrorKind::cancelled The stop token fired, possibly before
  ///     anything was sent.
  /// @retval AuthErrorKind::invalid_npsso The NPSSO is unusable.
  /// @retval AuthErrorKind::invalid_request The DUID is malformed or the body
  ///     could not be written.
  /// @retval AuthErrorKind::rejected The server answered with a non-success
  ///     status.
  /// @retval AuthErrorKind::transport The request did not complete.
  [[nodiscard]] auto authorize_check(StreamingService service,
                                     std::string_view duid)
      -> Expected<void, AuthError>;
  /// @brief Replaces a token a service refused, once.
  ///
  /// Counts a retry against the grant that served it, forgets it from the
  /// cache, and repeats the request that issued it; the new token is fresh
  /// from the server and is not renewable() again.
  ///
  /// @param refused A token this object handed out.
  /// @param http_status The refusal's status, for the log.
  /// @return The new token.
  /// @retval AuthErrorKind::invalid_request This object did not hand out
  ///     `refused`.
  /// @retval AuthError The failures of the request that issued it.
  [[nodiscard]] auto refresh(const BearerToken& refused,
                             std::uint16_t http_status)
      -> Expected<BearerToken, AuthError>;
  /// @brief Tells whether refresh() is worth trying for a refused token.
  ///
  /// @param token A token.
  /// @return True when this object handed it out from the cache (not freshly
  ///     exchanged), a cache is in use and no stop has been requested.
  auto renewable(const BearerToken& token) const noexcept -> bool;
  /// @brief Records in the cache that a request used a token, so its
  /// statistics and refusal count stay current; does nothing without a cache
  /// or for a token this object did not hand out.
  ///
  /// @param token The token the request carried.
  /// @param http_status The reply's status.
  void record_use(const BearerToken& token, std::uint16_t http_status);
  /// @brief Forgets a refused token from the cache without renewing it; does
  /// nothing without a cache, for an empty token, or for one this object did
  /// not hand out.
  ///
  /// @param refused The token.
  void forget(const BearerToken& refused);

 private:
  /// @brief The log severities.
  using LogSeverity = platform::LogSeverity;
  /// @brief The order of the user agent and cookie headers.
  using HeaderOrder = Authorization::HeaderOrder;

  /// @brief The OAuth v1 authorize endpoint, used for the commerce tokens,
  /// the LegacyCloudStream codes and LegacyCloudStream GK codes.
  static constexpr std::string_view authorize_url =
      "https://ca.account.sony.com/api/v1/oauth/authorize";
  /// @brief The v3 `authorizeCheck` endpoint.
  static constexpr std::string_view authorize_check_url =
      "https://ca.account.sony.com/api/authz/v3/oauth/authorizeCheck";

  static_assert(authorize_url.starts_with(Authorization::account_api));
  static_assert(authorize_check_url.starts_with(Authorization::authorize_url));

  /// @brief The LegacyCloudStream commerce client id the commerce and checkout
  /// tokens are issued to.
  static constexpr std::string_view legacy_cloud_stream_commerce_client_id =
      "dc523cc2-b51b-4190-bff0-3397c06871b3";
  /// @brief The PS5 cloud streaming OAuth client id, named in its
  /// authorizeCheck.
  static constexpr std::string_view cloud_stream_client_id =
      "19ae39c4-3f88-4d11-a792-94e4f52c996d";

  /// @brief The scopes of the LegacyCloudStream checkout token: the
  /// owned-entitlement scopes, privacy settings and the Premium subscription.
  static constexpr std::string_view legacy_cloud_stream_checkout_scope =
      "kamaji:get_internal_entitlements user:account.attributes.validate "
      "kamaji:get_privacy_settings "
      "user:account.settings.privacy.get kamaji:s2s.subscriptionsPremium.get";

  /// @brief The scopes of the LegacyCloudStream store codes: native commerce,
  /// store containers, lists and the Premium subscription.
  static constexpr std::string_view legacy_cloud_stream_store_scope =
      "kamaji:commerce_native kamaji:commerce_container kamaji:lists "
      "kamaji:s2s.subscriptionsPremium.get";
  /// @brief The scope of a PS3 game's account code and GK server code.
  static constexpr std::string_view legacy_cloud_stream_ps3_store_scope =
      "kamaji:commerce_native";

  /// @brief The scopes of the LegacyCloudStream GK client code, including the
  /// first-play entitlement update.
  static constexpr std::string_view legacy_cloud_stream_gk_client_scope =
      "kamaji:commerce_native versa:user_update_entitlements_first_play "
      "kamaji:lists";
  /// @brief The scope of the LegacyCloudStream GK server code for PS4 and PS5
  /// games.
  static constexpr std::string_view legacy_cloud_stream_gk_server_scope =
      "sso:none";

  /// @brief The scopes of the PS5 cloud streaming client code: OpenID claims
  /// (DUID, online id), the Premium subscription and server-to-server PSN.
  static constexpr std::string_view cloud_stream_client_scope =
      "id_token:psn.basic_claims kamaji:s2s.subscriptionsPremium.get "
      "id_token:duid id_token:online_id openid psn:s2s";

  /// @brief The scopes of the PS5 cloud streaming server code, which lets the
  /// cloud console sign in as the user.
  static constexpr std::string_view cloud_stream_server_scope =
      "id_token:duid id_token:online_id openid "
      "oauth:create_authn_ticket_for_cloud_console_signin";
  /// @brief The diagnostic name of the owned-entitlements token.
  static constexpr std::string_view owned_entitlements_token_label =
      "owned entitlements";
  /// @brief The diagnostic name of the LegacyCloudStream checkout token.
  static constexpr std::string_view legacy_cloud_stream_checkout_token_label =
      "ps now commerce";
  /// @brief The longest authorisation code read from a redirect, in bytes.
  static constexpr std::size_t code_capacity = 1024;
  /// @brief One more than the longest access token accepted from a
  /// redirect, in bytes; a longer token is refused, not cut.
  static constexpr std::size_t token_capacity = 2048;
  /// @brief The longest `error` value read from a redirect, in bytes.
  static constexpr std::size_t error_capacity = 128;
  /// @brief The longest `Location` header accepted, in bytes (16 KiB).
  static constexpr std::size_t location_limit = 16 * 1024;
  /// @brief The most digits of `expires_in` read.
  static constexpr std::size_t expires_in_digits = 10;

  /// @brief The commerce and checkout token requests, recorded as
  /// `psn.auth.commerce_token`.
  static constexpr net::http::Endpoint commerce_token_endpoint{
      .label = "psn.auth.commerce_token",
      .maximum_response_bytes = json::max_document_bytes,
  };

  /// @brief The LegacyCloudStream store code requests.
  static constexpr net::http::Endpoint legacy_cloud_stream_code_endpoint{
      .label = "psn.auth.legacy_cloud_stream_code",
      .maximum_response_bytes = json::max_document_bytes,
  };

  /// @brief The GK client code request, whose reply is kept after a stop.
  static constexpr net::http::Endpoint gk_client_code_endpoint{
      .label = "psn.auth.gk_client_code",
      .keep_reply_after_stop = true,
      .maximum_response_bytes = json::max_document_bytes,
  };

  /// @brief The GK server code request, whose reply is kept after a stop.
  static constexpr net::http::Endpoint gk_server_code_endpoint{
      .label = "psn.auth.gk_server_code",
      .keep_reply_after_stop = true,
      .maximum_response_bytes = json::max_document_bytes,
  };

  /// @brief The authorizeCheck POST, whose reply is kept after a stop.
  static constexpr net::http::Endpoint authorize_check_endpoint{
      .label = "psn.auth.authorize_check",
      .method = net::http::Method::post,
      .keep_reply_after_stop = true,
      .maximum_response_bytes = json::max_document_bytes,
  };

  /// @brief How query values are written into an authorize URL.
  enum class Encoding : std::uint8_t {
    /// @brief Percent-encoded, as a browser would.
    percent,
    /// @brief As given, with spaces in the scope written as `%20`.
    verbatim,
  };

  /// @brief The application parameters an authorize URL names the calling app
  /// with.
  struct Application {
    /// @brief The `smcid` parameter.
    std::string_view smcid;
    /// @brief The `applicationId` parameter.
    std::string_view application_id;
    /// @brief The `mid` parameter.
    std::string_view mid;
  };

  /// @brief The LegacyCloudStream application.
  static constexpr Application legacy_cloud_stream_application{
      "pc:psnow", "psnow", "PSNOW"};
  /// @brief The PS5 cloud streaming application (`qlite`).
  static constexpr Application qlite_application{"qlite", "qlite", "qlite"};

  /// @brief How one streaming service asks for its GK codes.
  struct GkRoute {
    /// @brief The authorize endpoint.
    std::string_view authorize_url;
    /// @brief The redirect URI registered for the service's clients.
    std::string_view redirect_uri;
    /// @brief The service's user agent.
    std::string_view user_agent;
    /// @brief The application parameters.
    Application application;
    /// @brief The client code's scopes.
    std::string_view client_scope;
    /// @brief Whether the LegacyCloudStream sign-in page and popup parameters
    /// are added.
    bool sign_in_page{};
  };

  /// @brief Builds an authorize URL in a chosen Encoding.
  class AuthorizeUrl;
  /// @brief What a 302 from the authorize endpoint carried: the code or
  /// token, `expires_in` and the status.
  struct Authorized;
  /// @brief Everything one authorize request needs: endpoint, grant, URL,
  /// redirect URI, user agent, header order and diagnostic label; repeatable
  /// for refresh().
  struct AuthorizationFlow;
  /// @brief A token this object handed out, with the grant that served it and
  /// the flow that obtained it.
  struct IssuedToken;
  /// @brief The JSON body of an authorizeCheck request.
  struct AuthorizeCheckBody;

  /// @brief Tells whether a byte is URL-unreserved.
  ///
  /// @param value The byte.
  /// @return True for ASCII letters, digits, `-`, `_`, `.` and `~`.
  static auto unreserved(char value) noexcept -> bool;
  /// @brief Checks a GK client id before it is put in a URL verbatim.
  ///
  /// @param id The id.
  /// @return True when it is not empty, at most gk_id_max_length bytes
  ///     and all unreserved.
  static auto valid_client_id(std::string_view id) noexcept -> bool;
  /// @brief Tells whether a service streams games of a platform.
  ///
  /// @param service The service.
  /// @param platform The platform.
  /// @return True for PS5 on PS5 cloud streaming and for PS3, PS4 and PS5 on
  ///     LegacyCloudStream.
  static auto supported(StreamingService service,
                        StreamingPlatform platform) noexcept -> bool;
  /// @brief Tells whether a credential read from a redirect is printable
  /// ASCII.
  ///
  /// @param value The credential.
  /// @return True when every byte is visible ASCII; true for empty text.
  static auto visible(std::string_view value) noexcept -> bool;
  /// @brief Strips spaces and tabs from both ends.
  ///
  /// @param value The text.
  /// @return A view into `value`, empty when it was all blanks.
  static auto trimmed(std::string_view value) noexcept -> std::string_view;
  /// @brief Picks a service's GK route.
  ///
  /// @param service The service.
  /// @return The PS5 cloud streaming route for cloud_stream; the
  /// LegacyCloudStream route
  ///     otherwise.
  static auto gk_route(StreamingService service) noexcept -> const GkRoute&;

  /// @brief Reads the access token from a redirect, as written.
  ///
  /// @param location The `Location` header.
  /// @return The first `access_token` value, empty when there is none;
  ///     nothing when it is longer than token_capacity less one bytes.
  [[nodiscard]] static auto redirect_token(std::string_view location)
      -> std::optional<SecretString>;
  /// @brief Reads the authorisation code from a redirect to the flow's
  /// redirect URI.
  ///
  /// @param location The `Location` header.
  /// @param redirect_uri The redirect URI the request named.
  /// @return The decoded code; nothing when the location does not lead to
  ///     @p redirect_uri with a query, or the query does not carry exactly
  ///     one `code` of at most code_capacity less one bytes.
  [[nodiscard]] static auto redirect_code(std::string_view location,
                                          std::string_view redirect_uri)
      -> std::optional<SecretString>;
  /// @brief Reads `expires_in` from a redirect.
  ///
  /// @param location The `Location` header.
  /// @return The lifetime in seconds, capped at the largest 32-bit value; 0
  ///     when absent, longer than expires_in_digits or not a number.
  static auto location_expires_in(std::string_view location) -> std::uint32_t;
  /// @brief Describes a grant for the log.
  ///
  /// @param grant The grant.
  /// @return `client=<id> scope="<scopes>"`; holds no secret.
  static auto grant_text(const Grant& grant) -> std::string;
  /// @brief Translates a failure to send.
  ///
  /// @param fault The fault.
  /// @return The AuthErrorKind of the same name.
  static auto failure_of(Authorization::Fault fault) noexcept -> AuthError;

  /// @brief Assembles a flow.
  ///
  /// @param endpoint The endpoint the request is recorded under.
  /// @param client_id The client the credential is issued to.
  /// @param scope The scopes asked for.
  /// @param response Whether a token or a code is asked for.
  /// @param url The URL, which is consumed.
  /// @param redirect_uri The redirect URI the URL names.
  /// @param user_agent The user agent sent.
  /// @param order The header order.
  /// @param token_label The diagnostic name of a cached token; may be empty.
  /// @return The flow.
  static auto authorization(const net::http::Endpoint& endpoint,
                            std::string_view client_id, std::string_view scope,
                            ResponseType response, AuthorizeUrl url,
                            std::string_view redirect_uri,
                            std::string_view user_agent, HeaderOrder order,
                            std::string_view token_label = {})
      -> AuthorizationFlow;
  /// @brief Builds the commerce token request of commerce_token().
  ///
  /// @param scope The scopes.
  /// @return The flow, labelled as owned entitlements when the scopes are
  ///     owned_entitlements_scope.
  static auto commerce_flow(std::string_view scope) -> AuthorizationFlow;
  /// @brief Builds the checkout token request of
  /// legacy_cloud_stream_checkout_token().
  ///
  /// @param duid The DUID.
  /// @return The flow.
  static auto checkout_flow(std::string_view duid) -> AuthorizationFlow;

  /// @brief Builds a LegacyCloudStream store code request.
  ///
  /// @param duid The DUID.
  /// @param scope The scopes.
  /// @param sign_in_page Whether the sign-in page parameters are added.
  /// @param encoding How values are written.
  /// @return The flow.
  static auto legacy_cloud_stream_code_flow(std::string_view duid,
                                            std::string_view scope,
                                            bool sign_in_page,
                                            Encoding encoding)
      -> AuthorizationFlow;
  /// @brief Keeps a flow only if its URL is sendable.
  ///
  /// @param flow The flow.
  /// @return The flow, or nothing when Authorization::sendable() refuses its
  ///     URL.
  [[nodiscard]] static auto bounded(AuthorizationFlow flow)
      -> std::optional<AuthorizationFlow>;

  /// @brief Builds the GK client code request.
  ///
  /// @param service The service, which picks the route.
  /// @param client_id GK's `gkClientId`.
  /// @param duid The DUID.
  /// @return The flow; nothing when the client id is invalid or the URL is
  ///     not sendable.
  [[nodiscard]] static auto gk_client_flow(StreamingService service,
                                           std::string_view client_id,
                                           const Duid& duid)
      -> std::optional<AuthorizationFlow>;

  /// @brief Builds the GK server code request.
  ///
  /// @param service The service, which picks the route and scope.
  /// @param platform The game's platform; a PS3 game gets its own scope and
  ///     no DUID.
  /// @param client_id The stream server's client id.
  /// @param duid The DUID.
  /// @return The flow; nothing when the client id is invalid or the URL is
  ///     not sendable.
  [[nodiscard]] static auto gk_server_flow(StreamingService service,
                                           StreamingPlatform platform,
                                           std::string_view client_id,
                                           const Duid& duid)
      -> std::optional<AuthorizationFlow>;

  /// @brief Sends a flow's request and reads the redirect.
  ///
  /// Blocks for one round trip.
  ///
  /// @param flow The request.
  /// @param npsso The NPSSO to send.
  /// @return The code or token, `expires_in` and the status.
  /// @retval AuthError The failures commerce_token() lists.
  [[nodiscard]] auto authorize(const AuthorizationFlow& flow,
                               const Npsso& npsso)
      -> Expected<Authorized, AuthError>;
  /// @brief Exchanges the NPSSO for a token, logging why at debug level.
  ///
  /// @param flow The request.
  /// @param npsso The NPSSO to send.
  /// @param occasion Why the exchange is made, for the log.
  /// @return The token, its stated lifetime and the status.
  /// @retval AuthError The failures of authorize().
  [[nodiscard]] auto exchange(const AuthorizationFlow& flow, const Npsso& npsso,
                              std::string_view occasion)
      -> Expected<Exchanged, AuthError>;
  /// @brief Obtains a token for a flow, through the cache when there is one.
  ///
  /// @param flow The request.
  /// @return The lease.
  /// @retval AuthError The failures of TokenCache::acquire() or exchange().
  [[nodiscard]] auto lease(const AuthorizationFlow& flow)
      -> Expected<Lease, AuthError>;
  /// @brief Obtains a token, remembers it for refresh(), and logs a failure.
  ///
  /// @param flow The request, kept with the token.
  /// @return The token.
  /// @retval AuthError The failures of lease().
  [[nodiscard]] auto token(AuthorizationFlow flow)
      -> Expected<BearerToken, AuthError>;
  /// @brief Obtains an authorisation code, always from the server, and logs a
  /// failure.
  ///
  /// @param flow The request.
  /// @return The code.
  /// @retval AuthError The failures of authorize().
  [[nodiscard]] auto code(const AuthorizationFlow& flow)
      -> Expected<AuthCode, AuthError>;
  /// @brief Finds a token this object handed out, comparing in constant time,
  /// newest first.
  ///
  /// @param token The token.
  /// @return The record, or null when unknown.
  auto issued(const BearerToken& token) const noexcept -> IssuedToken*;
  /// @brief Records a handed-out token, replacing an earlier one from the
  /// same endpoint and grant.
  ///
  /// @param issued The record.
  void remember(IssuedToken issued);
  /// @brief Logs a failed step with its kind and status.
  ///
  /// @param label The endpoint label.
  /// @param error The failure.
  void report(std::string_view label, const AuthError& error) const;

  /// @brief The client the requests go through.
  net::http::Client& client_;
  /// @brief Sends the authorize requests.
  Authorization authorization_;
  /// @brief The user's NPSSO.
  Npsso npsso_;
  /// @brief The session's token cache; null when tokens are not cached.
  TokenCache* cache_;
  /// @brief The tokens handed out, oldest first, one per endpoint and grant.
  std::vector<std::unique_ptr<IssuedToken>> issued_;
};

}  // namespace libchiaking::psn::auth
