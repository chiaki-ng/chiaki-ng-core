#include "src/core/consoles/adapters/search/search_datagram.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace consoles_test {
namespace {

using libchiaking::consoles::SearchDatagram;
using libchiaking::consoles::SearchReply;
using libchiaking::platform::IpAddress;
using Refusal = SearchDatagram::Refusal;

static_assert(SearchDatagram::reply_limit == 4096);
static_assert(SearchDatagram::id_limit == 63 &&
              SearchDatagram::name_limit == 127);
static_assert(SearchDatagram::type_limit == 31 &&
              SearchDatagram::system_version_limit == 31);
static_assert(SearchDatagram::protocol_version_limit == 31);
static_assert(SearchDatagram::running_app_title_id_limit == 31 &&
              SearchDatagram::running_app_name_limit == 127);
static_assert(SearchDatagram::receive_limit == 4097);

class SearchReplyAdmission : public ::testing::Test {
 protected:
  static constexpr std::string_view ps5_reply =
      "HTTP/1.1 620 Server Standby\nhost-id:AABBCCDDEEFF\nhost-type:PS5\n"
      "host-name:Loopback fixture\nhost-request-port:997\n"
      "system-version:08050001\ndevice-discovery-protocol-version:00030010\n";
  static constexpr std::string_view ps4_reply =
      "HTTP/1.1 200 Ok\nhost-id:001122334455\nhost-type:PS4\n"
      "host-name:PS4 fixture\n";
  static constexpr std::string_view anonymous_reply =
      "HTTP/1.1 200 Ok\nhost-id:not-a-mac-id\nhost-type:PS4\n";
  static constexpr std::string_view prefix =
      "HTTP/1.1 200 Ok\nhost-type:PS4\nhost-id:001122334455\n";

  static auto bytes_of(std::string_view reply) -> std::span<const std::byte> {
    return std::as_bytes(std::span{reply.data(), reply.size()});
  }

  static auto admitted(std::string_view reply,
                       IpAddress source = IpAddress::loopback_ipv4())
      -> libchiaking::Expected<SearchReply, Refusal> {
    return SearchDatagram::admit(bytes_of(reply), source);
  }

  static auto by_reference_rules(std::string_view reply,
                                 IpAddress source = IpAddress::loopback_ipv4())
      -> std::optional<SearchReply> {
    const auto fields = SearchDatagram::parse(bytes_of(reply), source);
    return fields ? SearchDatagram::from(*fields) : std::nullopt;
  }

  static auto refused_as(std::string_view reply, Refusal refusal)
      -> testing::AssertionResult {
    const auto strict = admitted(reply);
    if (strict) return testing::AssertionFailure() << "the reply was admitted";
    if (strict.error() != refusal)
      return testing::AssertionFailure()
             << "refused as " << testing::PrintToString(strict.error())
             << ", not " << testing::PrintToString(refusal);
    return testing::AssertionSuccess();
  }

