#include "src/core/psn/rest/push/push.hpp"
#include "src/core/utils/json/json.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/psn/rest/rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace psn_test {
namespace {

using libchiaking::psn::rest::Cause;
using libchiaking::psn::rest::Push;
namespace bad_reply = libchiaking::psn::rest::bad_reply;
namespace bad_request = libchiaking::psn::rest::bad_request;
using Reply = RestRig::Reply;

class PushServerAddress : public ::testing::Test {
 protected:
  static constexpr std::string_view push_url =
      "https://mobile-pushcl.np.communication.playstation.net/np/serveraddr"
      "?version=2.1&fields=keepAliveStatus&keepAliveStatusType=3";

  static auto fqdn_from(std::string_view body) -> libchiaking::Expected<
      std::string, libchiaking::psn::rest::Push::ServerAddressError> {
    RestRig rig;
    rig.fake.on("GET", std::string{push_url}, {Reply{200, std::string{body}}});
    Push push{rig.http, RestRig::bearer()};
    auto result = push.server_address();
    EXPECT_FALSE(rig.fake.seen.empty());
    if (!rig.fake.seen.empty()) {
      EXPECT_EQ(RestRig::headers_of(rig.fake.seen.at(0)),
                (std::vector<std::string>{
                    "Authorization: Bearer SYNTHETIC_TEST_TOKEN"}));
    }
    return result;
  }

  static auto body(std::string_view host) -> std::string {
    return std::string{R"({"fqdn":")"} + std::string{host} + R"("})";
  }

  static auto host_of(std::size_t last_label) -> std::string {
    return std::string(63, 'a') + "." + std::string(63, 'b') + "." +
           std::string(63, 'c') + "." + std::string(last_label, 'd');
  }

  template <class Reason>
  static auto refused_as(std::string_view text) -> testing::AssertionResult {
    const auto read = fqdn_from(text);
    if (read)
      return testing::AssertionFailure() << text << " gave " << read.value();
    if (!read.error().template is<Reason>())
      return testing::AssertionFailure()
             << text << " was refused as " << read.error().reason_name();
    return testing::AssertionSuccess();
  }
};

TEST_F(PushServerAddress, ReadsAHostName) {
  EXPECT_EQ(fqdn_from(body("push.example.test")).value(), "push.example.test");
  EXPECT_EQ(fqdn_from(body(std::string(63, 'a'))).value(),
            std::string(63, 'a'));
  EXPECT_EQ(fqdn_from(body("xn--p1ai.example")).value(), "xn--p1ai.example");
  EXPECT_EQ(fqdn_from(body(std::string(64, 'a'))).value(),
            std::string(64, 'a'));
}

TEST_F(PushServerAddress, AcceptsA253CharacterHost) {
  const auto max_host = host_of(61);
  ASSERT_EQ(max_host.size(), 253U);
  EXPECT_EQ(fqdn_from(body(max_host)).value(), max_host);
}

TEST_F(PushServerAddress, RejectsALongerHost) {
  EXPECT_TRUE(refused_as<bad_reply::InvalidFqdn>(body(host_of(62))));
}

TEST_F(PushServerAddress, AcceptsLooseButHarmlessHosts) {
  for (const std::string_view accepted :
       {"push.example.test:443", "push_server.np.example.test", "-lead.test",
        "trail-.test", "a.-b.test", "double..dot.test", ".lead.test",
        "trail.test.", "[2001:db8::1]:443"})
    EXPECT_EQ(fqdn_from(body(accepted)).value(), accepted);
}

TEST_F(PushServerAddress, RejectsAnythingButAHost) {
  for (const std::string_view bad :
       {"https://push.example.test", "push.example.test/path",
        "user@push.example.test", "push example.test", "push.test?x=1",
        "push.test#frag", "push\\\\test", "push.test\\r\\nX-Injected: 1",
        "push.test\\u0000"})
    EXPECT_TRUE(refused_as<bad_reply::InvalidFqdn>(body(bad)));
}

TEST_F(PushServerAddress, RejectsAMissingOrNonTextHost) {
  EXPECT_TRUE(refused_as<bad_reply::InvalidSchema>(R"({"fqdn":123})"));
  EXPECT_TRUE(
      refused_as<bad_reply::InvalidSchema>(R"({"url":"push.example.test"})"));
}

TEST_F(PushServerAddress, RejectsInvalidJson) {
  EXPECT_TRUE(refused_as<bad_reply::InvalidJson>("{"));
}

TEST_F(PushServerAddress, KeepsTheLastDuplicateHost) {
  EXPECT_EQ(fqdn_from(R"({"fqdn":"a.test","fqdn":"b.test"})").value(),
            "b.test");
}

TEST_F(PushServerAddress, RejectsAnOversizedReply) {
  EXPECT_TRUE(refused_as<bad_reply::TooLarge>(
      std::string(libchiaking::json::max_document_bytes + 1, ' ')));
}

}  // namespace
}  // namespace psn_test
