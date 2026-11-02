/// @file
/// @brief Declares HttpCallLog, the always-on in-memory record of the
/// library's recent HTTP exchanges, and the values it hands out.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/platform/byte_units.hpp"
#include "libchiaking/platform/http/provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::diagnostics {

/// @brief The part of the library that made an HTTP call.
///
/// It is taken from which context the call went through, not from the URL.
enum class HttpArea : std::uint8_t {
  /// @brief Signing in, accounts and the credentials of signed-in players.
  users,
  /// @brief Finding, pairing, waking and starting consoles.
  consoles,
  /// @brief Games, ownership, store lookups and images.
  catalog,
  /// @brief Trophy levels, trophy lists and each title's trophies.
  trophies,
  /// @brief Message conversations, reading them and sending text.
  messages,
  /// @brief Starting and running sessions, including cloud streaming.
  play,
  /// @brief A call that came through none of the contexts above.
  other,
};

/// @brief What a secret span in recorded text holds.
enum class HttpSecretKind : std::uint8_t {
  /// @brief An authorisation header value after its scheme, or a token
  /// found elsewhere.
  credential,
  /// @brief The value of a cookie.
  cookie,
  /// @brief A cloud streaming session value.
  session,
  /// @brief The value of a sensitive query or form parameter.
  parameter,
  /// @brief The value of a sensitive JSON field.
  field,
  /// @brief A whole body whose secrets could not be located because it does
  /// not parse.
  body,
};

/// @brief A range of recorded text that holds a secret.
struct HttpSecretSpan {
  /// @brief Offset of the first byte of the secret.
  std::size_t offset{};
  /// @brief Number of bytes the secret covers.
  std::size_t length{};
  /// @brief What the secret is.
  HttpSecretKind kind{};

  /// @brief Compares two spans member by member.
  friend auto operator==(const HttpSecretSpan&, const HttpSecretSpan&) noexcept
      -> bool = default;
};

/// @brief Recorded text, kept in full, with every secret in it marked.
///
/// The text is wiped when the value is destroyed. It is move-only; use
/// clone() for a deliberate copy.
class HttpMarkedText {
 public:
  /// @brief Empty text with no secrets.
  HttpMarkedText() = default;
  /// @brief Adopts text and the spans that mark its secrets.
  ///
  /// @param text The text.
  /// @param spans Secret ranges in any order. Empty ranges are dropped,
  ///     ranges are clipped to the text, and overlapping or touching ranges
  ///     are merged, keeping the kind of the first.
  HttpMarkedText(SecretString text, std::vector<HttpSecretSpan> spans);

  /// @brief Makes an independent copy, duplicating the text into its own
  /// wiping storage.
  ///
  /// @return A value equal to this one.
  auto clone() const -> HttpMarkedText;
  /// @brief The full text, secrets included.
  ///
  /// @return A view valid while this value lives and is not moved from.
  auto text() const noexcept -> std::string_view { return text_.view(); }
  /// @brief The secret ranges, sorted by offset and not overlapping.
  ///
  /// @return A view valid while this value lives and is not moved from.
  auto spans() const noexcept -> std::span<const HttpSecretSpan> {
    return spans_;
  }

 private:
  SecretString text_;
  std::vector<HttpSecretSpan> spans_;
};

/// @brief One recorded header, with the secrets in its value marked.
struct HttpMarkedHeader {
  /// @brief Header name as sent or received.
  std::string name;
  /// @brief Header value.
  HttpMarkedText value;
};

/// @brief A recorded body as it is shown.
struct HttpShownBody {
  /// @brief Size of the whole body as sent or received, in bytes.
  std::size_t size{};
  /// @brief Number of those bytes the log kept; at most
  /// HttpCallLog::body_limit.
  std::size_t kept{};
  /// @brief True when the log kept only the first part of the body.
  bool truncated{};
  /// @brief True when the kept bytes are not UTF-8 text; text is then empty.
  bool binary{};
  /// @brief The kept body as text. A JSON document is pretty-printed with
  /// two-space indentation.
  HttpMarkedText text;
};

/// @brief One recorded HTTP exchange: what was asked, what came back, and how
/// the library made the call.
///
/// Recorded exchanges are made only by HttpCallLog and never change. They hold
/// the full request and response, secrets included, in wiping memory.
/// Accessors that return marked text work the marking out when called, on the
/// calling thread.
class HttpCall {
 public:
  /// @brief The wall clock the start time is read from.
  using Clock = std::chrono::system_clock;

