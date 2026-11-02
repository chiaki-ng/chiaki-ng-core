#include "src/core/consoles/adapters/search/search_datagram.hpp"
#include "tests/unit/core/consoles/adapters/search/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace consoles_test {
namespace {

using libchiaking::consoles::Beacon;
using libchiaking::consoles::ConsoleName;
using libchiaking::consoles::Firmware;
using libchiaking::consoles::Generation;
using libchiaking::consoles::RunningTitle;
using libchiaking::consoles::SearchDatagram;
using libchiaking::consoles::SearchReply;
using libchiaking::platform::IpAddress;
using libchiaking::testing::FakeConsole;
using libchiaking::testing::InMemoryNetwork;

static_assert(SearchDatagram::standby_status == 620);

class Hearing {
 public:
  auto hear(std::string_view datagram) -> testing::AssertionResult {
    if (!beacon.start())
      return testing::AssertionFailure() << "the beacon did not start";
    const auto local = beacon.local_endpoint();
    if (!local)
      return testing::AssertionFailure() << "the beacon has no endpoint";
    ps5.send_to(local.value().port, datagram);
    const auto received = beacon.receive_once();
    if (!received) return testing::AssertionFailure() << "receiving failed";
    if (!received.value())
      return testing::AssertionFailure() << "nothing was received";
    if (sink.datagrams.size() != 1)
      return testing::AssertionFailure()
             << sink.datagrams.size() << " datagrams were judged, not 1";
    return testing::AssertionSuccess();
  }

  InMemoryNetwork network{SearchRig::threading(), SearchRig::clock()};
  FakeConsole ps4{network};
  FakeConsole ps5{network};
  SearchSink sink;
  Beacon beacon{network, SearchRig::threading(), SearchRig::clock(), sink,
                SearchRig::loopback(ps4, ps5)};
};

class SearchReplyParse : public ::testing::Test {
 protected:
  static constexpr std::string_view ready =
      "HTTP/1.1 200 OK\r\n"
      "system-version:08050001\r\n"
      "device-discovery-protocol-version:00030010\r\n"
      "host-request-port:9295\r\n"
      "host-name:Living Room\r\n"
      "host-type:PS5\r\n"
      "host-id:001122334455\r\n"
      "running-app-titleid:PPSA00001\r\n"
      "running-app-name:Example\r\n";
  static constexpr std::string_view standby =
      "HTTP/1.1 620 Server Standby\n"
      "host-request-port:9295\n";
  static constexpr std::string_view unknown =
      "HTTP/1.1 503 Unavailable\n"
      "host-request-port:9295\n";
  static constexpr std::string_view identity =
      "host-id:001122334455\nhost-type:PS4\n";
  static constexpr std::string_view bad_status = "HTTP/1.0 200 OK\nfoo:bar\n";
  static constexpr std::string_view status_only = "HTTP/1.1 200 OK\n";
  static constexpr std::string_view no_colon = "HTTP/1.1 200 OK\nhost-id\n";

  static auto as_bytes(std::string_view text) -> std::span<const std::byte> {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
  }

