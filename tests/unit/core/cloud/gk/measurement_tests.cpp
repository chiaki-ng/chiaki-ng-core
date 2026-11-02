#include "src/core/psn/gk/measurement.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace cloud_test {
namespace {

namespace gk = libchiaking::psn::gk;

class GkPingResultsJson : public ::testing::Test {
 protected:
  static auto row() -> gk::Ping {
    gk::Ping result;
    result.name = "us-west";
    result.rtt_ms = 21;
    result.rtts_ms = {22, 20, 21};
    result.mtu_in = 1454;
    result.mtu_out = 1254;
    result.port = 2053;
    result.selected_port = 1234;
    result.public_ip = "203.0.113.7";
    result.max_bandwidth = 20000;
    result.measured = true;
    return result;
  }

  static auto second() -> gk::Ping {
    auto result = row();
    result.name = "jp-east";
    result.rtt_ms = 999;
    result.rtts_ms = {999};
    result.mtu_in = 0;
    result.mtu_out = 0;
    result.port = 0;
    result.public_ip = "192.0.2.9";
    result.max_bandwidth = 0;
    result.measured = false;
    return result;
  }
};

TEST_F(GkPingResultsJson, SerializesEveryRowInOrder) {
  const auto json = gk::Ping::serialize({row(), second()});
  ASSERT_TRUE(json);
  EXPECT_EQ(
      *json,
      "[{\"dataCenter\":\"us-west\",\"rtt\":21,\"rtts\":[22,20,21],"
      "\"mtu_in\":1454,\"mtu_out\":1254,\"port\":2053,"
      "\"publicIp\":\"203.0.113.7\",\"maxBandwidth\":20000,\"measured\":true},"
      "{\"dataCenter\":\"jp-east\",\"rtt\":999,\"rtts\":[999],"
      "\"mtu_in\":0,\"mtu_out\":0,\"port\":0,"
      "\"publicIp\":\"192.0.2.9\",\"maxBandwidth\":0,\"measured\":false}]");
  EXPECT_EQ(json->find("selected_port"), std::string::npos);
  EXPECT_EQ(json->find("1234"), std::string::npos);
}

TEST_F(GkPingResultsJson, EscapesTheDatacenterName) {
  auto escaped = row();
  escaped.name = "a\"b/c\\d\ne\tf";
  const auto json = gk::Ping::serialize({escaped});
  ASSERT_TRUE(json);
  EXPECT_NE(json->find("\"dataCenter\":\"a\\\"b/c\\\\d\\ne\\tf\""),
            std::string::npos);
}

TEST_F(GkPingResultsJson, EscapesControlCharactersAndWritesEmptyRtts) {
  auto escaped = row();
  escaped.name = std::string{"a\x01", 2};
  escaped.rtts_ms.clear();
  const auto json = gk::Ping::serialize({escaped});
  ASSERT_TRUE(json);
  EXPECT_NE(json->find("\"dataCenter\":\"a\\u0001\""), std::string::npos);
  EXPECT_NE(json->find("\"rtts\":[]"), std::string::npos);
}

TEST_F(GkPingResultsJson, RefusesAnEmbeddedNul) {
  auto named = row();
  named.name = std::string{"a\0b", 3};
  EXPECT_FALSE(gk::Ping::serialize({named}));
  auto addressed = row();
  addressed.public_ip = std::string{"1\0.2", 4};
  EXPECT_FALSE(gk::Ping::serialize({addressed}));
}

TEST_F(GkPingResultsJson, RefusesNoRowsOrTooMany) {
  EXPECT_FALSE(gk::Ping::serialize({}));
  const std::vector<gk::Ping> many(200, row());
  EXPECT_FALSE(gk::Ping::serialize(many));
}

TEST_F(GkPingResultsJson, FitsTwelveKilobytesUnlessGivenMore) {
  auto boundary = row();
  boundary.name = "x";
  const auto base = gk::Ping::serialize({boundary});
  ASSERT_TRUE(base);
  boundary.name.assign(12 * 1024 - (base->size() - 1), 'x');
  const auto json = gk::Ping::serialize({boundary});
  ASSERT_TRUE(json);
  EXPECT_EQ(json->size(), std::size_t{12 * 1024});
  boundary.name.push_back('x');
  EXPECT_FALSE(gk::Ping::serialize({boundary}));
  EXPECT_TRUE(gk::Ping::serialize({boundary}, 64 * 1024));
}

TEST_F(GkPingResultsJson, ParsesBackWhatItSerialized) {
  const auto stored = gk::Ping::serialize({row(), second()});
  ASSERT_TRUE(stored);
  const auto decoded = gk::Ping::parse(*stored);
  ASSERT_EQ(decoded.size(), std::size_t{2});
  EXPECT_EQ(decoded[0].name, "us-west");
  EXPECT_EQ(decoded[0].rtt_ms, 21);
  EXPECT_EQ(decoded[0].rtts_ms, std::vector<int>({22, 20, 21}));
  EXPECT_EQ(decoded[0].mtu_in, 1454);
  EXPECT_EQ(decoded[0].mtu_out, 1254);
  EXPECT_EQ(decoded[0].port, 2053);
  EXPECT_EQ(decoded[0].selected_port, 0);
  EXPECT_EQ(decoded[0].public_ip, "203.0.113.7");
  EXPECT_EQ(decoded[0].max_bandwidth, 20000);
  EXPECT_TRUE(decoded[0].measured);
  EXPECT_EQ(decoded[1].name, "jp-east");
  EXPECT_EQ(decoded[1].rtt_ms, 999);
  EXPECT_FALSE(decoded[1].measured);
  EXPECT_EQ(decoded[1].public_ip, "192.0.2.9");
  EXPECT_EQ(gk::Ping::serialize(decoded), stored);
}

TEST_F(GkPingResultsJson, ParsesRowsWithMissingFields) {
  const auto partial =
      gk::Ping::parse(R"([{"dataCenter":"eu","rtt":30},{"rtt":5}])");
  ASSERT_EQ(partial.size(), std::size_t{2});
  EXPECT_EQ(partial[0].name, "eu");
  EXPECT_EQ(partial[0].rtt_ms, 30);
  EXPECT_FALSE(partial[0].measured);
  EXPECT_TRUE(partial[1].name.empty());
  EXPECT_EQ(partial[1].rtt_ms, 5);
}

TEST_F(GkPingResultsJson, ParsesNothingFromMalformedInput) {
  EXPECT_TRUE(
      gk::Ping::parse(
          R"([{"dataCenter":"eu","rtt":"30","measured":1},7,{"rtt":5}])")
          .empty());
  EXPECT_TRUE(gk::Ping::parse("").empty());
  EXPECT_TRUE(gk::Ping::parse("not json").empty());
  EXPECT_TRUE(gk::Ping::parse(R"({"dataCenter":"eu"})").empty());
}

class GkReportedRoundTrip : public ::testing::Test {
 protected:
  static auto measured() -> gk::Ping {
    gk::Ping result;
    result.name = "lona";
    result.rtt_ms = 15;
    result.rtts_ms = {15, 25, 21};
    result.measured = true;
    return result;
  }

