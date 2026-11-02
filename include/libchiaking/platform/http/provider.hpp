/// @file
/// @brief Declares the HTTP port: request, response, TLS and proxy settings,
/// HttpError and the HttpProvider interface.
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/secret_string.hpp>
#include <libchiaking/platform/http/values.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

namespace libchiaking::platform {

/// @brief Where a request came from inside the library, for diagnostics. None
/// of it is sent to the server.
///
/// The library fills it in when it builds a request. A provider may ignore it
/// entirely. carried is secret and wiped with the request.
struct HttpProvenance {
  /// @brief The kinds of credential the library attached.
  HttpCredentials credentials;
  /// @brief Short diagnostic name of the credential that authorises the
  /// request, such as "remote play bearer", or empty. A view of text that
  /// outlives the request.
  std::string_view credential_label;
  /// @brief Which try this is: 1 for the first, counting up on each retry of
  /// the same request.
  std::uint16_t attempt = 1;
  /// @brief Secrets the library placed in the URL or body rather than in a
  /// header, so that diagnostics can recognise them there.
  std::vector<SecretString> carried;
  /// @brief Statuses outside 2xx that are ordinary answers for this request,
  /// such as the 400 a poll gets while its ticket waits, so that diagnostics
  /// do not count them as errors. A view of codes that outlive the request.
  std::span<const std::uint16_t> expected_statuses;

