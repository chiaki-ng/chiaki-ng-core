#include "tests/unit/core/cloud/rig.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace cloud_test {
namespace {

using Reason = gk::Error::Reason;

class GkAllocationReply : public ::testing::Test {
 protected:
  static auto allocate(std::string_view body)
      -> libchiaking::Expected<gk::AllocateState, gk::Error> {
    RecordingHttp http;
    http.reply_body = std::string{body};
    Gk client{http};
    gk::Ping selected;
    selected.name = "dc";
    return client.allocate(GkRig::session_id("synthetic-session"),
                           GkRig::config_key("synthetic-key"), "{}", selected,
                           2053, 10000);
  }

  static auto failure(std::string_view body) -> std::optional<Reason> {
    const auto result = allocate(body);
    if (result) return std::nullopt;
    return result.error().reason();
  }

  static auto progress(std::string_view body)
      -> std::optional<gk::AllocationProgress> {
    const auto result = allocate(body);
    if (!result ||
        !std::holds_alternative<gk::AllocationProgress>(result.value().outcome))
      return std::nullopt;
    return std::get<gk::AllocationProgress>(result.value().outcome);
  }

  static auto success(std::string_view body)
      -> libchiaking::Expected<gk::Allocation, Reason> {
    auto result = allocate(body);
    if (!result) return libchiaking::unexpected(result.error().reason());
    EXPECT_TRUE(std::holds_alternative<gk::Allocation>(result.value().outcome));
    if (!std::holds_alternative<gk::Allocation>(result.value().outcome))
      return libchiaking::unexpected(Reason::malformed);
    return std::move(std::get<gk::Allocation>(result.value().outcome));
  }

  static auto slot(const std::string& private_ip) -> std::string {
    return "{\"launchSlot\":{\"publicIp\":\"1.2.3.4\",\"port\":8299,"
           "\"privateIp\":\"" +
           private_ip +
           "\"},"
           "\"handshakeKey\":\"hk\",\"launchSpecification\":\"spec\","
           "\"sessionId\":\"sid\"}";
  }

  static auto wrapper_of(const std::string& private_ip) -> std::optional<int> {
    const auto result = success(slot(private_ip));
    if (!result) return std::nullopt;
    return static_cast<int>(result.value().psn_wrapper_type);
  }
};

using GkAllocationProgress = GkAllocationReply;
using GkAllocationSuccess = GkAllocationReply;

TEST_F(GkAllocationProgress, ReadsAQueuePosition) {
  const auto result = progress(
      R"({"queued":true,"waitTimeEstimate":120,"pollFrequency":7,"queuePosition":4})");
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->queued);
  EXPECT_FALSE(result->migrating);
  EXPECT_EQ(result->wait_time_estimate, 120);
  EXPECT_EQ(result->poll_frequency, 7);
  EXPECT_EQ(result->queue_position, 4);
}

TEST_F(GkAllocationProgress, ReadsADataMigration) {
  const auto result = progress(
      R"({"dataMigration":true,"dataMigrationPercentageComplete":63})");
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->migrating);
  EXPECT_FALSE(result->queued);
  EXPECT_EQ(result->migration_percentage, 63);
  EXPECT_EQ(result->queue_position, -1);
}

TEST_F(GkAllocationProgress, PrefersTheDisplayedQueuePosition) {
  const auto result = progress(
      R"({"queued":true,"dataMigration":true,"displayQueuePosition":-3,"queuePosition":9})");
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->queued);
  EXPECT_TRUE(result->migrating);
  EXPECT_EQ(result->queue_position, -3);
}

TEST_F(GkAllocationProgress, RefusesADisplayedPositionThatIsNotANumber) {
  EXPECT_EQ(
      failure(R"({"queued":true,"displayQueuePosition":{},"queuePosition":9})"),
      Reason::malformed);
}

TEST_F(GkAllocationProgress, IsMissingASlotWhenNeitherQueuedNorMigrating) {
  EXPECT_EQ(failure("{}"), Reason::missing_launch_slot);
  EXPECT_EQ(failure(R"({"queued":false,"dataMigration":null})"),
            Reason::missing_launch_slot);
}

TEST_F(GkAllocationProgress, RefusesFlagsAndNumbersOfTheWrongType) {
  EXPECT_EQ(failure(R"({"queued":0,"dataMigration":false})"),
            Reason::malformed);
  EXPECT_EQ(failure(R"({"queued":[],"dataMigration":null})"),
            Reason::malformed);
  EXPECT_EQ(failure(R"({"queued":"false"})"), Reason::malformed);
  EXPECT_EQ(failure(R"({"queued":true,"pollFrequency":"15abc"})"),
            Reason::malformed);
}

TEST_F(GkAllocationProgress, ReadsNumbersSentAsText) {
  const auto result = progress(
      R"({"queued":true,"waitTimeEstimate":"120","pollFrequency":"15"})");
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->queued);
  EXPECT_EQ(result->wait_time_estimate, 120);
  EXPECT_EQ(result->poll_frequency, 15);
}

TEST_F(GkAllocationProgress, RefusesAReplyThatIsNotAnObject) {
  EXPECT_EQ(failure("{nope"), Reason::malformed);
  EXPECT_EQ(failure("[]"), Reason::malformed);
}

TEST_F(GkAllocationProgress, StopsReadingAtANul) {
  const std::string with_nul =
      std::string{R"({"queued":true})"} + '\0' + "garbage";
  const auto result = progress(with_nul);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->queued);
  const std::string cut = std::string{"{\"queued\":tru"} + '\0' + "e}";
  EXPECT_EQ(failure(cut), Reason::malformed);
}

