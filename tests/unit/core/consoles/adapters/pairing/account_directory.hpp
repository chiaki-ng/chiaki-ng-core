#pragma once

#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/consoles/account_rig.hpp"

#include <libchiaking/consoles_types.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace consoles_test {

class AccountDirectory : public ::testing::Test {
 protected:
  static constexpr std::string_view store_token = "fake-mobile-token-40aa";
  static constexpr std::string_view remote_token = "fake-remote-token-7d40";
  static constexpr std::string_view renewed_remote_token =
      "fake-remote-token-renewed-19f2";
  static constexpr std::string_view den = "a1b2c3d4e5f6";
  static constexpr std::string_view consoles_url =
      "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/"
      "clients"
      "?platform=PS5&includeFields=device,systemData&limit=10&offset=0";
  static constexpr std::string_view second_page_url =
      "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/"
      "clients"
      "?platform=PS5&includeFields=device,systemData&limit=10&offset=10";

  static auto uid(char digit) -> libchiaking::consoles::DeviceUid {
    return libchiaking::consoles::DeviceUid::parse(std::string(64, digit))
        .value();
  }

  static auto client(std::string_view duid, std::string_view name,
                     std::string_view features = R"(["remotePlay"])")
      -> std::string {
    return R"({"duid":")" + std::string{duid} + R"(","device":{"name":")" +
           std::string{name} + R"(","enabledFeatures":)" +
           std::string{features} + "}}";
  }

  static auto page(const std::vector<std::string>& clients) -> std::string {
    std::string list;
    for (const auto& entry : clients) list += (list.empty() ? "" : ",") + entry;
    return R"({"clients":[)" + list + "]}";
  }

  Account account;
};

}  // namespace consoles_test
