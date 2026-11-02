#include "src/core/net/nat/holepunch/holepunch_codec.hpp"
#include "src/core/psn/remote_play/session_client.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/json/tree.hpp"
#include "tests/support/psn/fakes.hpp"
#include "tests/support/random.hpp"

#include <libchiaking/platform/runtime/clock_provider.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace psn_test {
namespace {

using libchiaking::net::nat::holepunch::Action;
using libchiaking::net::nat::holepunch::CandidateType;
using libchiaking::net::nat::holepunch::ConnRequest;
using libchiaking::net::nat::holepunch::MessageCodec;
using libchiaking::psn::remote_play::SessionClient;
using libchiaking::psn::remote_play::SessionErrorKind;
using libchiaking::psn::rest::ConsoleFamily;
using test_support::JsonTree;
using test_support::psn::Reply;

class SessionClientMessage : public ::testing::Test {
 protected:
  static inline const libchiaking::psn::auth::BearerToken token{
      std::string_view{"SYNTHETIC_TEST_TOKEN"}};
  static constexpr std::string_view session_id =
      "01234567-89ab-cdef-0123-456789abcdef";
  static constexpr std::string_view payload_prefix =
      "ver=1.0, type=text, body=";

  SessionClientMessage() {
    http.on("POST",
            "https://web.np.playstation.com/api/sessionManager/v1/"
            "remotePlaySessions/" +
                std::string{session_id} + "/sessionMessage",
            {Reply{204, ""}});
    for (std::size_t index = 0; index < sequence_uid.size(); ++index)
      sequence_uid[index] = static_cast<std::byte>(index);
    request.sid = 1;
    request.peer_sid = 2;
  }

  void counts_bytes_into_request() {
    for (std::size_t index = 0; index < request.skey.size(); ++index)
      request.skey[index] = static_cast<std::byte>(index);
    for (std::size_t index = 0; index < request.local_hashed_id.size(); ++index)
      request.local_hashed_id[index] = static_cast<std::byte>(index);
  }

  template <std::size_t Size>
  static auto same_bytes(const libchiaking::SecretArray<Size>& actual,
                         const libchiaking::SecretArray<Size>& expected)
      -> testing::AssertionResult {
    for (std::size_t index = 0; index < Size; ++index)
      if (actual[index] != expected[index])
        return testing::AssertionFailure()
               << "byte " << index << " is "
               << std::to_integer<int>(actual[index]) << ", expected "
               << std::to_integer<int>(expected[index]);
    return testing::AssertionSuccess();
  }

  auto last_body() const -> const std::string& { return http.seen.back().body; }

  libchiaking::platform::SteadyClock clock;
  test_support::psn::FakeHttp http;
  test_support::FixedRandom random{std::byte{}};
  SessionClient client{http, clock, random};
  std::array<std::byte, 32> zero_uid{};
  std::array<std::byte, 32> sequence_uid{};
  ConnRequest request{};
};

TEST_F(SessionClientMessage, SendsAShortTerminateExactly) {
  EXPECT_TRUE(client.send_short_message(token, session_id, Action::terminate,
                                        65535, 1, -42, sequence_uid,
                                        ConsoleFamily::ps4, {}));
  ASSERT_EQ(http.seen.size(), 1U);
  EXPECT_EQ(
      last_body(),
      "{\"channel\":\"remote_play:1\",\"payload\":\"ver=1.0, type=text, body="
      "{\\\"action\\\":\\\"TERMINATE\\\",\\\"reqId\\\":65535,\\\"error\\\":1,"
      "\\\"connRequest\\\":{}}\",\"to\":[{\"accountId\":\"-42\","
      "\"deviceUniqueId\":"
      "\"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\","
      "\"platform\":\"PS4\"}]}");
}

TEST_F(SessionClientMessage, SendsAFullOfferExactly) {
  EXPECT_TRUE(client.send_full_message(token, session_id, Action::offer, 7, 0,
                                       request, 42, zero_uid,
                                       ConsoleFamily::ps5, {}));
  ASSERT_EQ(http.seen.size(), 1U);
  EXPECT_EQ(
      last_body(),
      "{\"channel\":\"remote_play:1\",\"payload\":\"ver=1.0, type=text, body="
      "{\\\"action\\\":\\\"OFFER\\\",\\\"reqId\\\":7,\\\"error\\\":0,"
      "\\\"connRequest\\\":{\\\"sid\\\":1,\\\"peerSid\\\":2,"
      "\\\"skey\\\":\\\"AAAAAAAAAAAAAAAAAAAAAA==\\\",\\\"natType\\\":0,"
      "\\\"candidate\\\":[],\\\"defaultRouteMacAddr\\\":\\\"\\\","
      "\\\"localPeerAddr\\\":{\\\"accountId\\\":\\\"42\\\","
      "\\\"platform\\\":\\\"REMOTE_PLAY\\\"},\\\"localHashedId\\\":\\\"\\\"}}"
      "\",\"to\":[{\"accountId\":\"42\","
      "\"deviceUniqueId\":"
      "\"0000000000000000000000000000000000000000000000000000000000000000\","
      "\"platform\":\"PS5\"}]}");
}

TEST_F(SessionClientMessage, CarriesTheKeyAndHashedIdOfAnAccept) {
  counts_bytes_into_request();
  EXPECT_TRUE(client.send_full_message(token, session_id, Action::accept, 65535,
                                       1, request, -42, sequence_uid,
                                       ConsoleFamily::ps4, {}));
  ASSERT_EQ(http.seen.size(), 1U);
  const auto& accept = last_body();
  EXPECT_NE(
      accept.find(
          R"("deviceUniqueId":"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f","platform":"PS4")"),
      std::string::npos)
      << accept;
  const auto parsed = JsonTree::parse(accept);
  ASSERT_TRUE(parsed.has_value());
  const auto* payload = JsonTree::member(parsed.value(), "payload");
  ASSERT_NE(payload, nullptr);
  ASSERT_TRUE(payload->is_string());
  const std::string_view payload_text = JsonTree::text(*payload);
  ASSERT_TRUE(payload_text.starts_with(payload_prefix)) << payload_text;
  const auto body =
      MessageCodec::decode_session(payload_text.substr(payload_prefix.size()));
  ASSERT_TRUE(body.has_value());
  ASSERT_TRUE(body.value().conn_request.has_value());
  const auto& decoded = body.value().conn_request.value();
  EXPECT_TRUE(same_bytes(decoded.skey, request.skey));
  EXPECT_TRUE(same_bytes(decoded.local_hashed_id, request.local_hashed_id));
}

TEST_F(SessionClientMessage, RefusesAResultAsAFullMessageUnsent) {
  const auto invalid_action =
      client.send_full_message(token, session_id, Action::result, 1, 0, request,
                               42, zero_uid, ConsoleFamily::ps5, {});
  ASSERT_FALSE(invalid_action);
  EXPECT_EQ(invalid_action.error().kind, SessionErrorKind::invalid_request);
  EXPECT_TRUE(http.seen.empty());
}

TEST_F(SessionClientMessage, RefusesAnOversizedCandidateUnsent) {
  request.candidates.push_back(
      {CandidateType::local, std::string(46U, 'a'), "", 0, 0});
  const auto invalid_candidate =
      client.send_full_message(token, session_id, Action::offer, 1, 0, request,
                               42, zero_uid, ConsoleFamily::ps5, {});
  ASSERT_FALSE(invalid_candidate);
  EXPECT_EQ(invalid_candidate.error().kind, SessionErrorKind::invalid_request);
  EXPECT_TRUE(http.seen.empty());
}

}  // namespace
}  // namespace psn_test