  /// @brief Not copyable; exchanges are shared by pointer.
  HttpCall(const HttpCall&) = delete;
  /// @brief Not copy-assignable; exchanges are shared by pointer.
  auto operator=(const HttpCall&) -> HttpCall& = delete;
  /// @brief Wipes the recorded secrets.
  ~HttpCall();

  /// @brief Number of the exchange since the log was created, counting from
  /// 1. It is never reused, even after HttpCallLog::clear.
  ///
  /// @return The number.
  auto sequence() const noexcept -> std::uint64_t { return sequence_; }
  /// @brief The part of the library that made the call.
  ///
  /// @return The area.
  auto area() const noexcept -> HttpArea { return area_; }
  /// @brief The kinds of credential the library attached.
  ///
  /// @return The set; empty for an anonymous call.
  auto credentials() const noexcept -> platform::HttpCredentials {
    return credentials_;
  }
  /// @brief The diagnostic name of the credential that authorised the call,
  /// such as "remote play bearer".
  ///
  /// @return The name, or empty.
  auto credential_label() const noexcept -> std::string_view {
    return credential_label_;
  }
  /// @brief The diagnostic name of the endpoint, such as
  /// "psn.gamelist.played_titles".
  ///
  /// @return The name, or empty.
  auto label() const noexcept -> std::string_view { return label_; }
  /// @brief Which try of the request this was: 1 for the first.
  ///
  /// @return The attempt number.
  auto attempt() const noexcept -> std::uint16_t { return attempt_; }
  /// @brief The request method.
  ///
  /// @return The method.
  auto method() const noexcept -> platform::HttpMethod { return method_; }
  /// @brief When the request was handed to the HTTP provider.
  ///
  /// @return The wall-clock time.
  auto started() const noexcept -> Clock::time_point { return started_; }
  /// @brief How long the provider took to answer or fail.
  ///
  /// @return The duration.
  auto duration() const noexcept -> std::chrono::microseconds {
    return duration_;
  }
  /// @brief The scheme and host the request went to, such as
  /// "https://m.np.playstation.com", with the port when the URL names one.
  ///
  /// @return A view valid while this exchange lives. Hosts hold no secret.
  auto base() const noexcept -> std::string_view;
  /// @brief The HTTP status of the response.
  ///
  /// @return The status, or std::nullopt when the transport failed.
  auto status() const noexcept -> std::optional<std::uint16_t>;
  /// @brief The reason phrase of the response.
  ///
  /// @return The phrase, often empty, and empty when the transport failed.
  auto reason() const noexcept -> std::string_view;
  /// @brief Why the request produced no response.
  ///
  /// @return The transport error, or std::nullopt when a response came back.
  auto transport_error() const noexcept -> std::optional<platform::HttpError>;
  /// @brief Whether the exchange failed: a transport error, or a status of
  /// 400 or more that the endpoint did not declare an ordinary answer.
  ///
  /// @return True when it failed.
  auto failed() const noexcept -> bool;
  /// @brief Size of the request body, in bytes.
  ///
  /// @return The full size, whether or not it was all kept.
  auto bytes_sent() const noexcept -> std::size_t { return request_size_; }
  /// @brief Size of the response body, in bytes.
  ///
  /// @return The full size, or 0 when the transport failed.
  auto bytes_received() const noexcept -> std::size_t { return response_size_; }

  /// @brief The full request URL with its secrets marked.
  ///
  /// @return Sensitive query and fragment parameters, and any credential the
  ///     request carried, marked.
  auto url() const -> HttpMarkedText;
  /// @brief The request headers in the order sent, with their secrets marked.
  ///
  /// @return Authorisation values after the scheme, cookie values,
  ///     X-Gaikai values and sensitive parameters in other values, marked.
  auto request_headers() const -> std::vector<HttpMarkedHeader>;
  /// @brief The request body as shown.
  ///
  /// @return The body, with sensitive form parameters and JSON string fields
  ///     marked, and any credential the request carried marked wherever it
  ///     appears.
  auto request_body() const -> HttpShownBody;
  /// @brief The response headers in the order received, with their secrets
  /// marked.
  ///
  /// @return The headers; empty when the transport failed.
  auto response_headers() const -> std::vector<HttpMarkedHeader>;
  /// @brief The response body as shown.
  ///
  /// @return The body, marked as request_body() marks; empty when the
  ///     transport failed.
  auto response_body() const -> HttpShownBody;

