#include "src/core/psn/rest/cloud_assisted_navigation/cloud_assisted_navigation.hpp"
#include "tests/unit/core/psn/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace psn_test {
namespace {

using libchiaking::psn::rest::Cause;
using libchiaking::psn::rest::ClientsPage;
using libchiaking::psn::rest::RegisteredConsole;
namespace bad_reply = libchiaking::psn::rest::bad_reply;
namespace bad_request = libchiaking::psn::rest::bad_request;
using Reply = ClientRig::Reply;

class ConsoleListing : public ::testing::Test {
 protected:
  static constexpr std::string_view uid =
      "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
  static constexpr std::string_view clients_url =
      "https://web.np.playstation.com/api/cloudAssistedNavigation/v2/users/me/"
      "clients"
      "?platform=PS5&includeFields=device,systemData&limit=10&offset=0";

  auto list(Reply reply) -> libchiaking::Expected<
      ClientsPage,
      libchiaking::psn::rest::CloudAssistedNavigation::ListClientsError> {
    rig.http.rules.clear();
    rig.http.on("GET", std::string{clients_url}, {std::move(reply)});
    libchiaking::psn::rest::CloudAssistedNavigation navigation{
        rig.client, libchiaking::psn::auth::BearerToken{"synthetic-token"}};
    return navigation.list_clients(0, false);
  }

  auto list(std::string body) -> libchiaking::Expected<
      ClientsPage,
      libchiaking::psn::rest::CloudAssistedNavigation::ListClientsError> {
    return list(Reply{200, std::move(body)});
  }

  auto row(std::string_view device) -> RegisteredConsole {
    const auto result =
        list(std::string{R"({"clients":[{"duid":")"} + std::string{uid} +
             R"(","device":)" + std::string{device} + "}]}");
    if (!result) {
      ADD_FAILURE() << "the listing failed for " << device;
      return {};
    }
    if (result.value().consoles.size() != 1) {
      ADD_FAILURE() << "the listing held " << result.value().consoles.size()
                    << " consoles for " << device;
      return {};
    }
    return result.value().consoles.front();
  }

  auto plays_remotely(std::string_view device) -> bool {
    return row(device).remote_play_enabled;
  }

  auto addressable(std::string_view device) -> bool {
    return row(device).device_uid.has_value();
  }

  ClientRig rig;
};

TEST_F(ConsoleListing, ParsesTheRemotePlayFlagAndDeviceUid) {
  const auto result = list(Reply{
      200,
      std::string{R"({"clients":[{"duid":")"} + std::string{uid} +
          R"(","device":{"name":"東京 PS5","enabledFeatures":["remotePlay"]}},)" +
          R"({"duid":"bad","device":{"name":"Broken"}},)" + R"({"duid":")" +
          std::string{uid} +
          R"(","device":{"name":"Living Room","enabledFeatures":["other"]}},)" +
          R"({"device":{"name":"Missing ID"}},{"duid":42,"device":{"name":"Wrong Type"}}]})"});
  ASSERT_TRUE(result);
  ASSERT_EQ(rig.http.seen.size(), 1U);
  const auto& seen = rig.http.seen.front();
  EXPECT_EQ(seen.method, "GET");
  EXPECT_EQ(seen.url, clients_url);
  EXPECT_EQ(seen.header("Authorization"), "Bearer synthetic-token");
  EXPECT_EQ(seen.connect_timeout, std::chrono::seconds{10});
  EXPECT_EQ(seen.total_timeout, std::chrono::seconds{30});

  const auto& page = result.value();
  EXPECT_EQ(page.received, 5U);
  ASSERT_EQ(page.consoles.size(), 5U);
  EXPECT_EQ(page.unaddressable, 3U);
  const auto& tokyo = page.consoles[0];
  EXPECT_EQ(tokyo.name, "東京 PS5");
  EXPECT_EQ(tokyo.duid, uid);
  EXPECT_TRUE(tokyo.remote_play_enabled);
  ASSERT_TRUE(tokyo.device_uid);
  EXPECT_EQ((*tokyo.device_uid)[0], std::byte{0x00});
  EXPECT_EQ((*tokyo.device_uid)[1], std::byte{0x11});
  EXPECT_EQ((*tokyo.device_uid)[31], std::byte{0xff});
  EXPECT_EQ(page.consoles[1].name, "Broken");
  EXPECT_FALSE(page.consoles[1].device_uid);
  EXPECT_FALSE(page.consoles[1].remote_play_enabled);
  const auto& living = page.consoles[2];
  EXPECT_EQ(living.name, "Living Room");
  EXPECT_TRUE(living.device_uid);
  EXPECT_FALSE(living.remote_play_enabled);
  EXPECT_FALSE(page.consoles[3].device_uid);
  EXPECT_FALSE(page.consoles[4].device_uid);
}

