#pragma once

#include "src/core/psn/rest/commerce/commerce_client.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/psn/rest/rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace psn_test {

class CommerceFixture : public ::testing::Test {
 protected:
  using Reply = RestRig::Reply;

  static constexpr std::string_view entitlements =
      "https://commerce.api.np.km.playstation.net/commerce/api/v1/users/me/"
      "internal_entitlements";
  static constexpr std::string_view app_agent = "AGENT/1.0";

  static auto bearer() -> libchiaking::psn::auth::BearerToken {
    return libchiaking::psn::auth::BearerToken{std::string_view{"BEARER-FAKE"},
                                               "commerce bearer"};
  }

  static auto page_url(int start) -> std::string {
    return std::string{entitlements} +
           "?fields=game_meta%2Csku_meta&entitlement_type=5&start=" +
           std::to_string(start) + "&size=300";
  }

  static auto page_of(int count) -> std::string {
    std::string body = R"({"entitlements":[)";
    for (int index = 0; index < count; ++index) body += (index ? ",{}" : "{}");
    return body + "]}";
  }

  static auto single_url(std::string_view escaped_id) -> std::string {
    return std::string{entitlements} + "/" + std::string{escaped_id} +
           "?fields=game_meta%2Csku_meta";
  }

  RestRig rig;
  libchiaking::psn::rest::CommerceClient client{rig.http};
};

}  // namespace psn_test