 private:
  friend class HttpCallLog;

  struct Recorded;
  struct Known {
    std::string_view value;
    HttpSecretKind kind;
  };

  static constexpr std::size_t shortest_known = 8;

  explicit HttpCall(const Recorded& recorded);

  static auto recorded(
      std::uint64_t sequence, HttpArea area,
      const platform::HttpRequest& request,
      const Expected<platform::HttpResponse, platform::HttpError>& outcome,
      Clock::time_point started, std::chrono::microseconds duration)
      -> std::shared_ptr<const HttpCall>;

  static auto base_of(std::string_view url) noexcept -> std::string_view;
  static auto kept(const SecretBytes& body) -> SecretBytes;
  static auto header_spans(std::string_view name, std::string_view value)
      -> std::vector<HttpSecretSpan>;
  static void add_parameter_spans(std::vector<HttpSecretSpan>& spans,
                                  std::string_view text, std::size_t from);
  static void add_matches(std::vector<HttpSecretSpan>& spans,
                          std::string_view text, std::span<const Known> known);
  static void add_known(std::vector<Known>& known, std::string_view text,
                        std::span<const HttpSecretSpan> spans);
  static auto form_like(std::string_view text) noexcept -> bool;

  auto known() const -> std::vector<Known>;
  auto marked_headers(const std::vector<platform::HttpHeader>& headers) const
      -> std::vector<HttpMarkedHeader>;
  auto shown(const SecretBytes& body, std::size_t size) const -> HttpShownBody;
  auto stored_bytes() const noexcept -> std::size_t;

  std::uint64_t sequence_;
  HttpArea area_;
  platform::HttpCredentials credentials_;
  std::string credential_label_;
  std::string label_;
  std::uint16_t attempt_;
  platform::HttpMethod method_;
  Clock::time_point started_;
  std::chrono::microseconds duration_;
  SecretString url_;
  std::vector<platform::HttpHeader> request_headers_;
  SecretBytes request_body_;
  std::size_t request_size_;
  std::vector<SecretString> carried_;
  std::optional<std::uint16_t> status_;
  bool expected_{};
  std::string reason_;
  std::vector<platform::HttpHeader> response_headers_;
  SecretBytes response_body_;
  std::size_t response_size_;
  std::optional<platform::HttpError> transport_error_;
};

/// @brief Counts of HTTP exchanges in one group.
struct HttpTally {
  /// @brief Exchanges made.
  std::uint64_t requests{};
  /// @brief Exchanges answered with a status of 400 or more that the endpoint
  /// did not declare an ordinary answer.
  std::uint64_t http_errors{};
  /// @brief Exchanges that produced no response.
  std::uint64_t transport_errors{};
  /// @brief Request body bytes sent.
  std::uint64_t bytes_sent{};
  /// @brief Response body bytes received.
  std::uint64_t bytes_received{};

  /// @brief Compares two tallies member by member.
  friend auto operator==(const HttpTally&, const HttpTally&) noexcept
      -> bool = default;
};

/// @brief The tally of exchanges sent to one scheme and host.
struct HttpBaseTally {
  /// @brief The scheme and host, as HttpCall::base gives it.
  std::string base;
  /// @brief The counts for that base.
  HttpTally tally;
};

/// @brief The tally of exchanges that carried one kind of credential.
struct HttpCredentialTally {
  /// @brief The kind.
  platform::HttpCredential credential{};
  /// @brief The counts for exchanges that carried it. An exchange that
  /// carried several kinds counts under each.
  HttpTally tally;
};

/// @brief Lifetime counts, kept apart from the bounded list of exchanges.
struct HttpCallTotals {
  /// @brief Every exchange.
  HttpTally all;
  /// @brief Exchanges that carried no credential.
  HttpTally anonymous;
  /// @brief One entry for each kind in platform::every_http_credential, in
  /// that order.
  std::vector<HttpCredentialTally> credentials;
  /// @brief One entry for each base seen, sorted by base.
  std::vector<HttpBaseTally> bases;
};

/// @brief What HttpCallLog::snapshot returns: a consistent copy of the log.
struct HttpCallSnapshot {
  /// @brief The revision the copy was taken at; see HttpCallLog::revision.
  std::uint64_t revision{};
  /// @brief The kept exchanges, newest first.
  std::vector<std::shared_ptr<const HttpCall>> calls;
  /// @brief The lifetime counts.
  HttpCallTotals totals;
};