TEST_F(ConsoleListing, EnablesRemotePlayOnlyForAListedFeature) {
  EXPECT_TRUE(
      plays_remotely(R"({"name":"A","enabledFeatures":["x","remotePlay"]})"));
  EXPECT_FALSE(
      plays_remotely(R"({"name":"A","enabledFeatures":"remotePlay"})"));
  EXPECT_FALSE(plays_remotely(
      R"({"name":"A","enabledFeatures":[7,null,"RemotePlay"]})"));
  EXPECT_FALSE(plays_remotely(R"({"name":"A"})"));
}

TEST_F(ConsoleListing, AddressesOnlyAConsoleWithAUsableName) {
  EXPECT_TRUE(addressable(R"({"name":""})"));
  EXPECT_TRUE(addressable(R"({"name":")" + std::string(512, 'n') + R"("})"));
  EXPECT_FALSE(addressable(R"({"name":")" + std::string(513, 'n') + R"("})"));
  EXPECT_FALSE(addressable(R"({"name":"nul\u0000name"})"));
  EXPECT_FALSE(addressable(R"({"name":7})"));
  EXPECT_FALSE(addressable(R"({})"));
  EXPECT_FALSE(addressable(R"("PS5")"));
}

TEST_F(ConsoleListing, AcceptsAnUppercaseDuidAndSkipsNonObjectRows) {
  const auto upper = list(
      R"({"clients":[{"duid":"00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF",)"
      R"("device":{"name":"A"}},7,{"duid":"0011","device":{"name":"A"}}]})");
  ASSERT_TRUE(upper);
  EXPECT_EQ(upper.value().received, 3U);
  ASSERT_EQ(upper.value().consoles.size(), 2U);
  EXPECT_EQ(upper.value().unaddressable, 2U);
  EXPECT_TRUE(upper.value().consoles[0].device_uid);
  EXPECT_FALSE(upper.value().consoles[1].device_uid);
}

TEST_F(ConsoleListing, RejectsAnInvalidPayload) {
  const auto missing = list("{}");
  ASSERT_FALSE(missing);
  EXPECT_TRUE(missing.error().is<bad_reply::InvalidSchema>());
  const auto wrong_type = list(R"({"clients":{}})");
  ASSERT_FALSE(wrong_type);
  EXPECT_TRUE(wrong_type.error().is<bad_reply::InvalidSchema>());
  const auto not_json = list("clients");
  ASSERT_FALSE(not_json);
  EXPECT_TRUE(not_json.error().is<bad_reply::InvalidJson>());
}

TEST_F(ConsoleListing, ReportsAnErrorStatus) {
  for (const int status : {401, 429, 503}) {
    SCOPED_TRACE(status);
    const auto result = list(Reply{status, ""});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().http_status(), status);
    EXPECT_EQ(result.error().auth_refused(), status == 401);
  }
}

TEST_F(ConsoleListing, ReportsATransportFailure) {
  const auto transport = list(Reply::transport());
  ASSERT_FALSE(transport);
  EXPECT_TRUE(transport.error().failure());
  EXPECT_FALSE(transport.error().is_cancelled());
}

TEST_F(ConsoleListing, SendsNothingOnceStopped) {
  rig.stop();
  const auto result = list(Reply{200, R"({"clients":[]})"});
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().is_cancelled());
  EXPECT_TRUE(rig.http.seen.empty());
}

}  // namespace
}  // namespace psn_test
