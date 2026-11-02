#pragma once

#include <string>
#include <string_view>

namespace psn_test {

class SignallingBodies {
 public:
  static constexpr std::string_view route_offer =
      R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"OFFER\",\"reqId\":2,\"error\":0,\"connRequest\":{\"sid\":3,\"peerSid\":4,\"skey\":\"AAAAAAAAAAAAAAAAAAAAAA==\",\"natType\":2,\"candidate\":[{\"type\":\"LOCAL\",\"addr\":\"127.0.0.1\",\"mappedAddr\":\"0.0.0.0\",\"port\":9999,\"mappedPort\":0}],\"defaultRouteMacAddr\":\"\",\"localPeerAddr\":,\"localHashedId\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAA=\"}}"}}}})";
  static constexpr std::string_view route_result =
      R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"RESULT\",\"reqId\":1,\"error\":0,\"connRequest\":{}}"}}}})";
  static constexpr std::string_view route_accept =
      R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"ACCEPT\",\"reqId\":2,\"error\":0,\"connRequest\":{}}"}}}})";
  static constexpr std::string_view data_offer =
      R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"OFFER\",\"reqId\":4,\"error\":0,\"connRequest\":{\"sid\":3,\"peerSid\":4,\"skey\":\"AAAAAAAAAAAAAAAAAAAAAA==\",\"natType\":2,\"candidate\":[{\"type\":\"LOCAL\",\"addr\":\"127.0.0.1\",\"mappedAddr\":\"0.0.0.0\",\"port\":9999,\"mappedPort\":0}],\"defaultRouteMacAddr\":\"\",\"localPeerAddr\":,\"localHashedId\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAA=\"}}"}}}})";
  static constexpr std::string_view terminate =
      R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"TERMINATE\",\"reqId\":9,\"error\":0,\"connRequest\":{}}"}}}})";
  static constexpr std::string_view undecodable =
      R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"body=not-json"}}}})";
  static constexpr std::string_view session_deleted =
      R"({"dataType":"psn:sessionManager:sys:remotePlaySession:deleted","body":{"data":{"sessionId":"01234567-89ab-cdef-0123-456789abcdef"}}})";
  static constexpr std::string_view other_session_deleted =
      R"({"dataType":"psn:sessionManager:sys:remotePlaySession:deleted","body":{"data":{"sessionId":"unrelated"}}})";
  static constexpr std::string_view data_offer_id = R"(\"reqId\":4)";

  static auto route_offer_numbered(std::string_view request_id) -> std::string {
    std::string offer{route_offer};
    constexpr std::string_view original = R"(\"reqId\":2)";
    offer.replace(offer.find(original), original.size(),
                  R"(\"reqId\":)" + std::string{request_id});
    return offer;
  }
};

}  // namespace psn_test
