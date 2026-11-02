#pragma once

#include "src/core/psn/auth/authenticator.hpp"
#include "src/core/psn/auth/token_cache.hpp"
#include "tests/support/clock.hpp"
#include "tests/unit/core/psn/rig.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace psn_test {

class AuthRig : public ClientRig {
 public:
  using AuthCode = libchiaking::psn::auth::AuthCode;
  using AuthError = libchiaking::psn::auth::AuthError;
  using BearerToken = libchiaking::psn::auth::BearerToken;

  static constexpr std::string_view kamaji_agent =
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";
  static constexpr std::string_view legacy_cloud_stream_agent =
      "Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like "
      "Gecko) "
      "playstation-now/0.0.0 "
      "Chrome/83.0.4103.104 Electron/9.0.4 Safari/537.36 gkApollo";
  static constexpr std::string_view cloud_stream_agent =
      "PlayStation Portal/6.0.0-rel.444+6a9cea6f5";
  static constexpr std::string_view authorize_check_url =
      "https://ca.account.sony.com/api/authz/v3/oauth/authorizeCheck";
  static constexpr std::string_view gk_duid =
      "000000070041008000112233445566778899aabbccddeeff";
  static constexpr std::string_view other_duid =
      "0000000700410080ffeeddccbbaa99887766554433221100";
  static constexpr std::string_view commerce_client =
      "dc523cc2-b51b-4190-bff0-3397c06871b3";
  static constexpr std::string_view checkout_scope =
      "kamaji:get_internal_entitlements user:account.attributes.validate "
      "kamaji:get_privacy_settings "
      "user:account.settings.privacy.get kamaji:s2s.subscriptionsPremium.get";
  static constexpr std::string_view owned_url =
      "https://ca.account.sony.com/api/v1/oauth/authorize?response_type=token"
      "&scope=kamaji%3Aget_internal_entitlements%20user%3Aaccount.attributes."
      "validate"
      "&client_id=dc523cc2-b51b-4190-bff0-3397c06871b3"
      "&redirect_uri=https%3A%2F%2Fpsnow.playstation.com%2Fapp%2F2.2.0%2F133%"
      "2F5cdcc037d%2Fgrc-response.html"
      "&service_entity=urn%3Aservice-entity%3Apsn&prompt=none";
  static constexpr std::string_view catalog_url =
      "https://ca.account.sony.com/api/v1/oauth/"
      "authorize?smcid=pc%3Apsnow&applicationId=psnow"
      "&response_type=code"
      "&scope=kamaji%3Acommerce_native%20kamaji%3Acommerce_container%20kamaji%"
      "3Alists"
      "%20kamaji%3As2s.subscriptionsPremium.get"
      "&client_id=bc6b0777-abb5-40da-92ca-e133cf18e989"
      "&redirect_uri=https%3A%2F%2Fpsnow.playstation.com%2Fapp%2F2.2.0%2F133%"
      "2F5cdcc037d%2Fgrc-response.html"
      "&service_entity=urn%3Aservice-entity%3Apsn&prompt=none&renderMode="
      "mobilePortrait"
      "&hidePageElements=forgotPasswordLink&displayFooter=none&disableLinks="
      "qriocityLink"
      "&mid=PSNOW&duid=0000000700410080ab%3Acd"
      "&layout_type=popup&service_logo=ps&tp_psn=true&noEVBlock=true";

  static auto checkout_url(std::string_view duid) -> std::string {
    return "https://ca.account.sony.com/api/v1/oauth/"
           "authorize?smcid=pc:psnow&applicationId=psnow"
           "&response_type=token"
           "&scope=kamaji:get_internal_entitlements%20user:account.attributes."
           "validate%20kamaji:get_privacy_settings"
           "%20user:account.settings.privacy.get%20kamaji:s2s."
           "subscriptionsPremium.get"
           "&client_id=dc523cc2-b51b-4190-bff0-3397c06871b3"
           "&redirect_uri=https://psnow.playstation.com/app/2.2.0/133/"
           "5cdcc037d/grc-response.html"
           "&grant_type=authorization_code&service_entity=urn:service-entity:"
           "psn&prompt=none"
           "&renderMode=mobilePortrait&hidePageElements=forgotPasswordLink&"
           "displayFooter=none"
           "&disableLinks=qriocityLink&mid=PSNOW&duid=" +
           std::string{duid} +
           "&layout_type=popup&service_logo=ps&tp_psn=true&noEVBlock=true";
  }

