#pragma once

#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/platform/net/default.hpp"
#include "src/core/play/adapters/links/cloud/cloud_link.hpp"
#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
#include "tests/unit/core/play/takion_defaults.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace play_test::cloud_loopback {

namespace remote_play = libchiaking::net::protocol::remote_play;

class HandshakePair final : public libchiaking::platform::EcdhKeyPair {
 public:
  auto public_coordinates(std::span<std::byte> output) const
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    EXPECT_TRUE(output.size() == 64 || output.size() == 132) << output.size();
    for (std::size_t index = 0; index < output.size(); ++index)
      output[index] = static_cast<std::byte>(index);
    return {};
  }
  auto derive_secret(std::span<const std::byte>, std::span<std::byte>) const
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }
};

class HandshakeCrypto final : public libchiaking::platform::CryptoProvider {
 public:
  using Result =
      libchiaking::Expected<void, libchiaking::platform::CryptoError>;

  auto generate_ecdh(libchiaking::platform::EcdhCurve curve)
      -> libchiaking::Expected<
          std::unique_ptr<libchiaking::platform::EcdhKeyPair>,
          libchiaking::platform::CryptoError> override {
    EXPECT_TRUE(curve == libchiaking::platform::EcdhCurve::secp256k1 ||
                curve == libchiaking::platform::EcdhCurve::secp521r1);
    return std::unique_ptr<libchiaking::platform::EcdhKeyPair>{
        new HandshakePair};
  }
  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> Result override {
    return refused();
  }
  auto hmac_sha256(std::span<const std::byte> key,
                   std::span<const std::byte> input,
                   std::span<std::byte> output) -> Result override {
    EXPECT_EQ(key.size(), 16U);
    EXPECT_TRUE(input.size() == 65 || input.size() == 133) << input.size();
    EXPECT_EQ(output.size(), 32U);
    std::ranges::fill(output, std::byte{0x5a});
    return {};
  }
  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>, std::span<std::byte>)
      -> Result override {
    return refused();
  }
  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> Result override {
    return refused();
  }

 private:
  static auto refused() -> Result {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }
};

class GkScript final : public libchiaking::platform::HttpProvider {
 public:
  auto perform(const libchiaking::platform::HttpRequest& request,
               const libchiaking::platform::StopToken&)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    libchiaking::platform::HttpResponse response;
    response.status_code = 200;
    std::string body;
    auto step = calls++;
    const auto url = request.url.view();
    const auto agent = std::ranges::find_if(
        request.headers,
        [](const auto& header) { return header.name == "User-Agent"; });
    EXPECT_NE(agent, request.headers.end());
    if (agent != request.headers.end()) {
      EXPECT_NE(
          agent->value.view().find(legacy_cloud_stream ? "playstation-now/"
                                                       : "PlayStation Portal/"),
          std::string::npos);
    }
    if (legacy_cloud_stream && step == 1) {
      EXPECT_NE(url.find("scope=kamaji:commerce_native%20kamaji:"
                         "commerce_container"),
                std::string::npos);
      response.status_code = 302;
      response.headers.push_back(
          {"Location", libchiaking::SecretString{
                           "https://psnow.playstation.com/app/2.2.0/133/"
                           "5cdcc037d/grc-response.html?code=ACCOUNT"}});
      return response;
    }
    if (legacy_cloud_stream && step == 2) {
      EXPECT_TRUE(url.ends_with("/user/session"));
      const std::string_view posted{request.body.view()};
      EXPECT_TRUE(posted.starts_with(
          "code=ACCOUNT&client_id=bc6b0777-abb5-40da-92ca-e133cf18e989&duid="));
      for (const char byte :
           std::string_view{R"({"header":{"status_code":"0x0000"}})"})
        response.body.push_back(static_cast<std::byte>(byte));
      return response;
    }
    if (legacy_cloud_stream && step >= 3) step -= 2;
    switch (step) {
      case 0:
        EXPECT_NE(url.find("authorizeCheck"), std::string::npos);
        response.status_code = 204;
        break;
      case 1:
        EXPECT_NE(url.find("/client_ids"), std::string::npos);
        body =
            R"({"gkClientId":"gk-id","ps3GkClientId":"ps3-id","streamServerClientId":"server-id"})";
        break;
      case 2:
        EXPECT_TRUE(url.ends_with("/config"));
        body = R"({"configKey":"config-key"})";
        break;
      case 3:
        EXPECT_NE(url.find("/sessions/start"), std::string::npos);
        body = R"({"sessionId":"synthetic-session"})";
        response.headers.push_back(
            {"X-Gaikai-Session", libchiaking::SecretString{"start-key"}});
        break;
      case 4:
      case 5:
        EXPECT_EQ(request.method, libchiaking::platform::HttpMethod::get);
        response.status_code = 302;
        response.headers.push_back(
            {"Location",
             libchiaking::SecretString{
                 step == 4 ? (request.url.view().find("grc-response") !=
                                      std::string_view::npos
                                  ? std::string{"https://psnow.playstation.com/"
                                                "app/2.2.0/133/5cdcc037d/"
                                                "grc-response.html"}
                                  : std::string{"gaikai://local"}) +
                                 "?code=CLIENT"
                           : (request.url.view().find("grc-response") !=
                                      std::string_view::npos
                                  ? std::string{"https://psnow.playstation.com/"
                                                "app/2.2.0/133/5cdcc037d/"
                                                "grc-response.html"}
                                  : std::string{"gaikai://local"}) +
                                 "?code=SERVER"}});
        break;
      case 6:
        EXPECT_NE(url.find("/authorize"), std::string::npos);
        EXPECT_EQ(session_header(request), "start-key");
        response.headers.push_back(
            {"X-Gaikai-Session", libchiaking::SecretString{"authorize-key"}});
        break;
      case 7:
        EXPECT_NE(url.find("/lock?forceLogout=true"), std::string::npos);
        EXPECT_EQ(session_header(request), "authorize-key");
        body = R"({"lockAcquired":true,"pollFrequency":5})";
        break;
      case 8:
        EXPECT_TRUE(url.ends_with("/datacenters"));
        EXPECT_EQ(session_header(request), "authorize-key");
        body =
            R"([{"dataCenter":"dc1","publicIp":"192.0.2.1","port":2053,"maxBandwidth":20000}])";
        response.headers.push_back(
            {"X-Gaikai-Session", libchiaking::SecretString{"datacenter-key"}});
        break;
      case 9:
        EXPECT_TRUE(url.ends_with("/datacenters/select"));
        EXPECT_EQ(session_header(request), "datacenter-key");
        body = R"({"port":8975})";
        response.headers.push_back(
            {"X-Gaikai-Session", libchiaking::SecretString{"select-key"}});
        break;
      case 10:
        EXPECT_TRUE(url.ends_with("/allocate"));
        EXPECT_EQ(session_header(request), "select-key");
        body =
            R"({"launchSlot":{"publicIp":"127.0.0.1","port":)" +
            std::to_string(allocation_port) +
            R"(,"privateIp":"10.0.0.7"},"handshakeKey":"AAECAwQFBgcICQoLDA0ODw==","launchSpecification":"synthetic-launch","sessionId":"synthetic-session"})";
        response.headers.push_back(
            {"X-Gaikai-Session", libchiaking::SecretString{"allocation-key"}});
        break;
      default:
        ADD_FAILURE() << "unscripted request " << step << ": " << url;
    }
    for (const char character : body)
      response.body.push_back(static_cast<std::byte>(character));
    return response;
  }

  std::uint16_t allocation_port{};
  unsigned calls{};
  bool legacy_cloud_stream{};

 private:
  static auto session_header(const libchiaking::platform::HttpRequest& request)
      -> std::string_view {
    constexpr std::size_t at = 4;
    EXPECT_GT(request.headers.size(), at);
    if (request.headers.size() <= at) return {};
    return request.headers[at].value.view();
  }
};

