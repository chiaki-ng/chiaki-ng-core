#include "tests/support/enum_printing.hpp"
#include "tests/support/users/signed_in.hpp"
#include "tests/support/websocket.hpp"
#include "tests/unit/core/play/adapters/links/internet/internet_link_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace play_test {

using test_support::play::SignedInPlayer;
namespace {

using internet_links::RecordingSink;
using internet_links::RefusedAccess;
using internet_links::ScriptedRemote;
using internet_links::Target;
namespace platform = libchiaking::platform;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace script = remote_start_script;

class InternetLinkAccess : public ::testing::Test {
 protected:
  auto without_access(std::shared_ptr<libchiaking::users::PsUser> user)
      -> std::optional<play::LinkFailure> {
    play::InternetLinkConfiguration config;
    config.user = std::move(user);
    config.host = "127.0.0.1";
    config.console.target = Target::ps5_1;
    RecordingSink sink;
    auto link =
        play::InternetLinks::playing(std::move(config), factory_, nullptr);
    EXPECT_TRUE(link);
    if (!link) return std::nullopt;
    link.value()->set_event_sink(sink);
    const auto started = link.value()->start();
    EXPECT_FALSE(started);
    if (!started) {
      EXPECT_EQ(started.error(), remote_play::StreamError::route_failed);
    }
    EXPECT_EQ(link.value()->state(), remote_play::StreamState::failed);
    const auto& progress = sink.heard().setup_progress;
    EXPECT_EQ(progress.size(), 1U);
    if (!progress.empty()) {
      EXPECT_EQ(progress.front().stage, remote_play::FailureStage::psn_open);
    }
    const auto& failure = link.value()->failure();
    EXPECT_TRUE(failure);
    if (!failure) return std::nullopt;
    EXPECT_EQ(failure->stage(), remote_play::FailureStage::psn_open);
    return *failure;
  }

  static auto unleased(const play::LinkFailure& failure) -> bool {
    const auto* psn = failure.as<play::PsnFailure>();
    return psn && psn->lacks_access();
  }

  auto calls() const -> unsigned { return http_.calls(); }

 private:
  script::RejectCrypto crypto_;
  RefusedAccess http_;
  test_support::RefusingWebSocket websocket_{
      platform::WebSocketError::backend_failure};
  platform::DefaultPlatformFactory factory_{platform::PlatformProviders{
      .crypto = &crypto_, .http = &http_, .websocket = &websocket_}};
};

TEST_F(InternetLinkAccess, LacksAccessForAHomeUserWithoutAskingPsn) {
  test_support::users::SignedIn home;
  const auto added = home.users.add_home_user(libchiaking::AccountId{40},
                                              "Living Room Player");
  ASSERT_TRUE(added);
  const auto failure = without_access(added.value());
  ASSERT_TRUE(failure);
  EXPECT_TRUE(unleased(*failure));
  EXPECT_EQ(calls(), 0U);
}

TEST_F(InternetLinkAccess, AsksPsnOnceAndThenLacksTheRefusedAccess) {
  const auto player = SignedInPlayer::fresh_for_remote_play();
  const auto refused = without_access(player);
  ASSERT_TRUE(refused);
  EXPECT_FALSE(unleased(*refused));
  EXPECT_EQ(refused->http_status(), 401);
  EXPECT_EQ(calls(), 1U);
  const auto again = without_access(player);
  ASSERT_TRUE(again);
  EXPECT_TRUE(unleased(*again));
  EXPECT_EQ(calls(), 1U);
}

TEST_F(InternetLinkAccess, LacksAccessForAUserNotSignedInForRemotePlay) {
  const auto failure = without_access(SignedInPlayer::fresh());
  ASSERT_TRUE(failure);
  EXPECT_TRUE(unleased(*failure));
  EXPECT_EQ(calls(), 0U);
}

class InternetLinkDeviceId : public internet_links::ScriptedInternetLinkTest {
 protected:
  static auto did_header(std::array<std::byte, 32> did) -> std::string {
    ScriptedRemote<> rig{{script::created_body, script::members_body,
                          script::ready_members_body, script::ready_custom_body,
                          script::route_offer_body, script::route_result_body,
                          script::route_accept_body}};
    rig.http().start_status = 200;
    rig.crypto().allow_registration_request = true;
    rig.answer_control_route();
    auto config = ScriptedRemote<>::configuration();
    config.enable_ipv6 = false;
    config.did = did;
    auto* link = rig.open(std::move(config));
    EXPECT_NE(link, nullptr);
    if (!link) return {};
    EXPECT_TRUE(link->start());
    const auto* socket = rig.sockets().last;
    EXPECT_NE(socket, nullptr);
    if (!socket) return {};
    const auto& request = socket->ctrl_request;
    const auto begin = request.find("RP-Did: ");
    EXPECT_NE(begin, std::string::npos);
    if (begin == std::string::npos) return {};
    const auto end = request.find("\r\n", begin);
    auto header = request.substr(begin, end - begin);
    EXPECT_TRUE(link->stop());
    return header;
  }

  static auto reference_did() -> std::array<std::byte, 32> {
    std::array<std::byte, 32> reference{};
    constexpr std::array<std::uint8_t, 10> prefix{0x00, 0x18, 0x00, 0x00, 0x00,
                                                  0x07, 0x00, 0x40, 0x00, 0x80};
    std::ranges::transform(prefix, reference.begin(), [](std::uint8_t value) {
      return static_cast<std::byte>(value);
    });
    return reference;
  }
};

TEST_F(InternetLinkDeviceId, ReplacesAZeroDeviceIdWithTheReferenceLayout) {
  const auto generated = did_header({});
  EXPECT_FALSE(generated.empty());
  EXPECT_EQ(did_header(reference_did()), generated);
}

TEST_F(InternetLinkDeviceId, SendsACustomDeviceIdAsGiven) {
  std::array<std::byte, 32> custom{};
  custom.fill(std::byte{0x5a});
  EXPECT_NE(did_header(custom), did_header({}));
}

}  // namespace
}  // namespace play_test
