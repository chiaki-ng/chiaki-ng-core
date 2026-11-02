#pragma once

#include "src/core/psn/legacy_cloud_stream_endpoints.hpp"
#include "src/core/psn/rest/kamaji/kamaji_client.hpp"
#include "src/core/psn/user_agents.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/psn/rest/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace psn_test {

class KamajiFixture : public ::testing::Test {
 protected:
  using Reply = RestRig::Reply;
  using Headers = std::vector<std::pair<std::string, std::string>>;

  static constexpr std::string_view api =
      "https://psnow.playstation.com/kamaji/api/pcnow/00_09_000";
  static constexpr std::string_view store =
      "https://psnow.playstation.com/store/api/pcnow/00_09_000/container/";
  static constexpr std::string_view browser_agent =
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";
  static constexpr std::string_view referer =
      "https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/";
  static constexpr std::string_view alt_referer =
      "https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/"
      "grc-response.html";
  static constexpr std::string_view duid =
      "0000000700410080000102030405060708090a0b0c0d0e0f";
  static constexpr std::string_view free_cart =
      R"({"header":{"status_code":"0x0000"},"data":{"cart":{"total_price_value":0,"items":[{"sku_id":"SKU-SERVER"}]}}})";
  static constexpr std::string_view accepted =
      R"({"header":{"status_code":"0x0000"}})";

  static auto code(std::string_view value = "CODE1")
      -> libchiaking::psn::auth::AuthCode {
    return libchiaking::psn::auth::AuthCode{value, "legacy_cloud_stream code"};
  }

  static auto bearer() -> libchiaking::psn::auth::BearerToken {
    return libchiaking::psn::auth::BearerToken{std::string_view{"BEARER-FAKE"},
                                               "commerce bearer"};
  }

  static auto stream_agent() -> std::string {
    return std::string{libchiaking::psn::legacy_cloud_stream_http_user_agent};
  }

  static auto headers_match(const RestRig::Seen& seen, const Headers& expected)
      -> testing::AssertionResult {
    if (seen.headers.size() != expected.size())
      return testing::AssertionFailure()
             << seen.url << " sent " << seen.headers.size()
             << " headers, expected " << expected.size();
    for (std::size_t index = 0; index < expected.size(); ++index) {
      const auto& header = seen.headers[index];
      if (header.name != expected[index].first ||
          header.value != expected[index].second)
        return testing::AssertionFailure()
               << seen.url << " header " << index << " is " << header.name
               << ": " << header.value << ", expected " << expected[index].first
               << ": " << expected[index].second;
    }
    return testing::AssertionSuccess();
  }

  void opens_session(std::vector<test_support::psn::TestHeader> headers) {
    Reply reply{
        200,
        R"({"header":{"status_code":"0x0000"},"data":{"country":"GB","language":"en"}})"};
    reply.headers = std::move(headers);
    rig.fake.on("POST", std::string{api} + "/user/session", {reply});
    const auto created = client.create_session(code(), duid);
    ASSERT_TRUE(created);
    EXPECT_EQ(created.value().http_status, 200);
    EXPECT_TRUE(created.value().accepted);
    EXPECT_EQ(created.value().country, "GB");
    EXPECT_EQ(created.value().language, "en");
  }

  RestRig rig;
  libchiaking::psn::rest::KamajiClient client{rig.http};
};

}  // namespace psn_test