class LoopbackSink final : public libchiaking::play::LinkEventSink {
 public:
  void on_event(remote_play::StreamEvent event) override {
    if (auto* progress =
            std::get_if<remote_play::event::SetupProgress>(&event)) {
      EXPECT_LE(progress->text.size(), remote_play::setup_progress_text_limit);
      progress_stages.push_back(progress->stage);
      progress_texts.push_back(progress->text);
      return;
    }
    if (auto* rows =
            std::get_if<remote_play::event::CloudDatacenters>(&event)) {
      ++datacenter_events;
      datacenters = rows->datacenters;
      return;
    }
    if (auto* chosen =
            std::get_if<remote_play::event::CloudDatacenterSelected>(&event)) {
      selections.push_back(*chosen);
      return;
    }
    ++events;
    if (auto* changed = std::get_if<remote_play::event::StateChanged>(&event)) {
      last_state = changed->state;
      last_quit = changed->quit_reason;
      last_quit_text = changed->quit_text;
    } else if (std::holds_alternative<remote_play::event::Failed>(event)) {
      ++failures;
    } else {
      ADD_FAILURE() << "an unexpected stream event";
    }
  }

  unsigned events{};
  unsigned failures{};
  remote_play::StreamState last_state{remote_play::StreamState::idle};
  remote_play::QuitReason last_quit{remote_play::QuitReason::none};
  std::string last_quit_text;
  std::vector<remote_play::FailureStage> progress_stages;
  std::vector<std::string> progress_texts;
  unsigned datacenter_events{};
  std::vector<libchiaking::cloud::DatacenterPing> datacenters;
  std::vector<remote_play::event::CloudDatacenterSelected> selections;
};

class LoopbackTest : public ::testing::Test {
 protected:
  using CloudService = libchiaking::cloud::CloudService;

  void SetUp() override {
    if (!TakionDefaults::cloud())
      GTEST_SKIP() << "the build pins non-default cloud Takion versions";
  }

  static auto prior_row() -> libchiaking::cloud::DatacenterPing {
    return {"dc0", 44, {44, 45}, 1400, 1200, 9296, "198.51.100.4", 15000, true};
  }

  static auto venue_of(CloudService service) -> libchiaking::play::Venue {
    return service == CloudService::legacy_cloud_stream
               ? libchiaking::play::Venue::cloud(
                     libchiaking::catalog::ServiceGeneration::old_service)
               : libchiaking::play::Venue::cloud(
                     libchiaking::catalog::ServiceGeneration::ps5_cloud);
  }
};

}  // namespace play_test::cloud_loopback
