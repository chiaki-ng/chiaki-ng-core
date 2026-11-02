/// @file
/// @brief Declares QrSignIn, the protocol client of PSN's password-free
/// sign-in, in which another device approves a code shown as a QR code and
/// PSN answers with an NPSSO.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/authorization.hpp"
#include "src/core/psn/auth/credentials.hpp"

#include <libchiaking/detail/secret_string.hpp>
#include <libchiaking/platform/runtime/stop_token.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace libchiaking::platform {
class ClockProvider;
class DiagnosticsProvider;
class HttpProvider;
}  // namespace libchiaking::platform

namespace libchiaking::psn::auth {

/// @brief Tag for the client secret of the hosted sign-in page's
/// registration. Never sent as a header.
struct ClientSecretKind {
  /// @brief Name used for the credential in diagnostics.
  static constexpr std::string_view label = "client secret";
  /// @brief The longest secret accepted, in bytes.
  static constexpr std::size_t maximum_bytes = 1024;
  /// @brief Recorded in diagnostics as client credentials.
  static constexpr platform::HttpCredential credential =
      platform::HttpCredential::client_credentials;
};

/// @brief Tag for the application token that authorises the ticket and its
/// polls.
struct ApplicationTokenKind {
  /// @brief Name used for the credential in diagnostics.
  static constexpr std::string_view label = "application token";
  /// @brief Sent as an `Authorization: Bearer` header.
  static constexpr net::http::CredentialPlacement placement{"Authorization",
                                                            "Bearer "};
  /// @brief The longest token accepted; see token_maximum_bytes.
  static constexpr std::size_t maximum_bytes = token_maximum_bytes;
  /// @brief Recorded in diagnostics as client credentials.
  static constexpr platform::HttpCredential credential =
      platform::HttpCredential::client_credentials;
};

/// @brief Tag for the private handle a ticket is polled by. Never sent as a
/// header and never shown.
struct TicketHandleKind {
  /// @brief Name used for the credential in diagnostics.
  static constexpr std::string_view label = "authentication ticket";
  /// @brief The longest handle accepted; see token_maximum_bytes.
  static constexpr std::size_t maximum_bytes = token_maximum_bytes;
  /// @brief Recorded in diagnostics as a sign-in grant.
  static constexpr platform::HttpCredential credential =
      platform::HttpCredential::sign_in_grant;
};

/// @brief The registration's client secret. Secret; held in wiping memory.
using ClientSecret = net::http::Credential<ClientSecretKind>;
/// @brief The application token. Secret; held in wiping memory.
using ApplicationToken = net::http::Credential<ApplicationTokenKind>;
/// @brief A ticket's polling handle. Secret; held in wiping memory.
using TicketHandle = net::http::Credential<TicketHandleKind>;

/// @brief The six decimal digits PSN shows beside a ticket on the approving
/// device.
///
/// Valid from construction: only parse() makes one. Held in wiping memory,
/// since anyone who reads it can approve the ticket's sign-in.
class UserCode {
 public:
  /// @brief The number of digits in every code.
  static constexpr std::size_t digits = 6;

  /// @brief Reads a code.
  ///
  /// @param text The code as PSN sent it.
  /// @return The code, leading zeroes kept, or nothing unless the text is
  ///     exactly six decimal digits.
  [[nodiscard]] static auto parse(std::string_view text)
      -> std::optional<UserCode>;

  /// @brief The digits.
  ///
  /// @return A view valid while this object lives.
  auto view() const noexcept -> std::string_view { return text_.view(); }

 private:
  /// @brief Holds checked digits.
  ///
  /// @param text Six decimal digits.
  explicit UserCode(std::string_view text) : text_(text) {}