TEST_F(GkAllocationSuccess, ReadsTheLaunchSlot) {
  const auto result = success(slot("10.0.9.5"));
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().server_ip, "1.2.3.4");
  EXPECT_EQ(result.value().server_port, 8299);
  EXPECT_EQ(result.value().handshake_key.view(), "hk");
  EXPECT_EQ(result.value().launch_specification.view(), "spec");
  EXPECT_EQ(result.value().session_id.view(), "sid");
  EXPECT_EQ(result.value().psn_wrapper_type, 5);
}

TEST_F(GkAllocationSuccess, TakesTheWrapperTypeFromThePrivateIpsLastOctet) {
  EXPECT_EQ(wrapper_of("1.1.1.0"), 0);
  EXPECT_EQ(wrapper_of("1.1.1.255"), 255);
  EXPECT_EQ(wrapper_of("1.1.1.256"), 1);
  EXPECT_EQ(wrapper_of("1.1.1.-1"), 1);
  EXPECT_EQ(wrapper_of("1.1.1.x7"), 0);
  EXPECT_EQ(wrapper_of("1.1.1.7x"), 7);
  EXPECT_EQ(wrapper_of("1.1.1. 7"), 0);
  EXPECT_EQ(wrapper_of("nodot"), 1);
}

TEST_F(GkAllocationSuccess, RefusesAPortSentAsText) {
  EXPECT_FALSE(
      success(R"({"launchSlot":{"port":"8299","privateIp":"1.2.3.9"}})"));
}

TEST_F(GkAllocationSuccess, LeavesMissingFieldsEmpty) {
  const auto result =
      success(R"({"launchSlot":{"port":8299,"privateIp":"1.2.3.9"}})");
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.value().server_ip.empty());
  EXPECT_EQ(result.value().server_port, 8299);
  EXPECT_TRUE(result.value().handshake_key.empty());
  EXPECT_TRUE(result.value().launch_specification.empty());
  EXPECT_TRUE(result.value().session_id.empty());
  EXPECT_EQ(result.value().psn_wrapper_type, 9);
}

TEST_F(GkAllocationSuccess, KeepsEscapedNulsInStrings) {
  const auto result = success(
      R"({"launchSlot":{"publicIp":"1.2.3.4\u0000tail"},"handshakeKey":"a\u0000b","launchSpecification":"c\u0000d","sessionId":"e\u0000f"})");
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().server_ip, std::string_view("1.2.3.4\0tail", 12));
  EXPECT_EQ(result.value().handshake_key.view(), std::string_view("a\0b", 3));
  EXPECT_EQ(result.value().launch_specification.view(),
            std::string_view("c\0d", 3));
  EXPECT_EQ(result.value().session_id.view(), std::string_view("e\0f", 3));
}

TEST_F(GkAllocationSuccess, StopsReadingAtARawNul) {
  const std::string with_nul = slot("1.2.3.4") + '\0' + "garbage";
  const auto result = success(with_nul);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().psn_wrapper_type, 4);
}

TEST_F(GkAllocationSuccess, RefusesAReplyThatIsNotAnObject) {
  const auto cut = success("{");
  ASSERT_FALSE(cut);
  EXPECT_EQ(cut.error(), Reason::malformed);
  const auto list = success("[]");
  ASSERT_FALSE(list);
  EXPECT_EQ(list.error(), Reason::malformed);
}

TEST_F(GkAllocationSuccess, IsMissingASlotWithoutOne) {
  const auto absent = success(R"({"handshakeKey":"x"})");
  ASSERT_FALSE(absent);
  EXPECT_EQ(absent.error(), Reason::missing_launch_slot);
  const auto null = success(R"({"launchSlot":null})");
  ASSERT_FALSE(null);
  EXPECT_EQ(null.error(), Reason::missing_launch_slot);
}

TEST_F(GkAllocationSuccess, RefusesASlotThatIsNotAnObject) {
  const auto result = success(R"({"launchSlot":"bad"})");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), Reason::malformed);
}

TEST_F(GkAllocationSuccess, AcceptsEveryPortNumber) {
  const auto highest =
      success(R"({"launchSlot":{"publicIp":"1.2.3.4","port":65535}})");
  ASSERT_TRUE(highest);
  EXPECT_EQ(highest.value().server_port, 65535);
  const auto zero =
      success(R"({"launchSlot":{"publicIp":"1.2.3.4","port":0}})");
  ASSERT_TRUE(zero);
  EXPECT_EQ(zero.value().server_port, 0);
}

TEST_F(GkAllocationSuccess, RefusesAPortOutOfRange) {
  const auto above =
      success(R"({"launchSlot":{"publicIp":"1.2.3.4","port":65536}})");
  ASSERT_FALSE(above);
  EXPECT_EQ(above.error(), Reason::malformed);
  const auto far =
      success(R"({"launchSlot":{"publicIp":"1.2.3.4","port":74835}})");
  ASSERT_FALSE(far);
  EXPECT_EQ(far.error(), Reason::malformed);
  const auto negative =
      success(R"({"launchSlot":{"publicIp":"1.2.3.4","port":-1}})");
  ASSERT_FALSE(negative);
  EXPECT_EQ(negative.error(), Reason::malformed);
}

}  // namespace
}  // namespace cloud_test
