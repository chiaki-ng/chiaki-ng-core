#include "src/core/utils/diagnostics/sensitive_text.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace diagnostics_test {
namespace {

using libchiaking::diagnostics::SensitiveText;

TEST(SensitiveJsonKeys, MaskCredentialsCodesAndKeys) {
  for (const std::string_view secret : {"access_token",
                                        "accessToken",
                                        "refresh_token",
                                        "id_token",
                                        "TOKEN_TYPE",
                                        "npsso",
                                        "client_secret",
                                        "clientSecret",
                                        "password",
                                        "newPassword",
                                        "code",
                                        "CODE",
                                        "authCode",
                                        "auth_code",
                                        "authorization_code",
                                        "authorizationCode",
                                        "device_code",
                                        "deviceCode",
                                        "user_code",
                                        "userCode",
                                        "gkCloudAuthCode",
                                        "streamServerAuthCode",
                                        "ps3AuthCode",
                                        "key",
                                        "skey",
                                        "handshakeKey",
                                        "configKey",
                                        "lockKey",
                                        "session_key",
                                        "sessionKey",
                                        "api_key",
                                        "apiKey",
                                        "privateKey",
                                        "SKEY",
                                        "postalCode",
                                        "postal_code",
                                        "zipCode",
                                        "zip_code"})
    EXPECT_TRUE(SensitiveText::json_key(secret)) << secret;
}

TEST(SensitiveJsonKeys, LeaveStatusCodesAndLookalikesReadable) {
  for (const std::string_view visible :
       {"status_code", "statusCode", "errorCode", "error_code", "reasonCode",
        "resultCode", "countryCode", "languageCode", "currencyCode",
        "regionCode", "videoCodec", "keyArt", "keyboard", "expires_in",
        "sessionId", "", "ode", "ey"})
    EXPECT_FALSE(SensitiveText::json_key(visible)) << visible;
}

}  // namespace
}  // namespace diagnostics_test
