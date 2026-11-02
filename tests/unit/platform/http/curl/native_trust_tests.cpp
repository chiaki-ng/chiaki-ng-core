#include "src/platform/curl/native_trust.hpp"

#include <gtest/gtest.h>

namespace platform_curl_test {
namespace {

using libchiaking::platform::TlsTrustMode;
using libchiaking::platform::curl_detail::http_ssl_options;
using libchiaking::platform::curl_detail::uses_native_ca;

TEST(CurlNativeTrust, IsSkippedForACustomCaAlone) {
  EXPECT_FALSE(uses_native_ca(TlsTrustMode::custom_ca_pem));
  EXPECT_EQ(http_ssl_options(TlsTrustMode::custom_ca_pem), 0);
}

TEST(CurlNativeTrust, IsUsedAlongsideACustomCa) {
  EXPECT_TRUE(uses_native_ca(TlsTrustMode::system_plus_custom_ca_pem));
}

#ifdef __APPLE__
TEST(CurlNativeTrust, BacksSystemTrustWithoutPartialChainsOnApple) {
  EXPECT_TRUE(uses_native_ca(TlsTrustMode::system));
  EXPECT_EQ(http_ssl_options(TlsTrustMode::system),
            CURLSSLOPT_NATIVE_CA | CURLSSLOPT_NO_PARTIALCHAIN);
}
#else
TEST(CurlNativeTrust, IsAddedOnlyForACustomCaOutsideApple) {
  EXPECT_EQ(http_ssl_options(TlsTrustMode::system_plus_custom_ca_pem),
            CURLSSLOPT_NATIVE_CA);
  EXPECT_FALSE(uses_native_ca(TlsTrustMode::system));
}
#endif

}  // namespace
}  // namespace platform_curl_test
