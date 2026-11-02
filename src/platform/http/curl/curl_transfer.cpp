#include "src/platform/http/curl/curl_transfer.hpp"

#include "src/platform/curl/native_trust.hpp"
#include "src/platform/curl/transfer.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace libchiaking::platform::curl_detail {

CurlTransfer::ResponseCollector::ResponseCollector(
    std::size_t maximum_response_bytes) noexcept
    : maximum_response_bytes_(maximum_response_bytes) {}

auto CurlTransfer::ResponseCollector::append_body(
    std::span<const std::byte> bytes) -> Expected<std::size_t, HttpError> {
  if (error_) return unexpected(*error_);
  const std::size_t current_size = response_.body.size();
  if (bytes.size() > std::numeric_limits<std::size_t>::max() - current_size ||
      (maximum_response_bytes_ != 0 &&
       (current_size > maximum_response_bytes_ ||
        bytes.size() > maximum_response_bytes_ - current_size)))
    return fail(HttpError::response_too_large);
  try {
    if (!response_.body.try_append(bytes))
      return fail(HttpError::backend_failure);
  } catch (...) {
    return fail(HttpError::backend_failure);
  }
  return bytes.size();
}

auto CurlTransfer::ResponseCollector::append_header_line(std::string_view line)
    -> Expected<std::size_t, HttpError> {
  if (error_) return unexpected(*error_);
  const auto byte_count = line.size();
  try {
    if (line.starts_with("HTTP/")) {
      response_.headers.clear();
      response_.reason.clear();
      while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.remove_suffix(1);
      const std::size_t status_separator = line.find(' ');
      if (status_separator != std::string_view::npos) {
        const std::size_t reason_separator =
            line.find(' ', status_separator + 1);
        if (reason_separator != std::string_view::npos)
          response_.reason.assign(line.substr(reason_separator + 1));
      }
      return byte_count;
    }

    const std::size_t separator = line.find(':');
    if (separator == std::string_view::npos) return byte_count;
    std::string_view value = line.substr(separator + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
      value.remove_prefix(1);
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
      value.remove_suffix(1);
    response_.headers.push_back(HttpHeader{
        .name = std::string(line.substr(0, separator)),
        .value = SecretString{value},
    });
    return byte_count;
  } catch (...) {
    return fail(HttpError::backend_failure);
  }
}

void CurlTransfer::ResponseCollector::record_failure(HttpError error) noexcept {
  (void)fail(error);
}

auto CurlTransfer::ResponseCollector::error() const noexcept
    -> std::optional<HttpError> {
  return error_;
}

auto CurlTransfer::ResponseCollector::take()
    -> Expected<HttpResponse, HttpError> {
  if (error_) return unexpected(*error_);
  HttpResponse taken = std::move(response_);
  response_ = HttpResponse{};
  return taken;
}

auto CurlTransfer::ResponseCollector::fail(HttpError error) noexcept
    -> Unexpected<HttpError> {
  if (!error_) error_ = error;
  return unexpected(error);
}

CurlTransfer::CurlTransfer(CURL* curl, const HttpRequest& request,
                           const StopToken& stop_token)
    : curl_(curl),
      request_(request),
      stop_token_(stop_token),
      collector_(request.maximum_response_bytes) {}

auto CurlTransfer::prepare(CURL* curl, CURLSH* share,
                           const HttpRequest& request,
                           const StopToken& stop_token)
    -> Expected<std::unique_ptr<CurlTransfer>, HttpError> {
  try {
    if (const auto checked = check(request, stop_token); !checked)
      return unexpected(checked.error());
    if (curl == nullptr) return unexpected(HttpError::backend_failure);
    std::unique_ptr<CurlTransfer> transfer(
        new CurlTransfer(curl, request, stop_token));
    if (const auto configured = transfer->configure(share); !configured)
      return unexpected(configured.error());
    return transfer;
  } catch (...) {
    return unexpected(HttpError::backend_failure);
  }
}

auto CurlTransfer::handle() const noexcept -> CURL* { return curl_; }

auto CurlTransfer::check(const HttpRequest& request,
                         const StopToken& stop_token)
    -> Expected<void, HttpError> {
  if (!ready()) return unexpected(HttpError::backend_failure);
  if (stop_token.stop_requested()) return unexpected(HttpError::cancelled);
  if (const auto valid_url =
          validate_url(request.url.view(), request.redirect_policy);
      !valid_url)
    return unexpected(valid_url.error());
  if (!std::ranges::all_of(request.headers, [](const HttpHeader& header) {
        return acceptable_header(header.name, header.value.view());
      }))
    return unexpected(HttpError::backend_failure);
  if (const auto usable = usable_tls<HttpError>(request.tls_configuration);
      !usable)
    return unexpected(usable.error());
  if (!milliseconds_option(request.connect_timeout) ||
      !milliseconds_option(request.total_timeout))
    return unexpected(HttpError::backend_failure);
  return {};
}

auto CurlTransfer::configure(CURLSH* share) -> Expected<void, HttpError> {
  auto set = [this](CURLoption option, auto value) noexcept {
    return curl_easy_setopt(curl_, option, value) == CURLE_OK;
  };

  if (share != nullptr && !set(CURLOPT_SHARE, share))
    return unexpected(HttpError::backend_failure);

  const auto connect_timeout = milliseconds_option(request_.connect_timeout);
  const auto total_timeout = milliseconds_option(request_.total_timeout);
  const auto url = terminated(request_.url.view());
  if (!set(CURLOPT_URL, url.data()) ||
      !set(CURLOPT_PROTOCOLS_STR, "http,https") ||
      !set(CURLOPT_SSL_VERIFYPEER, 1L) || !set(CURLOPT_SSL_VERIFYHOST, 2L) ||
      !set(CURLOPT_NOSIGNAL, 1L) ||
      !set(CURLOPT_CONNECTTIMEOUT_MS, connect_timeout.value()) ||
      !set(CURLOPT_TIMEOUT_MS, total_timeout.value()) ||
      !set(CURLOPT_XFERINFOFUNCTION, stop_on_request) ||
      !set(CURLOPT_XFERINFODATA, &stop_token_) || !set(CURLOPT_NOPROGRESS, 0L))
    return unexpected(HttpError::backend_failure);

  if (request_.redirect_policy == HttpRedirectPolicy::follow_https) {
    if (!set(CURLOPT_FOLLOWLOCATION, 1L) ||
        !set(CURLOPT_REDIR_PROTOCOLS_STR, "https"))
      return unexpected(HttpError::backend_failure);
  } else if (!set(CURLOPT_FOLLOWLOCATION, 0L)) {
    return unexpected(HttpError::backend_failure);
  }

  for (const auto& header : request_.headers) {
    if (!headers_.append(header)) return unexpected(HttpError::backend_failure);
  }
  if (headers_.get() != nullptr && !set(CURLOPT_HTTPHEADER, headers_.get()))
    return unexpected(HttpError::backend_failure);

  if (const auto method = configure_method(); !method)
    return unexpected(method.error());

  const auto& tls = request_.tls_configuration;
  if (const auto applied =
          apply_tls<HttpError>(curl_, tls, http_ssl_options(tls.trust_mode));
      !applied)
    return unexpected(applied.error());

  if (!set(CURLOPT_WRITEFUNCTION, response_body_callback) ||
      !set(CURLOPT_WRITEDATA, &collector_) ||
      !set(CURLOPT_HEADERFUNCTION, response_header_callback) ||
      !set(CURLOPT_HEADERDATA, &collector_))
    return unexpected(HttpError::backend_failure);
  return {};
}

auto CurlTransfer::configure_method() -> Expected<void, HttpError> {
  auto set = [this](CURLoption option, auto value) noexcept {
    return curl_easy_setopt(curl_, option, value) == CURLE_OK;
  };

  const char* const body_data =
      request_.body.empty() ? "" : request_.body.view().data();
  if (request_.body.size() >
      static_cast<std::size_t>(std::numeric_limits<curl_off_t>::max()))
    return unexpected(HttpError::backend_failure);
  const auto body_size = static_cast<curl_off_t>(request_.body.size());
  bool configured = false;
  switch (request_.method) {
    case HttpMethod::get:
      configured = set(CURLOPT_HTTPGET, 1L);
      break;
    case HttpMethod::post:
      configured = set(CURLOPT_POST, 1L) &&
                   set(CURLOPT_POSTFIELDS, body_data) &&
                   set(CURLOPT_POSTFIELDSIZE_LARGE, body_size);
      break;
    case HttpMethod::delete_:
      configured = set(CURLOPT_CUSTOMREQUEST, "DELETE") &&
                   set(CURLOPT_POSTFIELDS, body_data) &&
                   set(CURLOPT_POSTFIELDSIZE_LARGE, body_size);
      break;
    case HttpMethod::head:
      configured = set(CURLOPT_NOBODY, 1L);
      break;
    case HttpMethod::put:
      configured = set(CURLOPT_CUSTOMREQUEST, "PUT") &&
                   set(CURLOPT_POSTFIELDS, body_data) &&
                   set(CURLOPT_POSTFIELDSIZE_LARGE, body_size);
      break;
    case HttpMethod::patch:
      configured = set(CURLOPT_CUSTOMREQUEST, "PATCH") &&
                   set(CURLOPT_POSTFIELDS, body_data) &&
                   set(CURLOPT_POSTFIELDSIZE_LARGE, body_size);
      break;
  }
  if (!configured) return unexpected(HttpError::backend_failure);
  return {};
}

auto CurlTransfer::finish(CURLcode result)
    -> Expected<HttpResponse, HttpError> {
  try {
    if (const auto callback_error = collector_.error())
      return unexpected(*callback_error);
    if (result != CURLE_OK) {
      if (result == CURLE_ABORTED_BY_CALLBACK && stop_token_.stop_requested())
        return unexpected(HttpError::cancelled);
      if (result == CURLE_URL_MALFORMAT &&
          request_.tls_configuration.proxy_policy ==
              HttpProxyPolicy::explicit_proxy)
        return unexpected(HttpError::proxy_failed);
      return unexpected(map_curl_error(result));
    }

    auto response = collector_.take();
    if (!response) return unexpected(response.error());

    long status_code = 0;
    char* effective_url = nullptr;
    if (curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status_code) !=
            CURLE_OK ||
        status_code < 0 ||
        status_code >
            static_cast<long>(std::numeric_limits<std::uint16_t>::max()) ||
        curl_easy_getinfo(curl_, CURLINFO_EFFECTIVE_URL, &effective_url) !=
            CURLE_OK ||
        effective_url == nullptr)
      return unexpected(HttpError::backend_failure);
    response.value().status_code = static_cast<std::uint16_t>(status_code);
    response.value().final_url.assign(std::string_view{effective_url});
    return response;
  } catch (...) {
    return unexpected(HttpError::backend_failure);
  }
}

