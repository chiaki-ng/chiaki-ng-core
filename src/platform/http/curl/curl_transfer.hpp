/// @file
/// @brief Declares CurlTransfer, one HTTP request set up on a libcurl easy
/// handle, shared by the pooled and the multi-handle HTTP providers.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/http/provider.hpp>

#include "src/platform/curl/header_list.hpp"

#include <curl/curl.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace libchiaking::platform::curl_detail {

/// @brief One HTTP request configured on a libcurl easy handle, from the
/// checks before it starts to the response after it ends.
///
/// Both stock HTTP providers use it, so a request is validated, configured
/// and read back the same way whether it runs with `curl_easy_perform` or
/// inside a multi handle. The provider owns the easy handle and runs the
/// transfer; this class owns everything the handle points at while it runs:
/// the header list and the response being collected.
///
/// The request and the stop token are borrowed and must outlive the
/// transfer. The object never moves, because libcurl's callbacks hold its
/// address.
class CurlTransfer {
 public:
  /// @brief Checks a request and configures an easy handle to send it.
  ///
  /// @param curl The easy handle, freshly created or reset. It is
  ///     configured but not started.
  /// @param share The share handle to attach, or null for none.
  /// @param request The request. Borrowed until the transfer is finished.
  /// @param stop_token Aborts the transfer through libcurl's progress
  ///     callback. Borrowed until the transfer is finished.
  /// @return The configured transfer.
  /// @retval HttpError::cancelled The stop token was already stopped.
  /// @retval HttpError::invalid_url The URL is empty or malformed, is
  ///     neither http nor https, or is not https while redirects are to be
  ///     followed.
  /// @retval HttpError::tls_trust_failed A custom trust mode was given no
  ///     authorities, or libcurl refused the trust settings.
  /// @retval HttpError::proxy_failed The proxy settings are unusable.
  /// @retval HttpError::backend_failure libcurl could not be initialised
  ///     or refused an option, a header was invalid, or a timeout was
  ///     negative.
  [[nodiscard]] static auto prepare(CURL* curl, CURLSH* share,
                                    const HttpRequest& request,
                                    const StopToken& stop_token)
      -> Expected<std::unique_ptr<CurlTransfer>, HttpError>;

  /// @brief Checks a request before any handle is touched.
  ///
  /// @param request The request.
  /// @param stop_token The caller's stop token.
  /// @return Nothing when the request can be sent.
  /// @retval HttpError::cancelled The stop token was already stopped.
  /// @retval HttpError::invalid_url The URL is not acceptable.
  /// @retval HttpError::tls_trust_failed The trust settings are unusable.
  /// @retval HttpError::proxy_failed The proxy settings are unusable.
  /// @retval HttpError::backend_failure libcurl is not initialised, a
  ///     header is invalid, or a timeout is negative.
  [[nodiscard]] static auto check(const HttpRequest& request,
                                  const StopToken& stop_token)
      -> Expected<void, HttpError>;
  /// @brief Not copyable: libcurl holds this object's address.
  CurlTransfer(const CurlTransfer&) = delete;
  /// @brief Not copyable: libcurl holds this object's address.
  CurlTransfer& operator=(const CurlTransfer&) = delete;
  /// @brief Not movable: libcurl holds this object's address.
  CurlTransfer(CurlTransfer&&) = delete;
  /// @brief Not movable: libcurl holds this object's address.
  CurlTransfer& operator=(CurlTransfer&&) = delete;
  /// @brief Releases the header list. The easy handle is not touched.
  ~CurlTransfer() = default;

  /// @brief Reads the response once libcurl has finished the transfer.
  ///
  /// @param result What libcurl reported for the transfer.
  /// @return The final response, whatever its status code.
  /// @retval HttpError::cancelled The stop token stopped the transfer.
  /// @retval HttpError::response_too_large The body exceeded the request's
  ///     limit.
  /// @retval HttpError::timed_out A connect or total timeout elapsed.
  /// @retval HttpError::disconnected The host could not be resolved or
  ///     reached, or the transfer was cut short.
  /// @retval HttpError::backend_failure libcurl reported an error not
  ///     listed above, or the response could not be read back.
  [[nodiscard]] auto finish(CURLcode result)
      -> Expected<HttpResponse, HttpError>;
  /// @brief The easy handle this transfer configured.
  ///
  /// @return The handle given to prepare().
  [[nodiscard]] auto handle() const noexcept -> CURL*;

 private:
  /// @brief Collects the status line, headers and body as libcurl
  /// delivers them, enforcing the response size limit.
  class ResponseCollector {
   public:
    /// @brief Starts an empty response.
    ///
    /// @param maximum_response_bytes The largest body accepted, or 0
    ///     for no limit.
    explicit ResponseCollector(std::size_t maximum_response_bytes) noexcept;