  /// @brief Makes an independent copy, duplicating every carried secret.
  ///
  /// @return Provenance equal to this one that owns its own secrets.
  auto clone() const -> HttpProvenance {
    std::vector<SecretString> copies;
    copies.reserve(carried.size());
    std::ranges::transform(carried, std::back_inserter(copies),
                           &SecretString::clone);
    return {credentials, credential_label, attempt, std::move(copies),
            expected_statuses};
  }
};

/// @brief What a provider does with a 3xx redirect response.
enum class HttpRedirectPolicy : std::uint8_t {
  /// @brief Do not follow. The redirect response itself is returned, with its
  /// 3xx status and headers.
  deny,
  /// @brief Follow redirects, provided the first URL and every hop are HTTPS.
  /// The stock provider rejects a request whose own URL is not HTTPS with
  /// HttpError::invalid_url.
  follow_https,
};

/// @brief How a request chooses a proxy.
enum class HttpProxyPolicy : std::uint8_t {
  /// @brief Use the backend's default proxy selection. The stock curl
  /// providers leave libcurl's default in place, which honours the proxy
  /// environment variables.
  system,
  /// @brief Connect directly and ignore any configured proxy.
  disabled,
  /// @brief Use the proxy named by TlsConfiguration::explicit_proxy_url.
  explicit_proxy,
};

/// @brief Which certificate authorities a TLS connection trusts.
enum class TlsTrustMode : std::uint8_t {
  /// @brief Trust the platform's root certificates.
  system,
  /// @brief Trust only the certificates in TlsConfiguration::custom_ca_pem.
  custom_ca_pem,
  /// @brief Trust the platform's roots and the certificates in
  /// TlsConfiguration::custom_ca_pem.
  system_plus_custom_ca_pem,
};

/// @brief Trust and proxy settings for one HTTP request or WebSocket
/// connection.
///
/// Whatever the settings, a provider must verify the server certificate chain
/// and that the certificate matches the host name.
struct TlsConfiguration {
  /// @brief Which certificate authorities to trust.
  TlsTrustMode trust_mode = TlsTrustMode::system;
  /// @brief PEM-encoded CA certificates for the custom trust modes. It must
  /// not be empty when trust_mode is not TlsTrustMode::system; the stock
  /// providers fail such a request with a TLS trust error.
  std::vector<std::byte> custom_ca_pem;
  /// @brief How to choose a proxy.
  HttpProxyPolicy proxy_policy = HttpProxyPolicy::system;
  /// @brief Proxy URL for HttpProxyPolicy::explicit_proxy. It must not be
  /// empty under that policy; the stock providers fail such a request with a
  /// proxy error.
  std::string explicit_proxy_url;
};

/// @brief Punctuation characters allowed in a header name besides letters and
/// digits.
inline constexpr std::string_view http_token_punctuation = "!#$%&'*+-.^_`|~";

/// @brief Default limit on the whole of one HTTP request.
inline constexpr std::chrono::milliseconds default_http_total_timeout =
    std::chrono::seconds{30};
/// @brief Default cap on a buffered response body: 4 MiB.
inline constexpr std::size_t default_max_response_bytes =
    std::size_t{4} * 1024 * 1024;

/// @brief One complete HTTP request, with everything needed to perform it.
///
/// The request owns its URL, headers, body and TLS material. URL, header values
/// and body are wiping secret types because they carry account tokens and
/// sign-in codes; they are erased when the request is destroyed. The request is
/// therefore movable but not copyable; use clone().
struct HttpRequest {
  /// @brief Request method.
  HttpMethod method = HttpMethod::get;
  /// @brief Absolute URL. The stock provider accepts the http and https
  /// schemes only and fails anything else with HttpError::invalid_url.
  SecretString url;
  /// @brief Request headers, sent in this order with duplicates kept.
  std::vector<HttpHeader> headers;
  /// @brief Request body for HttpMethod::post and HttpMethod::delete_;
  /// ignored for the other methods.
  SecretBytes body;
  /// @brief Limit on establishing the connection. Zero leaves the backend's
  /// own default in place.
  std::chrono::milliseconds connect_timeout{0};
  /// @brief Limit on the whole request, from start to the last body byte.
  /// Zero means no limit.
  std::chrono::milliseconds total_timeout{default_http_total_timeout};
  /// @brief Whether redirects are followed.
  HttpRedirectPolicy redirect_policy = HttpRedirectPolicy::deny;
  /// @brief Largest response body to accept, in bytes. A larger body fails
  /// the request with HttpError::response_too_large. Zero means no cap.
  std::size_t maximum_response_bytes = default_max_response_bytes;
  /// @brief Certificate trust and proxy settings for this request.
  TlsConfiguration tls_configuration;
  /// @brief Short name of the endpoint for diagnostics. It is not sent to the
  /// server. It is a view the request does not own, so the text must outlive
  /// the request and its clones.
  std::string_view label;
  /// @brief What the library attached and which try this is, for
  /// diagnostics. It is not sent to the server.
  HttpProvenance provenance;

  /// @brief Makes an independent copy of the request, duplicating every
  /// secret into its own wiping storage. label still refers to the same text.
  ///
  /// @return A request equal to this one that owns its own secrets.
  auto clone() const -> HttpRequest {
    return {method,
            url.clone(),
            platform::clone(headers),
            body.clone(),
            connect_timeout,
            total_timeout,
            redirect_policy,
            maximum_response_bytes,
            tls_configuration,
            label,
            provenance.clone()};
  }
};

/// @brief A complete, buffered HTTP response.
///
/// URL, header values and body are wiping secret types and are erased when the
/// response is destroyed. The response is movable but not copyable; use
/// clone().
struct HttpResponse {
  /// @brief HTTP status code of the final response, for example 200 or 404.
  /// Any status is a successful perform(); it is the caller that decides what
  /// a 4xx or 5xx means.
  std::uint16_t status_code = 0;
  /// @brief Reason phrase from the status line. It can be empty, since newer
  /// HTTP versions send none.
  std::string reason;
  /// @brief Response headers in the order received. When redirects were
  /// followed, only the last response's headers are kept.
  std::vector<HttpHeader> headers;
  /// @brief URL the response came from, which differs from the request URL
  /// when redirects were followed.
  SecretString final_url;
  /// @brief Response body. Empty for HttpMethod::head.
  SecretBytes body;