  /// @brief The digits.
  SecretString text_;
};

/// @brief What the hosted sign-in page registers its password-free sign-in
/// as.
struct HostedRegistration {
  /// @brief The OAuth client id.
  std::string client_id;
  /// @brief The OAuth client secret. Secret.
  ClientSecret client_secret;
  /// @brief The scopes the application token is asked for, separated by
  /// spaces.
  std::string scope;
};

/// @brief A ticket PSN issued: the code to show and the handle to poll by.
struct Ticket {
  /// @brief The handle the ticket is polled by. Secret.
  TicketHandle handle;
  /// @brief The digits shown on the approving device.
  UserCode user_code;
  /// @brief The correlation id PSN gave the ticket, which the approval URL
  /// carries.
  std::string correlation_id;
  /// @brief How long to wait before each poll.
  std::chrono::seconds interval;
  /// @brief How long the ticket lasts from the moment it was issued.
  std::chrono::seconds expires_in;
};

/// @brief The ticket was approved; PSN issued an NPSSO.
struct TicketApproved {
  /// @brief The sign-in seed. Secret.
  Npsso npsso;
};
/// @brief Nobody has approved the ticket yet.
struct TicketPending {};
/// @brief PSN asked for the polls to slow down.
struct TicketSlowDown {
  /// @brief How long PSN asked to wait before the next poll.
  std::chrono::seconds retry_after;
};
/// @brief The person refused the sign-in on the approving device.
struct TicketDeclined {};
/// @brief The ticket expired or is no longer valid; a replacement is needed.
struct TicketLapsed {};
/// @brief PSN wants additional verification: a fresh application token, then
/// a replacement ticket.
struct TicketNeedsVerification {};

/// @brief Every answer a poll can bring that is not a failure.
using PollAnswer =
    std::variant<TicketApproved, TicketPending, TicketSlowDown, TicketDeclined,
                 TicketLapsed, TicketNeedsVerification>;

/// @brief Speaks PSN's password-free sign-in, one member per step.
///
/// registration() reads the hosted page's client registration,
/// application_token() turns it into an application token, ticket() opens a
/// ticket, approval_url() shows it and poll() asks whether it was approved.
/// Each member makes its requests on the calling thread and blocks until they
/// finish. Nothing is stored and no secret is logged.
class QrSignIn {
 public:
  /// @brief The scope the application token must be granted.
  static constexpr std::string_view required_scope =
      "oauth:create_authn_ticket_for_web_signin";

  /// @brief Creates the protocol client.
  ///
  /// @param http Carries the requests. Must outlive this object.
  /// @param clock Monotonic clock request deadlines are measured on. Must
  ///     outlive this object.
  /// @param stop Cancels every request this object makes.
  /// @param log Receives diagnostics. May be null.
  QrSignIn(platform::HttpProvider& http, const platform::ClockProvider& clock,
           platform::StopToken stop,
           platform::DiagnosticsProvider* log = nullptr);

