#include "src/core/psn/rest/push/push.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/psn/rest/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace psn_test {
namespace {

using libchiaking::psn::rest::Cause;
using libchiaking::psn::rest::PushApp;
using libchiaking::psn::rest::PushChannelRequest;
namespace bad_reply = libchiaking::psn::rest::bad_reply;
namespace bad_request = libchiaking::psn::rest::bad_request;

class PushChannel : public ::testing::Test {
 protected:
  static auto url(std::string_view fqdn) -> std::string {
    auto channel = PushChannelRequest::create(RestRig::bearer(), fqdn);
    return channel ? std::string{channel.value().request().url.view()}
                   : std::string{};
  }

  static auto host_of(std::size_t last_label) -> std::string {
    return std::string(63, 'a') + "." + std::string(63, 'b') + "." +
           std::string(63, 'c') + "." + std::string(last_label, 'd');
  }

  static auto header(const PushChannelRequest& channel, std::string_view name)
      -> std::string {
    for (const auto& field : channel.request().headers)
      if (field.name == name) return std::string{field.value.view()};
    return std::string{};
  }
};

TEST_F(PushChannel, OpensOnTheServerAddress) {
  EXPECT_EQ(url("push.example.test"),
            "wss://push.example.test/np/pushNotification");
  EXPECT_EQ(url(host_of(61)), "wss://" + host_of(61) + "/np/pushNotification");
  EXPECT_EQ(url("push_1.example.test:443"),
            "wss://push_1.example.test:443/np/pushNotification");
}

TEST_F(PushChannel, RefusesAnythingButAHost) {
  const auto over_total = host_of(62);
  for (const std::string_view bad :
       {std::string_view{over_total}, std::string_view{""},
        std::string_view{"push.example.test/path"},
        std::string_view{"push.example.test?x"},
        std::string_view{"user@push"}}) {
    const auto channel = PushChannelRequest::create(RestRig::bearer(), bad);
    ASSERT_FALSE(channel) << bad;
    EXPECT_TRUE(channel.error().is<bad_request::InvalidFqdn>()) << bad;
  }
}

TEST_F(PushChannel, NamesTheRemotePlayApp) {
  const auto remote =
      PushChannelRequest::create(RestRig::bearer(), "push.example.test")
          .value();
  EXPECT_EQ(header(remote, "X-PSN-APP-TYPE"), "REMOTE_PLAY");
  EXPECT_EQ(header(remote, "X-PSN-OS-VER"), "Windows/10.0");
}

TEST_F(PushChannel, NamesTheMobileApp) {
  const auto mobile =
      PushChannelRequest::create(RestRig::bearer(), "push.example.test",
                                 PushApp::mobile)
          .value();
  EXPECT_EQ(header(mobile, "X-PSN-APP-TYPE"), "MOBILE_APP.PSAPP");
  EXPECT_EQ(header(mobile, "X-PSN-APP-VER"), "26.10.0");
  EXPECT_EQ(header(mobile, "X-PSN-OS-VER"), "Android/14");
  EXPECT_EQ(header(mobile, "X-PSN-PROTOCOL-VERSION"), "2.1");
  EXPECT_EQ(header(mobile, "X-PSN-KEEP-ALIVE-STATUS-TYPE"), "3");
  EXPECT_TRUE(header(mobile, "User-Agent").empty());
  EXPECT_EQ(mobile.request().subprotocol, "np-pushpacket");
}

}  // namespace
}  // namespace psn_test
