#pragma once

#include "src/core/psn/rest/entitlement/library_entitlements.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/psn/rest/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace psn_test {

class LibraryFixture : public ::testing::Test {
 protected:
  using Reply = RestRig::Reply;
  using LibraryEntitlements = libchiaking::psn::rest::LibraryEntitlements;

  static constexpr std::string_view app_entitlements =
      "https://m.np.playstation.com/api/entitlement/v2/users/me/internal/"
      "entitlements";

  static auto app_bearer() -> libchiaking::psn::auth::BearerToken {
    return libchiaking::psn::auth::BearerToken{std::string_view{"APP-FAKE"},
                                               "app bearer"};
  }

  static auto concepts_url(std::size_t offset) -> std::string {
    return std::string{app_entitlements} +
           "?isActive=true&gameMetaPackageType=PS4GS&limit=20&offset=" +
           std::to_string(offset) + "&fields=conceptMeta,titleMeta";
  }

  static auto copies(std::size_t total, std::size_t from, std::size_t count)
      -> std::string {
    std::string body = R"({"revisionId":1,"start":)" + std::to_string(from) +
                       R"(,"totalResults":)" + std::to_string(total) +
                       R"(,"entitlements":[)";
    for (std::size_t index = 0; index < count; ++index) {
      const auto number = std::to_string(10000 + from + index);
      body += std::string{index ? "," : ""} + R"({"id":"EP0100-CUSA)" + number +
              R"(_00-FAKE0107TITLE000","productId":"EP0100-CUSA)" + number +
              R"(_00-GAME000000000000","conceptMeta":{"conceptId":")" + number +
              R"("}})";
    }
    return body + "]}";
  }

  static auto games_url(std::size_t offset) -> std::string {
    return std::string{app_entitlements} +
           "?isActive=true&gameMetaPackageType=PSGD"
           "&sortBy=ACTIVE_DATE&sortDirection=asc&limit=100&offset=" +
           std::to_string(offset) +
           "&useCache=true&fields=titleMeta,gameMeta,productMeta,conceptMeta,"
           "streamingSupported,skuMeta,preorderFlag,preorderCoreFlag,"
           "isPlayTimeFullyConsumed";
  }

  static auto game_url(std::string_view escaped_id) -> std::string {
    return std::string{app_entitlements} + "/" + std::string{escaped_id} +
           "?fields=gameMeta,titleMeta,productMeta,conceptMeta,"
           "streamingSupported,"
           "skuMeta,preorderCoreFlag,isPlayTimeFullyConsumed";
  }

  static auto games(std::size_t total, std::size_t from, std::size_t count,
                    std::uint64_t revision = 7) -> std::string {
    std::string body = R"({"revisionId":)" + std::to_string(revision) +
                       R"(,"start":)" + std::to_string(from) +
                       R"(,"totalResults":)" + std::to_string(total) +
                       R"(,"entitlements":[)";
    for (std::size_t index = 0; index < count; ++index) {
      const auto number = std::to_string(10000 + from + index);
      body +=
          std::string{index ? "," : ""} + R"({"id":"EP0100-PPSA)" + number +
          R"(_00-GAME000000000000","productId":"EP0100-PPSA)" + number +
          R"(_00-GAME000000000000","gameMeta":{"packageType":"PSGD"},"streamingSupported":true})";
    }
    return body + "]}";
  }

  RestRig rig;
  LibraryEntitlements library{rig.http, app_bearer()};
};

}  // namespace psn_test
