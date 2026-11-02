#include "src/core/net/kakusan/controller_frame.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "tests/unit/core/net/kakusan/control_message_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>

namespace net_kakusan_test {
namespace {

using namespace libchiaking::net::kakusan;

class ControlMessageCodec : public ControlMessageTest {
 protected:
  static auto grant(std::string_view fields) {
    return Codec::decode("{\"type\":\"SEAT_GRANTED\"," + std::string{fields} +
                         "}");
  }
};

TEST_F(ControlMessageCodec, RoundTripsARevokedSeat) {
  const auto revoked = round_trip(control::SeatRevoked{
      SeatBinding{2, 4294967295U}, control::Reason::capacity});
  ASSERT_TRUE(revoked);
  EXPECT_EQ(std::get<control::SeatRevoked>(*revoked).binding.generation,
            4294967295U);
  EXPECT_EQ(std::get<control::SeatRevoked>(*revoked).reason,
            control::Reason::capacity);
}

TEST_F(ControlMessageCodec, RoundTripsEveryReasonToEndOrLeave) {
  for (const auto reason :
       {control::Reason::user, control::Reason::capacity,
        control::Reason::source, control::Reason::timeout,
        control::Reason::network, control::Reason::protocol}) {
    SCOPED_TRACE(::testing::PrintToString(reason));
    const auto end = round_trip(control::End{reason});
    ASSERT_TRUE(end);
    EXPECT_EQ(std::get<control::End>(*end).reason, reason);
    const auto leave = round_trip(control::Leave{reason});
    ASSERT_TRUE(leave);
    EXPECT_EQ(std::get<control::Leave>(*leave).reason, reason);
  }
}

TEST_F(ControlMessageCodec, RoundTripsPingPongAndTheEmptyMessages) {
  control::Ping ping;
  ping.nonce[0] = std::byte{0xab};
  const auto pinged = round_trip(ping);
  ASSERT_TRUE(pinged);
  EXPECT_EQ(std::get<control::Ping>(*pinged).nonce, ping.nonce);
  control::Pong pong{ping.nonce};
  const auto ponged = round_trip(pong);
  ASSERT_TRUE(ponged);
  EXPECT_EQ(std::get<control::Pong>(*ponged).nonce, ping.nonce);
  EXPECT_TRUE(round_trip(control::AdmissionPending{}));
  EXPECT_TRUE(round_trip(control::SourceIdr{}));
}

TEST_F(ControlMessageCodec, RoundTripsEveryWireFailureCode) {
  for (const auto code :
       {Error::invalid_block, Error::expired, Error::version,
        Error::identity_changed, Error::transcript, Error::pairing_failed,
        Error::no_path, Error::no_capacity, Error::source_join_failed,
        Error::codec, Error::timeout, Error::protocol, Error::source_format,
        Error::congestion, Error::source_ended}) {
    SCOPED_TRACE(::testing::PrintToString(code));
    const auto failure = round_trip(control::Failure{code});
    ASSERT_TRUE(failure);
    EXPECT_EQ(std::get<control::Failure>(*failure).code, code);
  }
}

TEST_F(ControlMessageCodec, RefusesToSendLocalFailureCodes) {
  EXPECT_FALSE(Codec::encode(control::Failure{Error::internal}));
  EXPECT_FALSE(Codec::encode(control::Failure{Error::crypto_unavailable}));
}

TEST_F(ControlMessageCodec, WritesCanonicalJson) {
  const auto end = Codec::encode(control::End{control::Reason::user});
  ASSERT_TRUE(end);
  EXPECT_EQ(end.value(), "{\"reason\":\"user\",\"type\":\"END\"}");
  const auto failure = Codec::encode(control::Failure{Error::no_capacity});
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure.value(), "{\"code\":\"NO_CAPACITY\",\"type\":\"ERROR\"}");
  const auto pending = Codec::encode(control::AdmissionPending{});
  ASSERT_TRUE(pending);
  EXPECT_EQ(pending.value(),
            "{\"timeout_ms\":60000,\"type\":\"ADMISSION_PENDING\"}");
}

TEST_F(ControlMessageCodec, AcceptsMembersInAnyOrderAndSpacing) {
  EXPECT_TRUE(
      Codec::decode("{\"type\":\"READY\",\"seat\":3,\"generation\":7}"));
  EXPECT_TRUE(Codec::decode(
      " { \"generation\" : 7 , \"seat\" : 3, \"type\":\"READY\" } "));
  EXPECT_TRUE(Codec::decode("{\"type\":\"PING\",\"nonce\":\"AAAAAAAAAAA\"}"));
}

TEST_F(ControlMessageCodec, RejectsAnythingOutsideTheStrictSchema) {
  EXPECT_TRUE(rejects(""));
  EXPECT_TRUE(rejects("[]"));
  EXPECT_TRUE(rejects("{}"));
  EXPECT_TRUE(rejects("{\"type\":\"READY\"}"));
  EXPECT_TRUE(rejects("{\"type\":\"READY\",\"seat\":3}"));
  EXPECT_TRUE(
      rejects("{\"type\":\"READY\",\"seat\":3,\"generation\":7,\"extra\":1}"));
  EXPECT_TRUE(
      rejects("{\"type\":\"READY\",\"seat\":3,\"generation\":7,\"seat\":3}"));
  EXPECT_TRUE(rejects("{\"type\":\"READY\",\"seat\":0,\"generation\":7}"));
  EXPECT_TRUE(rejects("{\"type\":\"READY\",\"seat\":256,\"generation\":7}"));
  EXPECT_TRUE(rejects("{\"type\":\"READY\",\"seat\":3,\"generation\":0}"));
  EXPECT_TRUE(
      rejects("{\"type\":\"READY\",\"seat\":3,\"generation\":4294967296}"));
  EXPECT_TRUE(rejects("{\"type\":\"READY\",\"seat\":\"3\",\"generation\":7}"));
  EXPECT_TRUE(rejects("{\"type\":\"ready\",\"seat\":3,\"generation\":7}"));
  EXPECT_TRUE(rejects("{\"type\":\"RESUME\"}"));
  EXPECT_TRUE(rejects("{\"type\":7}"));
  EXPECT_TRUE(rejects("{\"type\":\"ADMISSION_PENDING\",\"timeout_ms\":60001}"));
  EXPECT_TRUE(rejects("{\"type\":\"SOURCE_IDR\",\"epoch\":2}"));
  EXPECT_TRUE(rejects("{\"type\":\"LEAVE\",\"reason\":\"bored\"}"));
  EXPECT_TRUE(rejects("{\"type\":\"ERROR\",\"code\":\"UNKNOWN\"}"));
  EXPECT_TRUE(rejects("{\"type\":\"ERROR\",\"code\":\"protocol\"}"));
  EXPECT_TRUE(rejects("{\"type\":\"PING\",\"nonce\":\"AAAAAAAAAAA=\"}"));
  EXPECT_TRUE(rejects("{\"type\":\"PING\",\"nonce\":\"AAAAAAAAAA\"}"));
  EXPECT_TRUE(rejects("{\"type\":\"PING\",\"nonce\":\"AAAAAAAAAAB\"}"));
  EXPECT_TRUE(rejects("{\"type\":\"HELLO\",\"transcript\":\"AAAA\"}"));
}

TEST_F(ControlMessageCodec, RejectsAMessageAboveTheSizeLimit) {
  const std::string padding(max_control_message_size, ' ');
  EXPECT_TRUE(rejects(padding + "{\"type\":\"SOURCE_IDR\",\"epoch\":1}"));
}

TEST_F(ControlMessageCodec, AcceptsASeatGrantOnlyWithinItsRules) {
  EXPECT_TRUE(
      grant("\"seat\":3,\"generation\":7,\"capacity\":4,\"local_seats\":2,"
            "\"input_hz\":60,\"allowed_buttons\":262143"));
  EXPECT_TRUE(
      grant("\"seat\":3,\"generation\":7,\"capacity\":4,\"local_seats\":2,"
            "\"input_hz\":60,\"allowed_buttons\":237567"));
  EXPECT_FALSE(
      grant("\"seat\":3,\"generation\":7,\"capacity\":4,\"local_seats\":2,"
            "\"input_hz\":60,\"allowed_buttons\":262142"));
  EXPECT_FALSE(
      grant("\"seat\":3,\"generation\":7,\"capacity\":4,\"local_seats\":2,"
            "\"input_hz\":60,\"allowed_buttons\":262144"));
  EXPECT_FALSE(
      grant("\"seat\":3,\"generation\":7,\"capacity\":4,\"local_seats\":2,"
            "\"input_hz\":120,\"allowed_buttons\":262143"));
  EXPECT_FALSE(
      grant("\"seat\":5,\"generation\":7,\"capacity\":4,\"local_seats\":2,"
            "\"input_hz\":60,\"allowed_buttons\":262143"));
  EXPECT_FALSE(
      grant("\"seat\":3,\"generation\":7,\"capacity\":4,\"local_seats\":5,"
            "\"input_hz\":60,\"allowed_buttons\":262143"));
  EXPECT_FALSE(
      grant("\"seat\":3,\"generation\":7,\"capacity\":4,\"local_seats\":0,"
            "\"input_hz\":60,\"allowed_buttons\":262143"));
  EXPECT_FALSE(
      grant("\"seat\":3,\"generation\":7,\"capacity\":0,\"local_seats\":0,"
            "\"input_hz\":60,\"allowed_buttons\":262143"));
}

TEST_F(ControlMessageCodec, RefusesToGrantFewerThanTheRequiredButtons) {
  control::SeatGranted invalid{SeatBinding{3, 7}, 4, 2, required_buttons >> 1U};
  EXPECT_FALSE(Codec::encode(invalid));
}

}  // namespace
}  // namespace net_kakusan_test