  static auto owned_grant() -> libchiaking::psn::auth::Grant {
    return libchiaking::psn::auth::Grant{
        std::string{commerce_client},
        std::string{libchiaking::psn::auth::owned_entitlements_scope},
        libchiaking::psn::auth::ResponseType::token};
  }

  static auto issued(std::string_view token, std::uint32_t expires_in = 0)
      -> libchiaking::psn::auth::TokenCache::Exchange {
    return [token = std::string{token},
            expires_in](const libchiaking::psn::auth::Npsso&)
               -> libchiaking::Expected<libchiaking::psn::auth::Exchanged,
                                        AuthError> {
      return libchiaking::psn::auth::Exchanged{BearerToken{token}, expires_in,
                                               302};
    };
  }

  template <class Value>
  static auto kind_of(const libchiaking::Expected<Value, AuthError>& result)
      -> std::optional<libchiaking::psn::auth::AuthErrorKind> {
    if (result) return std::nullopt;
    return result.error().kind();
  }

  explicit AuthRig(std::string_view npsso = "NPSSO-FAKE")
      : auth{client, libchiaking::psn::auth::Npsso{npsso}} {}

  auto token(Reply reply) -> libchiaking::Expected<BearerToken, AuthError> {
    http.on("GET", std::string{owned_url}, {std::move(reply)});
    return auth.commerce_token(
        libchiaking::psn::auth::owned_entitlements_scope);
  }

  auto code(Reply reply) -> libchiaking::Expected<AuthCode, AuthError> {
    http.on("GET", std::string{catalog_url}, {std::move(reply)});
    return auth.legacy_cloud_stream_catalog_code("0000000700410080ab:cd");
  }

  libchiaking::psn::auth::Authenticator auth;
};

class CachedAuthRig : public ClientRig {
 public:
  using AuthError = AuthRig::AuthError;
  using BearerToken = AuthRig::BearerToken;

  static constexpr std::uint64_t wall_start_us = 1'700'000'000'000'000;

  explicit CachedAuthRig(std::string_view npsso = "NPSSO-A")
      : cache{libchiaking::psn::auth::Npsso{npsso}, threading(), clock, wall},
        auth{client, libchiaking::psn::auth::Npsso{npsso}, &cache} {}

  void owned_tokens(std::vector<Reply> replies) {
    http.on("GET", std::string{AuthRig::owned_url}, std::move(replies));
  }

  void checkout_tokens(std::vector<Reply> replies,
                       std::string_view duid = AuthRig::gk_duid) {
    http.on("GET", AuthRig::checkout_url(duid), std::move(replies));
  }

  auto owned() -> libchiaking::Expected<BearerToken, AuthError> {
    return auth.commerce_token(
        libchiaking::psn::auth::owned_entitlements_scope);
  }

  auto checkout(std::string_view duid = AuthRig::gk_duid)
      -> libchiaking::Expected<BearerToken, AuthError> {
    return auth.legacy_cloud_stream_checkout_token(duid);
  }

  auto owned_view() -> std::string {
    const auto token = owned();
    return token ? std::string{token.value().view()} : std::string{};
  }

  auto exchanges() const -> int { return http.count("oauth/authorize"); }

  test_support::FakeClock clock;
  test_support::FakeClock wall{wall_start_us};
  libchiaking::psn::auth::TokenCache cache;
  libchiaking::psn::auth::Authenticator auth;
};

}  // namespace psn_test
