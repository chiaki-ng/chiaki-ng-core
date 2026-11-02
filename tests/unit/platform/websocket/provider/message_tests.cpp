#include "tests/unit/platform/websocket/provider/rig.hpp"

#include <gtest/gtest.h>

#include <span>
#include <type_traits>

namespace platform_websocket_test {
namespace {

using namespace libchiaking::platform;

static_assert(!std::is_copy_constructible_v<WebSocketMessage>);
static_assert(!std::is_copy_assignable_v<WebSocketMessage>);
static_assert(std::is_nothrow_move_constructible_v<WebSocketMessage>);

class WebSocketReceive : public WebSocketValuesTest {
 protected:
  [[nodiscard]] static auto message(WebSocketMessageType type,
                                    std::span<const std::byte> payload)
      -> WebSocketMessage {
    WebSocketMessage made;
    made.type = type;
    made.payload = libchiaking::SecretBytes{payload};
    return made;
  }
};

TEST_F(WebSocketReceive, KeepsEmbeddedZerosInABinaryMessage) {
  const auto payload = bytes({0, 0x00, 0xff, 0x00, 0x21});
  auto sent = message(WebSocketMessageType::binary, payload);
  ScriptedConnection connection(sent.clone(), std::nullopt, std::nullopt);
  const auto result = connection.receive(StopToken{});
  sent.payload.clear();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().type, WebSocketMessageType::binary);
  ASSERT_EQ(result.value().payload.size(), 5U);
  EXPECT_EQ(result.value().payload.bytes()[1], std::byte{0});
  EXPECT_EQ(result.value().payload.bytes()[3], std::byte{0});
}

TEST_F(WebSocketReceive, KeepsATextMessageWithAnEmbeddedZero) {
  const auto payload = bytes({0, 0x00, 0x61});
  const auto sent = message(WebSocketMessageType::text, payload);
  ScriptedConnection connection(sent.clone(), std::nullopt, std::nullopt);
  const auto result = connection.receive(StopToken{});
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().type, WebSocketMessageType::text);
  EXPECT_EQ(result.value().payload.view(), sent.payload.view());
}

TEST_F(WebSocketReceive, PassesEveryReceiveErrorThrough) {
  for (const auto error : every_error) {
    ScriptedConnection connection(WebSocketMessage{}, error, std::nullopt);
    const auto result = connection.receive(StopToken{});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), error);
  }
}

}  // namespace
}  // namespace platform_websocket_test