  const std::string base{ps4_reply};
};

TEST_F(SearchReplyAdmission, AdmitsAPs5Reply) {
  const auto ps5 = admitted(ps5_reply);
  ASSERT_TRUE(ps5);
  EXPECT_EQ(ps5.value().hardware_id().text(), "aabbccddeeff");
  EXPECT_EQ(ps5.value().request_port(), 997);
}

TEST_F(SearchReplyAdmission, AdmitsAPs4ReplyWithoutARequestPort) {
  const auto ps4 = admitted(ps4_reply);
  ASSERT_TRUE(ps4);
  EXPECT_EQ(ps4.value().request_port(), 0);
}

TEST_F(SearchReplyAdmission, AdmitsCarriageReturnLineEnds) {
  EXPECT_TRUE(admitted(
      "HTTP/1.1 200 Ok\r\nhost-id:001122334455\r\nhost-type:PS4\r\n\r\n"));
}

TEST_F(SearchReplyAdmission, RefusesADuplicateKnownHeader) {
  const std::string duplicate = base + "host-name:Second\n";
  EXPECT_TRUE(refused_as(duplicate, Refusal::duplicate_header));
  EXPECT_TRUE(
      refused_as(base + "Host-Id:001122334455\n", Refusal::duplicate_header));
  const auto first_wins = by_reference_rules(duplicate);
  ASSERT_TRUE(first_wins);
  ASSERT_TRUE(first_wins->name());
  EXPECT_EQ(first_wins->name()->text(), "PS4 fixture");
}

TEST_F(SearchReplyAdmission, ScreensNamesOfAnyCaseButReadsOnlyLowerCase) {
  const auto renamed = admitted(
      "HTTP/1.1 200 Ok\nhost-id:001122334455\nhost-type:PS4\n"
      "Host-Name:Cased\n");
  ASSERT_TRUE(renamed);
  EXPECT_FALSE(renamed.value().name());
  EXPECT_TRUE(refused_as(
      "HTTP/1.1 200 Ok\nhost-id:001122334455\nhost-type:PS4\nHOST-NAME:" +
          std::string(SearchDatagram::name_limit + 1, 'n') + "\n",
      Refusal::field_too_long));
}

TEST_F(SearchReplyAdmission, CountsTrailingBlanksTowardsAFieldLimit) {
  const std::string name(SearchDatagram::name_limit, 'n');
  EXPECT_TRUE(admitted(base.substr(0, base.find("host-name")) +
                       "host-name: \t" + name + "\n"));
  EXPECT_TRUE(refused_as(
      base.substr(0, base.find("host-name")) + "host-name:" + name + " \n",
      Refusal::field_too_long));
}

TEST_F(SearchReplyAdmission, AdmitsADuplicateUnknownHeader) {
  EXPECT_TRUE(admitted(base + "x-extra:1\nx-extra:2\n"));
}

TEST_F(SearchReplyAdmission, RefusesALineWithoutAColon) {
  EXPECT_TRUE(
      refused_as(base + "\n\nnot a header\n", Refusal::line_without_colon));
}

TEST_F(SearchReplyAdmission, RefusesAnEmbeddedNul) {
  const std::string terminated = base + '\0';
  EXPECT_TRUE(refused_as(terminated, Refusal::embedded_nul));
  EXPECT_TRUE(by_reference_rules(terminated));
}

TEST_F(SearchReplyAdmission, RefusesAnInvalidRequestPort) {
  for (const std::string_view port :
       {"0", "65536", "99999999999", "", "9x", "-1"})
    EXPECT_TRUE(
        refused_as(base + "host-request-port:" + std::string{port} + "\n",
                   Refusal::invalid_request_port))
        << port;
}

TEST_F(SearchReplyAdmission, AdmitsTheHighestRequestPort) {
  const auto highest = admitted(base + "host-request-port:65535\n");
  ASSERT_TRUE(highest);
  EXPECT_EQ(highest.value().request_port(), 65535);
  const auto wrapped = by_reference_rules(base + "host-request-port:65536\n");
  ASSERT_TRUE(wrapped);
  EXPECT_EQ(wrapped->request_port(), 0);
}

TEST_F(SearchReplyAdmission, RefusesAnOversizedReply) {
  const std::string padded =
      base + "x-padding:" + std::string(SearchDatagram::reply_limit, 'p') +
      "\n";
  EXPECT_TRUE(refused_as(padded, Refusal::oversized));
  EXPECT_TRUE(by_reference_rules(padded));
}

TEST_F(SearchReplyAdmission, RefusesAnIpv6Source) {
  const auto ipv6 = admitted(ps5_reply, IpAddress::loopback_ipv6());
  ASSERT_FALSE(ipv6);
  EXPECT_EQ(ipv6.error(), Refusal::not_ipv4);
  EXPECT_TRUE(
      SearchDatagram::parse(bytes_of(ps5_reply), IpAddress::loopback_ipv6()));
}

TEST_F(SearchReplyAdmission, RefusesAnUnusableSource) {
  for (const auto unusable :
       {IpAddress::ipv4({0, 0, 0, 0}), IpAddress::ipv4({0, 1, 2, 3}),
        IpAddress::ipv4({224, 0, 0, 251}),
        IpAddress::ipv4({255, 255, 255, 255})}) {
    const auto refused = admitted(ps5_reply, unusable);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error(), Refusal::unusable_source);
  }
}

TEST_F(SearchReplyAdmission, AdmitsPrivateSharedAndLoopbackSources) {
  for (const auto usable :
       {IpAddress::ipv4({10, 0, 0, 2}), IpAddress::ipv4({100, 64, 1, 9}),
        IpAddress::ipv4({192, 168, 1, 20}), IpAddress::loopback_ipv4()})
    EXPECT_TRUE(admitted(ps5_reply, usable));
}

TEST_F(SearchReplyAdmission, AdmitsANameAtTheLimit) {
  EXPECT_TRUE(admitted(std::string{prefix} + "host-name:" +
                       std::string(SearchDatagram::name_limit, 'n') + "\n"));
}

TEST_F(SearchReplyAdmission, RefusesAFieldOverItsLimit) {
  const auto long_name =
      std::string{prefix} +
      "host-name:" + std::string(SearchDatagram::name_limit + 1, 'n') + "\n";
  EXPECT_TRUE(refused_as(long_name, Refusal::field_too_long));
  EXPECT_TRUE(by_reference_rules(long_name));
  EXPECT_TRUE(refused_as("HTTP/1.1 200 Ok\nhost-type:PS4\nhost-id:" +
                             std::string(SearchDatagram::id_limit + 1, 'a') +
                             "\n",
                         Refusal::field_too_long));
  EXPECT_TRUE(refused_as(
      std::string{prefix} + "running-app-name:" +
          std::string(SearchDatagram::running_app_name_limit + 1, 'g') + "\n",
      Refusal::field_too_long));
  EXPECT_TRUE(refused_as(
      std::string{prefix} + "system-version:" +
          std::string(SearchDatagram::system_version_limit + 1, '1') + "\n",
      Refusal::field_too_long));
}

TEST_F(SearchReplyAdmission, RefusesAReplyThatIsNotAConsole) {
  EXPECT_TRUE(refused_as(anonymous_reply, Refusal::not_a_console));
}

TEST_F(SearchReplyAdmission, RefusesAMalformedReply) {
  EXPECT_TRUE(refused_as("not a discovery reply", Refusal::malformed));
}

}  // namespace
}  // namespace consoles_test