  /// @brief Makes an independent copy of the response and its secrets.
  ///
  /// @return A response equal to this one that owns its own secrets.
  auto clone() const -> HttpResponse {
    return {status_code, reason, platform::clone(headers), final_url.clone(),
            body.clone()};
  }
};

/// @brief Reason an HTTP request produced no response.
///
/// These are transport failures only. A response with an error status is not a
/// failure: it is returned as an HttpResponse.
enum class HttpError : std::uint8_t {
  /// @brief The stop token was stopped before or during the request.
  cancelled,
  /// @brief The connect or total timeout elapsed.
  timed_out,
  /// @brief The URL is malformed or its scheme is not allowed for this
  /// request.
  invalid_url,
  /// @brief The server certificate could not be verified, the TLS handshake
  /// failed, or the custom CA material is missing or unusable.
  tls_trust_failed,
  /// @brief The proxy could not be resolved or used, or an explicit proxy was
  /// requested without a usable URL.
  proxy_failed,
  /// @brief The host could not be resolved or connected to, or the connection
  /// dropped before the response was complete.
  disconnected,
  /// @brief The body exceeded HttpRequest::maximum_response_bytes. No partial
  /// body is returned.
  response_too_large,
  /// @brief The provider cannot perform this kind of request.
  unsupported,
  /// @brief Any other failure. The stock provider also returns it for a
  /// request it refuses to send: an invalid header name or value, or a
  /// negative timeout.
  backend_failure,
};

/// @brief A request that has been handed to an HttpProvider and whose response
/// is collected later.
///
/// HttpProvider::start returns one. Several can be outstanding at once, which
/// lets a provider that multiplexes requests run them side by side while the
/// caller keeps one thread. Move-only. The response is taken once, with wait();
/// destroying a pending response that was never waited for abandons the
/// request. The provider that started it must outlive it.
class PendingHttpResponse {
 public:
  /// @brief Where a pending response comes from. Implemented by providers.
  class Source {
   public:
    /// @brief Abandons the request when it was not waited for.
    virtual ~Source() = default;
    /// @brief Not copyable; owned by one PendingHttpResponse.
    Source(const Source&) = delete;
    /// @brief Not copy-assignable; owned by one PendingHttpResponse.
    Source& operator=(const Source&) = delete;

    /// @brief Blocks until the response is complete or the request fails.
    ///
    /// Called at most once.
    ///
    /// @return As HttpProvider::perform.
    [[nodiscard]] virtual auto wait() -> Expected<HttpResponse, HttpError> = 0;

   protected:
    /// @brief Constructs the base of an implementation.
    Source() = default;
  };

  /// @brief Takes ownership of a provider's source.
  ///
  /// @param source The request's source; must not be null.
  explicit PendingHttpResponse(std::unique_ptr<Source> source) noexcept
      : source_(std::move(source)) {}

  /// @brief Takes over another pending response.
  PendingHttpResponse(PendingHttpResponse&&) noexcept = default;
  /// @brief Takes over another pending response, abandoning this one.
  PendingHttpResponse& operator=(PendingHttpResponse&&) noexcept = default;
  /// @brief Not copyable: one response has one owner.
  PendingHttpResponse(const PendingHttpResponse&) = delete;
  /// @brief Not copy-assignable: one response has one owner.
  PendingHttpResponse& operator=(const PendingHttpResponse&) = delete;
  /// @brief Abandons the request when it was not waited for.
  ~PendingHttpResponse() = default;

  /// @brief Blocks until the response is complete or the request fails, and
  /// hands it over.
  ///
  /// @return As HttpProvider::perform.
  /// @retval HttpError::backend_failure The response was already taken.
  [[nodiscard]] auto wait() && -> Expected<HttpResponse, HttpError> {
    if (!source_) return unexpected(HttpError::backend_failure);
    auto source = std::move(source_);
    return source->wait();
  }

