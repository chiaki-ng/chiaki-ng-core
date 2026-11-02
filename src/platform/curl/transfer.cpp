#include "src/platform/curl/transfer.hpp"

#include "src/core/utils/memory/c_handle.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/platform/curl/header_list.hpp"

#include <algorithm>
#include <limits>

namespace libchiaking::platform::curl_detail {

bool ready() noexcept {
  static const CURLcode initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
  return initialized == CURLE_OK;
}

bool contains_nul(std::string_view value) noexcept {
  return value.find('\0') != std::string_view::npos;
}

bool acceptable_header(std::string_view name, std::string_view value) noexcept {
  const auto token = [](char character) {
    return text::ascii_alnum(character) ||
           http_token_punctuation.find(character) != std::string_view::npos;
  };
  return !name.empty() && std::ranges::all_of(name, token) &&
         !contains_nul(value) &&
         value.find_first_of("\r\n") == std::string_view::npos;
}

auto url_scheme(std::string_view url) -> std::optional<std::string> {
  if (url.empty() || contains_nul(url)) return std::nullopt;
  const auto text = terminated(url);
  const memory::CHandle<CURLU, curl_url_cleanup> parsed(curl_url());
  if (!parsed ||
      curl_url_set(parsed.get(), CURLUPART_URL, text.data(), 0) != CURLUE_OK)
    return std::nullopt;
  char* raw_scheme = nullptr;
  if (curl_url_get(parsed.get(), CURLUPART_SCHEME, &raw_scheme, 0) != CURLUE_OK)
    return std::nullopt;
  const memory::CHandle<char, curl_free> scheme(raw_scheme);
  return std::string{scheme.get()};
}

auto milliseconds_option(std::chrono::milliseconds value) noexcept
    -> std::optional<long> {
  if (value.count() < 0 ||
      value.count() > static_cast<std::chrono::milliseconds::rep>(
                          std::numeric_limits<long>::max()))
    return std::nullopt;
  return static_cast<long>(value.count());
}

int stop_on_request(void* stop_token, curl_off_t, curl_off_t, curl_off_t,
                    curl_off_t) noexcept {
  return static_cast<const StopToken*>(stop_token)->stop_requested() ? 1 : 0;
}

bool trust_custom_authorities(CURL* curl,
                              const TlsConfiguration& tls) noexcept {
  if (tls.trust_mode == TlsTrustMode::system) return true;
  curl_blob authorities{
      .data = const_cast<std::byte*>(tls.custom_ca_pem.data()),
      .len = tls.custom_ca_pem.size(),
      .flags = CURL_BLOB_COPY,
  };
  return curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &authorities) == CURLE_OK;
}

bool route_through_proxy(CURL* curl, const TlsConfiguration& tls) noexcept {
  switch (tls.proxy_policy) {
    case HttpProxyPolicy::system:
      return true;
    case HttpProxyPolicy::disabled:
      return curl_easy_setopt(curl, CURLOPT_PROXY, "") == CURLE_OK;
    case HttpProxyPolicy::explicit_proxy:
      return curl_easy_setopt(curl, CURLOPT_PROXY,
                              tls.explicit_proxy_url.c_str()) == CURLE_OK;
  }
  return true;
}

}  // namespace libchiaking::platform::curl_detail