  /// @brief Reads the registration from the hosted sign-in page and its
  /// application bundle.
  ///
  /// @return The client id, client secret and scopes.
  /// @retval SignInErrorKind::layout_changed The page or the bundle does not
  ///     have the expected shape, or the page names a bundle outside the
  ///     account site.
  /// @retval SignInErrorKind::cancelled The stop token was triggered.
  /// @retval SignInErrorKind::transport A request did not complete.
  /// @retval SignInErrorKind::unavailable PSN answered with an unexpected
  ///     status.
  [[nodiscard]] auto registration()
      -> Expected<HostedRegistration, SignInError>;
  /// @brief Obtains an application token for the registration.
  ///
  /// @param registration What registration() read.
  /// @return The token.
  /// @retval SignInErrorKind::rate_limited PSN is rate limiting sign-in.
  /// @retval SignInErrorKind::malformed The reply could not be read.
  /// @retval SignInError Any other failure, as registration() reports it.
  [[nodiscard]] auto application_token(const HostedRegistration& registration)
      -> Expected<ApplicationToken, SignInError>;
  /// @brief Opens a ticket for another device to approve.
  ///
  /// The request names this device, and the approving device shows that
  /// name. PSN refuses a ticket request without one.
  ///
  /// @param token The application token.
  /// @param device_name The name to show, in UTF-8.
  /// @return The ticket.
  /// @retval SignInErrorKind::invalid_request The token is not usable, or
  ///     the name cannot be written. Nothing was sent.
  /// @retval SignInErrorKind::malformed The reply lacks a usable handle,
  ///     code, interval, lifetime or correlation id.
  /// @retval SignInError Any other failure, as application_token() reports
  ///     it.
  [[nodiscard]] auto ticket(const ApplicationToken& token,
                            std::string_view device_name)
      -> Expected<Ticket, SignInError>;
  /// @brief Asks PSN whether a ticket has been approved.
  ///
  /// @param token The application token the ticket was opened with.
  /// @param handle The ticket's handle.
  /// @return What PSN answered.
  /// @retval SignInErrorKind::rate_limited PSN refused the poll without
  ///     saying how long to wait.
  /// @retval SignInErrorKind::unavailable PSN answered with a status or
  ///     error code the sign-in does not expect.
  /// @retval SignInErrorKind::malformed The reply could not be read.
  /// @retval SignInError Any other failure, as ticket() reports it.
  [[nodiscard]] auto poll(const ApplicationToken& token,
                          const TicketHandle& handle)
      -> Expected<PollAnswer, SignInError>;
  /// @brief The address another device opens to approve a ticket, shown as
  /// the QR code.
  ///
  /// @param ticket The ticket.
  /// @return The URL, in wiping memory since it carries the user code.
  [[nodiscard]] static auto approval_url(const Ticket& ticket) -> SecretString;

 private:
  /// @brief The members of an application token reply that are read.
  struct TokenReply;
  /// @brief The members of a ticket reply that are read.
  struct TicketReply;
  /// @brief The members of an approved poll reply that are read.
  struct ApprovalReply;
  /// @brief The members of a refused poll reply that are read.
  struct RefusalReply;

  /// @brief The body of a ticket request.
  struct TicketBody {
    /// @brief The name the approving device shows.
    std::string_view device_name;
  };

  /// @brief The body of a poll.
  struct PollBody {
    /// @brief Always `authentication_ticket`.
    std::string_view authentication_type;
    /// @brief The ticket's handle. Secret.
    SecretString authentication_ticket;
  };

  /// @brief The hosted sign-in page.
  static constexpr std::string_view page_url =
      "https://my.account.sony.com/sonyacct/signin/";
  /// @brief The account site the page and its bundle must come from.
  static constexpr std::string_view account_site = "my.account.sony.com";
  /// @brief The path every application bundle ends with.
  static constexpr std::string_view bundle_suffix = "/assets/app.bundle.js";
  /// @brief The OAuth token endpoint.
  static constexpr std::string_view token_url =
      "https://ca.account.sony.com/api/authz/v3/oauth/token";
  /// @brief The endpoint tickets are opened at.
  static constexpr std::string_view ticket_url =
      "https://ca.account.sony.com/api/authn/v3/sso/ticket/webSignin";
  /// @brief The endpoint tickets are polled at.
  static constexpr std::string_view poll_url =
      "https://ca.account.sony.com/api/authn/v3/sso";
  /// @brief The page another device approves a ticket at.
  static constexpr std::string_view approval_page =
      "https://id.sonyentertainmentnetwork.com/id/PSsignin/";
  /// @brief The header the ticket's correlation id arrives in.
  static constexpr std::string_view correlation_header = "X-Psn-Correlation-Id";