 private:
  std::unique_ptr<Source> source_;
};

/// @brief Port that performs one HTTP request and returns the whole response.
///
/// Implemented by the host frontend, or by CurlHttpProvider in this tree. The
/// host passes it in PlatformProviders::http; the library obtains it from
/// PlatformFactory::http and uses it for PlayStation Network sign-in, account
/// and catalogue requests, and for fetching STUN server lists.
///
/// perform() is synchronous: the calling library thread is blocked until the
/// response is complete or the request has failed. The library cancels through
/// the stop token and expects a prompt return.
///
/// An implementation must:
///
/// - keep the HTTP status separate from transport success, returning a response
///   for every status;
/// - verify the server certificate chain and host name according to
///   HttpRequest::tls_configuration;
/// - enforce HttpRequest::connect_timeout, HttpRequest::total_timeout and
///   HttpRequest::maximum_response_bytes;
/// - keep every hop HTTPS when it follows redirects;
/// - keep nothing from the request after perform() returns. Credentials in
///   headers and bodies belong to that request alone and must not be persisted;
/// - be safe for concurrent calls: the library calls perform() from several
///   threads at once (the session thread, PSN signalling, data route and
///   catalogue workers) on one provider, and each call must behave as if it
///   were the only one, with no request's headers, body, response or
///   cancellation seen by another. CurlHttpProvider meets this.
///
/// On HttpError::cancelled the library treats the operation as cancelled. Every
/// other error is reported to the caller of the library as a transport failure
/// of that request; only endpoints configured for it are retried.
/// @ingroup api_platform
class HttpProvider {
 public:
  /// @brief Not copyable; providers are shared by reference.
  HttpProvider(const HttpProvider&) = delete;
  /// @brief Not copy-assignable; providers are shared by reference.
  HttpProvider& operator=(const HttpProvider&) = delete;

 protected:
  /// @brief Constructs the base; only implementations create providers.
  HttpProvider() = default;

 public:
  /// @brief Allows destruction through the interface.
  virtual ~HttpProvider() = default;

  /// @brief Performs one request and blocks until the response is complete or
  /// the request fails.
  ///
  /// @param request The request to send. Borrowed for the call; the provider
  ///     must not keep references into it afterwards.
  /// @param stop_token Cancels the request when stopped. The stock provider
  ///     sends nothing when the token is already stopped.
  /// @return The buffered response, owned by the caller, whatever its status
  ///     code.
  /// @retval HttpError::cancelled The stop token was stopped.
  /// @retval HttpError::timed_out A timeout elapsed.
  /// @retval HttpError::invalid_url The URL was rejected.
  /// @retval HttpError::tls_trust_failed The server could not be trusted.
  /// @retval HttpError::proxy_failed The proxy could not be used.
  /// @retval HttpError::disconnected The connection could not be made or was
  ///     lost.
  /// @retval HttpError::response_too_large The body exceeded the cap.
  /// @retval HttpError::backend_failure Any other failure.
  [[nodiscard]] virtual auto perform(const HttpRequest& request,
                                     const StopToken& stop_token)
      -> Expected<HttpResponse, HttpError> = 0;

  /// @brief Hands a request over without waiting for its response.
  ///
  /// A provider that can run requests side by side starts this one now and
  /// returns while it runs. The default sends nothing until the response is
  /// waited for, and then calls perform() on the waiting thread, so a
  /// provider that only implements perform() still works, one request at a
  /// time.
  ///
  /// @param request The request to send. Copied, so it is read during the call
  ///     only.
  /// @param stop_token Cancels the request when stopped; kept until the
  ///     response is taken.
  /// @return The pending response. This provider must outlive it.
  [[nodiscard]] virtual auto start(const HttpRequest& request,
                                   const StopToken& stop_token)
      -> PendingHttpResponse;

 private:
  class Deferred;
};

}  // namespace libchiaking::platform