    /// @brief Not copyable: libcurl holds this object's address.
    ResponseCollector(const ResponseCollector&) = delete;
    /// @brief Not copyable: libcurl holds this object's address.
    ResponseCollector& operator=(const ResponseCollector&) = delete;

    /// @brief Appends body bytes.
    ///
    /// @param bytes The bytes libcurl delivered.
    /// @return How many bytes were accepted.
    /// @retval HttpError::response_too_large The limit was exceeded.
    /// @retval HttpError::backend_failure The body could not grow.
    [[nodiscard]] auto append_body(std::span<const std::byte> bytes)
        -> Expected<std::size_t, HttpError>;
    /// @brief Records one header line. A status line starts the
    /// headers again, so only the final response's headers remain.
    ///
    /// @param line The line libcurl delivered.
    /// @return How many bytes were accepted.
    /// @retval HttpError::backend_failure The line could not be stored.
    [[nodiscard]] auto append_header_line(std::string_view line)
        -> Expected<std::size_t, HttpError>;
    /// @brief Records a failure noticed outside the append calls.
    ///
    /// @param error The failure. Only the first one is kept.
    void record_failure(HttpError error) noexcept;
    /// @brief The first failure recorded, if any.
    ///
    /// @return The failure, or nothing.
    [[nodiscard]] auto error() const noexcept -> std::optional<HttpError>;
    /// @brief Hands over the collected response.
    ///
    /// @return The response, or the first failure recorded.
    [[nodiscard]] auto take() -> Expected<HttpResponse, HttpError>;

   private:
    /// @brief Keeps the first failure and reports the given one.
    ///
    /// @param error The failure.
    /// @return The failure, ready to return.
    auto fail(HttpError error) noexcept -> Unexpected<HttpError>;

    /// @brief The largest body accepted, or 0 for no limit.
    std::size_t maximum_response_bytes_;
    /// @brief The response collected so far.
    HttpResponse response_;
    /// @brief The first failure recorded, if any.
    std::optional<HttpError> error_;
  };

  /// @brief Binds a transfer to its handle, request and stop token.
  ///
  /// @param curl The easy handle to configure.
  /// @param request The request, borrowed.
  /// @param stop_token The stop token, borrowed.
  CurlTransfer(CURL* curl, const HttpRequest& request,
               const StopToken& stop_token);

  /// @brief Sets every option on the easy handle.
  ///
  /// @param share The share handle to attach, or null for none.
  /// @return Nothing on success.
  /// @retval HttpError::tls_trust_failed libcurl refused the trust settings.
  /// @retval HttpError::proxy_failed libcurl refused the proxy settings.
  /// @retval HttpError::backend_failure libcurl refused another option.
  [[nodiscard]] auto configure(CURLSH* share) -> Expected<void, HttpError>;
  /// @brief Sets the method and, for methods that send one, the body.
  ///
  /// @return Nothing on success.
  /// @retval HttpError::backend_failure libcurl refused an option or the
  ///     body is too large to describe to it.
  [[nodiscard]] auto configure_method() -> Expected<void, HttpError>;

  /// @brief Accepts only http and https URLs, and only https when
  /// redirects are followed.
  ///
  /// @param value The URL.
  /// @param redirect_policy The request's redirect policy.
  /// @return Nothing when the URL is acceptable.
  /// @retval HttpError::invalid_url The URL is not acceptable.
  [[nodiscard]] static auto validate_url(std::string_view value,
                                         HttpRedirectPolicy redirect_policy)
      -> Expected<void, HttpError>;
  /// @brief Maps a libcurl failure to the HTTP port's errors.
  ///
  /// @param code The failure.
  /// @return The matching error.
  [[nodiscard]] static auto map_curl_error(CURLcode code) noexcept -> HttpError;
  /// @brief libcurl's write callback; appends body bytes to the collector.
  ///
  /// @param data The bytes.
  /// @param size The size of one item.
  /// @param count The number of items.
  /// @param user The ResponseCollector.
  /// @return The bytes accepted; anything short aborts the transfer.
  static std::size_t response_body_callback(char* data, std::size_t size,
                                            std::size_t count,
                                            void* user) noexcept;
  /// @brief libcurl's header callback; records one header line.
  ///
  /// @param data The line.
  /// @param size The size of one item.
  /// @param count The number of items.
  /// @param user The ResponseCollector.
  /// @return The bytes accepted; anything short aborts the transfer.
  static std::size_t response_header_callback(char* data, std::size_t size,
                                              std::size_t count,
                                              void* user) noexcept;

  /// @brief The easy handle, owned by the provider.
  CURL* curl_;
  /// @brief The request being sent.
  const HttpRequest& request_;
  /// @brief The caller's stop token, read by the progress callback.
  const StopToken& stop_token_;
  /// @brief The request headers handed to libcurl.
  HeaderList headers_;
  /// @brief The response libcurl's callbacks fill in.
  ResponseCollector collector_;
};

}  // namespace libchiaking::platform::curl_detail