  static auto unmeasured() -> gk::Ping {
    gk::Ping result;
    result.rtt_ms = gk::Ping::unmeasured_rtt_ms;
    result.rtts_ms = {gk::Ping::unmeasured_rtt_ms};
    return result;
  }
};

TEST_F(GkReportedRoundTrip, SubtractsTheAllowanceFromANearRoundTrip) {
  const auto near = measured().reported();
  EXPECT_EQ(near.rtt_ms, 1);
  EXPECT_EQ(near.rtts_ms, std::vector<int>({1, 5, 1}));
  EXPECT_EQ(near.name, "lona");
  EXPECT_TRUE(near.measured);
}

TEST_F(GkReportedRoundTrip, SubtractsTheAllowanceFromAFarRoundTrip) {
  auto far = measured();
  far.rtt_ms = 48;
  far.rtts_ms = {48};
  EXPECT_EQ(far.reported().rtt_ms, 28);
}

TEST_F(GkReportedRoundTrip, LeavesAnUnmeasuredOrSilentRowAlone) {
  EXPECT_EQ(unmeasured().reported(), unmeasured());
  gk::Ping silent;
  silent.measured = true;
  EXPECT_EQ(silent.reported(), silent);
}

TEST_F(GkReportedRoundTrip, ReportsEveryRow) {
  auto far = measured();
  far.rtt_ms = 48;
  far.rtts_ms = {48};
  const auto rows = gk::Ping::reported({far, unmeasured()});
  ASSERT_EQ(rows.size(), std::size_t{2});
  EXPECT_EQ(rows[0].rtt_ms, 28);
  EXPECT_EQ(rows[1].rtt_ms, gk::Ping::unmeasured_rtt_ms);
}

}  // namespace
}  // namespace cloud_test
