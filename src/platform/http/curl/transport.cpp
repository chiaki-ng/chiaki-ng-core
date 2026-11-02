#include <libchiaking/platform/http/curl/transport.hpp>

#include "src/core/utils/memory/c_handle.hpp"
#include "src/platform/curl/transfer.hpp"

#include <curl/curl.h>

namespace libchiaking::platform {

auto CurlCapabilities::detect() noexcept -> CurlCapabilities {
  CurlCapabilities found;
  try {
    if (!curl_detail::ready()) return found;
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    if (info != nullptr) {
      found.version = info->version != nullptr ? info->version : "";
      found.asynchronous_dns = (info->features & CURL_VERSION_ASYNCHDNS) != 0;
      found.http2 = (info->features & CURL_VERSION_HTTP2) != 0;
      found.asynchronous_tls_handshake = true;
    }
    const memory::CHandle<CURLM, curl_multi_cleanup> probe(curl_multi_init());
    found.multi = static_cast<bool>(probe);
    found.wakeup = found.multi && curl_multi_wakeup(probe.get()) == CURLM_OK;
  } catch (...) {
    return CurlCapabilities{};
  }
  return found;
}

}  // namespace libchiaking::platform