  /// @brief Largest page accepted, in bytes.
  static constexpr std::size_t page_limit = 1024U * 1024U;
  /// @brief Largest bundle accepted, in bytes.
  static constexpr std::size_t bundle_limit = 4U * 1024U * 1024U;
  /// @brief Largest token, ticket or poll reply accepted, in bytes.
  static constexpr std::size_t reply_limit = 64U * 1024U;
  /// @brief Longest correlation id accepted, in bytes.
  static constexpr std::size_t correlation_limit = 256;
  /// @brief Longest client id accepted after decoding, in bytes.
  static constexpr std::size_t client_id_limit = 256;
  /// @brief Longest interval PSN may set between polls, in seconds.
  static constexpr std::uint64_t interval_limit = 600;
  /// @brief Longest lifetime PSN may give a ticket, in seconds.
  static constexpr std::uint64_t lifetime_limit = 86'400;
  /// @brief Longest wait a `Retry-After` may ask for, in seconds.
  static constexpr std::uint32_t retry_after_limit = 3'600;
  /// @brief The error code of a ticket nobody has approved yet.
  static constexpr std::int64_t code_pending = 5104;
  /// @brief The error code of a ticket refused on the approving device.
  static constexpr std::int64_t code_declined = 5106;
  /// @brief The error code that asks for additional verification.
  static constexpr std::int64_t code_verification = 4097;
  /// @brief The error codes of a ticket that expired or is no longer valid.
  static constexpr std::array<std::int64_t, 5> lapsed_codes{4421, 4607, 4608,
                                                            4621, 5102};

  /// @brief The GET of the hosted sign-in page.
  static constexpr net::http::Endpoint page_endpoint{
      .label = "psn.auth.qr_sign_in.page",
      .maximum_response_bytes = page_limit,
  };
  /// @brief The GET of the page's application bundle.
  static constexpr net::http::Endpoint bundle_endpoint{
      .label = "psn.auth.qr_sign_in.bundle",
      .maximum_response_bytes = bundle_limit,
  };
  /// @brief The application token request.
  static constexpr net::http::Endpoint token_endpoint{
      .label = "psn.auth.qr_sign_in.application_token",
      .method = net::http::Method::post,
      .maximum_response_bytes = reply_limit,
  };
  /// @brief The ticket request.
  static constexpr net::http::Endpoint ticket_endpoint{
      .label = "psn.auth.qr_sign_in.ticket",
      .method = net::http::Method::post,
      .maximum_response_bytes = reply_limit,
  };
  /// @brief The status a poll gets while nobody has approved the ticket yet,
  /// which is an answer rather than an error.
  static constexpr std::array<std::uint16_t, 1> poll_answers{
      net::HttpStatus::bad_request.code()};
  /// @brief A poll of the ticket.
  static constexpr net::http::Endpoint poll_endpoint{
      .label = "psn.auth.qr_sign_in.poll",
      .method = net::http::Method::post,
      .expected_statuses = poll_answers,
      .maximum_response_bytes = reply_limit,
  };