/// @brief Always-on, bounded, in-memory record of the HTTP exchanges the
/// library makes, with lifetime counts.
///
/// ChiakiNg owns one and records every call its contexts make, tagged with the
/// context and with the credentials the library attached. Nothing is written
/// to disk. The log keeps the newest capacity exchanges and at most byte_limit
/// bytes of them, dropping the oldest first, and keeps at most body_limit bytes
/// of each body. The counts in HttpCallTotals keep counting past those bounds
/// until reset_totals().
///
/// Every member is safe to call from any thread. record() copies the exchange
/// before taking a short lock and does no parsing; the marking of secrets is
/// done by the HttpCall accessors on the thread that reads them.
///
/// @warning The log holds live credentials in memory. A frontend shows them
///     masked, using the marked spans, unless the user asks to see one.
class HttpCallLog {
 public:
  /// @brief The most exchanges kept.
  static constexpr std::size_t capacity = 200;
  /// @brief The most bytes kept of one body. The rest is dropped and the body
  /// marked truncated.
  static constexpr std::size_t body_limit = bytes_per_mib;
  /// @brief The most bytes all kept exchanges may hold together.
  static constexpr std::size_t byte_limit = 32 * bytes_per_mib;

  /// @brief An empty log with zero counts.
  ///
  /// @param threading Makes the lock that guards the kept exchanges; must
  ///     outlive the log.
  explicit HttpCallLog(platform::ThreadingProvider& threading);
  /// @brief Not copyable; recorders hold it by reference.
  HttpCallLog(const HttpCallLog&) = delete;
  /// @brief Not copy-assignable; recorders hold it by reference.
  auto operator=(const HttpCallLog&) -> HttpCallLog& = delete;
  /// @brief Wipes every kept exchange.
  ~HttpCallLog();

  /// @brief Records one finished exchange.
  ///
  /// Called by the library's HTTP recorder on the thread that made the call,
  /// after the provider returned. Copies what it keeps and never blocks for
  /// longer than it takes to append to the list. When memory runs out the
  /// exchange is counted but not kept.
  ///
  /// @param area The part of the library that made the call.
  /// @param request The request as sent. Copied; not retained.
  /// @param outcome The response or the transport error. Copied; not
  ///     retained.
  /// @param started When the request was handed to the provider.
  /// @param duration How long the provider took.
  void record(
      HttpArea area, const platform::HttpRequest& request,
      const Expected<platform::HttpResponse, platform::HttpError>& outcome,
      HttpCall::Clock::time_point started,
      std::chrono::microseconds duration) noexcept;

  /// @brief Copies the log.
  ///
  /// @return The kept exchanges newest first, the lifetime counts and the
  ///     revision they belong to. The exchanges are shared, not copied.
  auto snapshot() const -> HttpCallSnapshot;
  /// @brief A number that changes whenever the log or the counts change.
  ///
  /// A viewer compares it with HttpCallSnapshot::revision to decide whether
  /// to take a new snapshot. Never blocks.
  ///
  /// @return The current revision.
  auto revision() const noexcept -> std::uint64_t {
    return revision_.load(std::memory_order_acquire);
  }
  /// @brief Drops every kept exchange. The lifetime counts are left alone.
  void clear() noexcept;
  /// @brief Sets the lifetime counts back to zero. The kept exchanges are
  /// left alone.
  void reset_totals() noexcept;

 private:
  static constexpr std::size_t evictions_reserved = 4;

  struct Counts {
    HttpTally all;
    HttpTally anonymous;
    std::array<HttpTally, platform::every_http_credential.size()> credentials{};
    std::map<std::string, HttpTally, std::less<>> bases;
  };

  static void count(
      HttpTally& tally, std::size_t sent,
      const Expected<platform::HttpResponse, platform::HttpError>& outcome,
      bool expected) noexcept;
  void count(
      const platform::HttpRequest& request,
      const Expected<platform::HttpResponse, platform::HttpError>& outcome);

  mutable platform::Mutex mutex_;
  std::deque<std::shared_ptr<const HttpCall>> calls_;
  std::size_t bytes_{};
  std::atomic<std::uint64_t> sequence_{};
  std::atomic<std::uint64_t> revision_{};
  Counts counts_;
};

}  // namespace libchiaking::diagnostics