  static auto decode(std::string_view text) {
    return SearchDatagram::parse(as_bytes(text), IpAddress::loopback_ipv4());
  }
};

TEST_F(SearchReplyParse, DecodesEveryFieldOfAReadyReply) {
  const auto source = IpAddress::ipv4({192, 168, 1, 42});
  const auto host = SearchDatagram::parse(as_bytes(ready), source);
  ASSERT_TRUE(host);
  EXPECT_EQ(host->status, SearchReply::Status::ready);
  EXPECT_EQ(host->address, source);
  EXPECT_EQ(host->request_port, 9295);
  EXPECT_EQ(host->protocol_version, "00030010");
  EXPECT_EQ(host->name, "Living Room");
  EXPECT_EQ(host->running_app_title_id, "PPSA00001");
}

TEST_F(SearchReplyParse, AdmitsAReadyReplyHeardByTheBeacon) {
  Hearing through;
  ASSERT_TRUE(through.hear(ready));
  EXPECT_TRUE(through.sink.datagrams.front());
  ASSERT_EQ(through.sink.hosts.size(), 1U);
  const auto& reply = through.sink.hosts.front();
  EXPECT_EQ(reply.status(), SearchReply::Status::ready);
  EXPECT_FALSE(reply.in_standby());
  EXPECT_EQ(reply.address(), IpAddress::loopback_ipv4());
  EXPECT_EQ(reply.request_port(), 9295);
  EXPECT_EQ(reply.generation(), Generation::ps5);
  EXPECT_EQ(reply.firmware(), Firmware{Firmware::Family::ps5_1});
  EXPECT_EQ(reply.hardware_id().text(), "001122334455");
  ASSERT_TRUE(reply.name());
  EXPECT_EQ(reply.name()->text(), "Living Room");
  EXPECT_EQ(reply.name()->source(), ConsoleName::Source::discovered);
  EXPECT_EQ(reply.system_version(), "08050001");
  ASSERT_TRUE(reply.running_title());
  EXPECT_EQ(*reply.running_title(), (RunningTitle{"PPSA00001", "Example"}));
}

TEST_F(SearchReplyParse, ASightingCarriesTheReplyFacts) {
  Hearing through;
  ASSERT_TRUE(through.hear(ready));
  ASSERT_EQ(through.sink.hosts.size(), 1U);
  const auto& reply = through.sink.hosts.front();
  const auto sighting = reply.sighting();
  ASSERT_TRUE(sighting);
  EXPECT_EQ(sighting->hardware_id, reply.hardware_id());
  EXPECT_FALSE(sighting->in_rest_mode);
  EXPECT_EQ(sighting->address.text(), "127.0.0.1");
  EXPECT_EQ(sighting->running_title, reply.running_title());
}

TEST_F(SearchReplyParse, KeepsTheStandbyStatus) {
  const auto host = decode(standby);
  ASSERT_TRUE(host);
  EXPECT_EQ(host->status, SearchReply::Status::standby);
}

TEST_F(SearchReplyParse, KeepsAnUnknownStatus) {
  const auto host = decode(unknown);
  ASSERT_TRUE(host);
  EXPECT_EQ(host->status, SearchReply::Status::unknown);
}

TEST_F(SearchReplyParse, AStandbyReplyIsAConsoleInRestMode) {
  Hearing resting;
  ASSERT_TRUE(resting.hear(std::string{standby} + std::string{identity}));
  ASSERT_EQ(resting.sink.hosts.size(), 1U);
  EXPECT_TRUE(resting.sink.hosts.front().in_standby());
  const auto sighting = resting.sink.hosts.front().sighting();
  ASSERT_TRUE(sighting);
  EXPECT_TRUE(sighting->in_rest_mode);
}

TEST_F(SearchReplyParse, AnUnknownStatusIsNeitherStandbyNorRest) {
  Hearing unavailable;
  ASSERT_TRUE(unavailable.hear(std::string{unknown} + std::string{identity}));
  ASSERT_EQ(unavailable.sink.hosts.size(), 1U);
  const auto& reply = unavailable.sink.hosts.front();
  EXPECT_EQ(reply.status(), SearchReply::Status::unknown);
  EXPECT_FALSE(reply.in_standby());
  const auto sighting = reply.sighting();
  ASSERT_TRUE(sighting);
  EXPECT_FALSE(sighting->in_rest_mode);
}

TEST_F(SearchReplyParse, RefusesAnOldHttpVersion) {
  EXPECT_FALSE(decode(bad_status));
}

TEST_F(SearchReplyParse, RefusesAStatusLineAlone) {
  EXPECT_FALSE(decode(status_only));
}

TEST_F(SearchReplyParse, RefusesAHeaderWithoutAColon) {
  EXPECT_FALSE(decode(no_colon));
}

TEST_F(SearchReplyParse, AMalformedDatagramIsJudgedAndNotAdmitted) {
  for (const auto malformed : {bad_status, status_only, no_colon}) {
    Hearing through;
    ASSERT_TRUE(through.hear(malformed)) << malformed;
    EXPECT_FALSE(through.sink.datagrams.front()) << malformed;
    EXPECT_TRUE(through.sink.hosts.empty()) << malformed;
  }
}

TEST_F(SearchReplyParse, ARequestPortAboveSixteenBitsIsZero) {
  const auto wide = decode("HTTP/1.1 200 OK\nhost-request-port:70000\n");
  ASSERT_TRUE(wide);
  EXPECT_EQ(wide->request_port, 0);
}

TEST_F(SearchReplyParse, AHexRequestPortIsZero) {
  const auto hex = decode("HTTP/1.1 200 OK\nhost-request-port:0x2457\n");
  ASSERT_TRUE(hex);
  EXPECT_EQ(hex->request_port, 0);
}

TEST_F(SearchReplyParse, ATextRequestPortIsZeroAndTheRestIsRead) {
  const auto text =
      decode("HTTP/1.1 200 OK\nhost-request-port:port\nhost-id:AA\n");
  ASSERT_TRUE(text);
  EXPECT_EQ(text->request_port, 0);
  EXPECT_EQ(text->id, "AA");
}

TEST_F(SearchReplyParse, ARequestPortWithASuffixIsZero) {
  const auto suffix = decode("HTTP/1.1 200 OK\nhost-request-port:9295x\n");
  ASSERT_TRUE(suffix);
  EXPECT_EQ(suffix->request_port, 0);
}

TEST_F(SearchReplyParse, AnUnterminatedLastHeaderIsDropped) {
  const auto unterminated =
      decode("HTTP/1.1 200 OK\nhost-request-port:9295\nhost-id:AA");
  ASSERT_TRUE(unterminated);
  EXPECT_EQ(unterminated->request_port, 9295);
  EXPECT_TRUE(unterminated->id.empty());
}

TEST_F(SearchReplyParse, AValuelessHeaderIsEmpty) {
  const auto valueless =
      decode("HTTP/1.1 200 OK\nhost-request-port:9295\nhost-name:\n");
  ASSERT_TRUE(valueless);
  EXPECT_EQ(valueless->request_port, 9295);
  EXPECT_TRUE(valueless->name.empty());
}

TEST_F(SearchReplyParse, ANulTerminatedReplyIsRead) {
  constexpr char nul_terminated[] =
      "HTTP/1.1 200 OK\nhost-request-port:9295\nhost-id:\0";
  const auto host =
      decode(std::string_view{nul_terminated, sizeof(nul_terminated) - 1});
  ASSERT_TRUE(host);
  EXPECT_EQ(host->request_port, 9295);
}

TEST_F(SearchReplyParse, KeepsTheFirstOfADuplicatedHeader) {
  const auto duplicate =
      decode("HTTP/1.1 200 OK\nhost-id:first\nhost-id:second\n");
  ASSERT_TRUE(duplicate);
  EXPECT_EQ(duplicate->id, "first");
}

}  // namespace
}  // namespace consoles_test