  /// @brief Sends a request, failing at once when the stop token has fired.
  ///
  /// @param endpoint The endpoint the request is for, which labels a failure.
  /// @param request The request; consumed.
  /// @return The reply, whatever its status.
  /// @retval SignInErrorKind::cancelled The stop token fired.
  /// @retval SignInErrorKind::transport The request did not complete.
  [[nodiscard]] auto sent(const net::http::Endpoint& endpoint,
                          net::http::Request request)
      -> Expected<net::http::Response, SignInError>;
  /// @brief Labels a failure with its endpoint and logs it.
  ///
  /// @param endpoint The endpoint that failed.
  /// @param error The failure.
  /// @param detail Appended to the log line; must hold no secret.
  /// @return The labelled failure.
  auto report(const net::http::Endpoint& endpoint, SignInError error,
              std::string_view detail = {}) const -> Unexpected<SignInError>;
  /// @brief Classifies a reply status the step does not expect.
  ///
  /// @param status The status.
  /// @return rate_limited for 429, unavailable otherwise.
  static auto status_failure(std::uint16_t status) noexcept -> SignInError;
  /// @brief Reads a JSON reply body within the endpoint's size limit.
  ///
  /// @tparam Reply The reply type.
  /// @param response The reply.
  /// @return The reply, or nothing when the body is oversized or not JSON of
  ///     that shape.
  template <class Reply>
  [[nodiscard]] static auto reply_of(const net::http::Response& response)
      -> std::optional<Reply>;
  /// @brief Finds the application bundle the hosted page loads.
  ///
  /// @param page The page's HTML.
  /// @return The bundle's absolute URL on the account site.
  /// @retval SignInErrorKind::layout_changed No script, or more than one,
  ///     loads a bundle, or it lies outside the account site.
  [[nodiscard]] static auto bundle_url(std::string_view page)
      -> Expected<std::string, SignInError>;
  /// @brief Makes a script source absolute against the hosted page.
  ///
  /// @param source The `src` attribute as written.
  /// @return The absolute URL, or nothing when it is not a plain path on the
  ///     account site.
  [[nodiscard]] static auto resolved(std::string_view source)
      -> std::optional<std::string>;
  /// @brief Reads the registration out of an application bundle.
  ///
  /// @param bundle The bundle's text.
  /// @return The registration.
  /// @retval SignInErrorKind::layout_changed The bundle does not hold exactly
  ///     one flat `passwordFreeSignin` object with the expected members.
  [[nodiscard]] static auto registration_of(std::string_view bundle)
      -> Expected<HostedRegistration, SignInError>;
  /// @brief Finds the one quoted string value of a member of an object
  /// literal.
  ///
  /// @param object The object's text between its braces.
  /// @param key The member's name.
  /// @return The value without quotes, or nothing when the member is
  ///     missing, repeated, or not a plain string.
  [[nodiscard]] static auto member(std::string_view object,
                                   std::string_view key)
      -> std::optional<std::string_view>;
  /// @brief Finds where the one member of an object literal starts.
  ///
  /// @param object The object's text between its braces.
  /// @param key The member's name.
  /// @return The offset just past the member's `key:`, or nothing when it is
  ///     missing or repeated.
  [[nodiscard]] static auto member_start(std::string_view object,
                                         std::string_view key)
      -> std::optional<std::size_t>;
  /// @brief Undoes the registration's encoding: Base64, reversed, Base64
  /// again.
  ///
  /// @param encoded The value as the bundle holds it.
  /// @return The decoded value, or nothing when either layer is not Base64.
  [[nodiscard]] static auto unfolded(std::string_view encoded)
      -> std::optional<SecretString>;
  /// @brief Reads the scopes array of the registration.
  ///
  /// @param object The registration object's text between its braces.
  /// @return The scopes separated by spaces, or nothing when the array is
  ///     missing, not a JSON string array, or lacks required_scope.
  [[nodiscard]] static auto scope_of(std::string_view object)
      -> std::optional<std::string>;
  /// @brief Reads a ticket reply.
  ///
  /// @param response The 201 reply.
  /// @return The ticket.
  /// @retval SignInErrorKind::malformed A member or the correlation header is
  ///     missing or unusable.
  [[nodiscard]] static auto ticket_of(const net::http::Response& response)
      -> Expected<Ticket, SignInError>;
  /// @brief Reads a poll reply that is not a 200 or a 429.
  ///
  /// @param response The reply.
  /// @return What the error code means.
  /// @retval SignInErrorKind::malformed The reply carries no error code.
  /// @retval SignInErrorKind::unavailable The status or code is unexpected.
  [[nodiscard]] auto refusal_of(const net::http::Response& response) const
      -> Expected<PollAnswer, SignInError>;
  /// @brief Reads the wait of a 429 poll reply.
  ///
  /// @param response The 429 reply.
  /// @return The wait PSN asked for.
  /// @retval SignInErrorKind::rate_limited No usable `Retry-After` header.
  [[nodiscard]] auto slowed(const net::http::Response& response) const
      -> Expected<PollAnswer, SignInError>;
  /// @brief Tells whether a lifetime in seconds is within a bound.
  ///
  /// @param seconds The lifetime.
  /// @param limit The longest accepted.
  /// @return The lifetime, or nothing when it is 0 or above the limit.
  static auto bounded(std::uint64_t seconds, std::uint64_t limit) noexcept
      -> std::optional<std::chrono::seconds>;

  /// @brief Carries every request.
  net::http::Client client_;
  /// @brief Logs failures under the `[AUTH]` prefix.
  Authorization authorization_;
};

}  // namespace libchiaking::psn::auth