auto CurlTransfer::validate_url(std::string_view value,
                                HttpRedirectPolicy redirect_policy)
    -> Expected<void, HttpError> {
  const auto scheme = url_scheme(value);
  if (!scheme || (*scheme != "http" && *scheme != "https"))
    return unexpected(HttpError::invalid_url);
  if (redirect_policy == HttpRedirectPolicy::follow_https && *scheme != "https")
    return unexpected(HttpError::invalid_url);
  return {};
}

auto CurlTransfer::map_curl_error(CURLcode code) noexcept -> HttpError {
  if (code == CURLE_PARTIAL_FILE) return HttpError::disconnected;
  return failure_of<HttpError>(code);
}

std::size_t CurlTransfer::response_body_callback(char* data, std::size_t size,
                                                 std::size_t count,
                                                 void* user) noexcept {
  auto& collector = *static_cast<ResponseCollector*>(user);
  if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
    collector.record_failure(HttpError::response_too_large);
    return 0;
  }
  const auto bytes =
      std::span(reinterpret_cast<const std::byte*>(data), size * count);
  const auto appended = collector.append_body(bytes);
  return appended ? appended.value() : 0;
}

std::size_t CurlTransfer::response_header_callback(char* data, std::size_t size,
                                                   std::size_t count,
                                                   void* user) noexcept {
  auto& collector = *static_cast<ResponseCollector*>(user);
  if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
    collector.record_failure(HttpError::backend_failure);
    return 0;
  }
  const auto appended =
      collector.append_header_line(std::string_view(data, size * count));
  return appended ? appended.value() : 0;
}

}  // namespace libchiaking::platform::curl_detail
