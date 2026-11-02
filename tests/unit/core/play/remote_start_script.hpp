#pragma once

#include "libchiaking/platform/crypto/provider.hpp"
#include "libchiaking/platform/http/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "libchiaking/platform/websocket/provider.hpp"
#include "tests/support/live_providers.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace remote_start_script {
namespace platform = libchiaking::platform;

class ScriptEcdhPair final : public platform::EcdhKeyPair {
 public:
  explicit ScriptEcdhPair(platform::EcdhCurve curve) : curve_(curve) {}
  auto public_coordinates(std::span<std::byte> output) const
      -> libchiaking::Expected<void, platform::CryptoError> override {
    if (output.size() != platform::ecdh_public_coordinates_size(curve_))
      return libchiaking::unexpected(platform::CryptoError::backend_failure);
    for (std::size_t i = 0; i < output.size(); ++i)
      output[i] = static_cast<std::byte>(i);
    return {};
  }
  auto derive_secret(std::span<const std::byte> peer,
                     std::span<std::byte> output) const
      -> libchiaking::Expected<void, platform::CryptoError> override {
    if (peer.size() != platform::ecdh_public_coordinates_size(curve_) ||
        output.size() != platform::ecdh_shared_secret_size(curve_))
      return libchiaking::unexpected(platform::CryptoError::backend_failure);
    for (std::size_t i = 0; i < output.size(); ++i)
      output[i] = static_cast<std::byte>(i + 1);
    return {};
  }

 private:
  platform::EcdhCurve curve_;
};

class RejectCrypto final : public platform::CryptoProvider {
 public:
  auto generate_ecdh(platform::EcdhCurve curve)
      -> libchiaking::Expected<std::unique_ptr<platform::EcdhKeyPair>,
                               platform::CryptoError> override {
    ++generate_calls;
    if (!allow_stream_prepare)
      return libchiaking::unexpected(platform::CryptoError::backend_failure);
    return std::unique_ptr<platform::EcdhKeyPair>{new ScriptEcdhPair(curve)};
  }
  auto sha256(std::span<const std::byte>, std::span<std::byte> output)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    if (!allow_stream_prepare)
      return libchiaking::unexpected(platform::CryptoError::backend_failure);
    std::fill(output.begin(), output.end(), std::byte{});
    return {};
  }
  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<std::byte> output)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    ++hmac_calls;
    if (allow_registration_request) {
      std::fill(output.begin(), output.end(), std::byte{});
      return {};
    }
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }
  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>,
                            std::span<std::byte> output)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    if (allow_registration_request) {
      std::fill(output.begin(), output.end(), std::byte{});
      return {};
    }
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }
  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte> output)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    if (!allow_stream_prepare)
      return libchiaking::unexpected(platform::CryptoError::backend_failure);
    std::fill(output.begin(), output.end(), std::byte{});
    return {};
  }
  unsigned hmac_calls{};
  unsigned generate_calls{};
  bool allow_registration_request{};
  bool allow_stream_prepare{};
};

class ScriptThreading final : public platform::ThreadingProvider {
 public:
  class Handle final : public platform::native::Thread {
   public:
    explicit Handle(ScriptThreading& owner) : owner_(owner) {}
    ~Handle() override {
      if (joinable_) (void)join();
    }
    bool joinable() const noexcept override { return joinable_; }
    auto join()
        -> libchiaking::Expected<void, platform::PlatformError> override {
      owner_.run_pending();
      joinable_ = false;
      return {};
    }
    auto join_for(std::chrono::nanoseconds)
        -> libchiaking::Expected<bool, platform::PlatformError> override {
      (void)join();
      return true;
    }
    auto result() const
        -> libchiaking::Expected<void*, platform::PlatformError> override {
      return nullptr;
    }

   private:
    ScriptThreading& owner_;
    bool joinable_{true};
  };
  auto start_thread(platform::native::ThreadEntry entry)
      -> libchiaking::Expected<std::unique_ptr<platform::native::Thread>,
                               platform::PlatformError> override {
    if (entry.role == platform::ThreadRole::control)
      return delegate.start_thread(entry);
    ++starts;
    if (fail_start)
      return libchiaking::unexpected(platform::PlatformError::thread_failure);
    EXPECT_FALSE(pending) << "a second holepunch thread started";
    EXPECT_EQ(entry.role, platform::ThreadRole::holepunch);
    pending = entry;
    if (!defer) run_pending();
    return std::unique_ptr<platform::native::Thread>{new Handle(*this)};
  }
  void run_pending() {
    if (!pending) return;
    auto entry = *pending;
    pending.reset();
    entry.function(entry.context);
  }
  auto create_mutex(platform::MutexKind kind)
      -> libchiaking::Expected<std::unique_ptr<platform::native::Mutex>,
                               platform::PlatformError> override {
    return delegate.create_mutex(kind);
  }
  auto create_condition_variable() -> libchiaking::Expected<
      std::unique_ptr<platform::native::ConditionVariable>,
      platform::PlatformError> override {
    return delegate.create_condition_variable();
  }
  auto create_stop_source()
      -> libchiaking::Expected<std::unique_ptr<platform::native::StopSource>,
                               platform::PlatformError> override {
    return delegate.create_stop_source();
  }
  platform::DefaultThreadingProvider delegate;
  std::optional<platform::native::ThreadEntry> pending;
  unsigned starts{};
  bool defer{};
  bool fail_start{};
};

class ScriptClock final : public platform::ClockProvider {
 public:
  std::atomic<std::uint64_t> now{100};
  std::uint64_t now_microseconds() const noexcept override { return now; }
};

class ScriptRandom final : public platform::SecureRandomProvider {
 public:
  auto fill_bytes(std::span<std::byte> bytes)
      -> libchiaking::Expected<void, platform::PlatformError> override {
    if (fail)
      return libchiaking::unexpected(platform::PlatformError::system_failure);
    for (auto& byte : bytes) byte = std::byte{};
    return {};
  }
  bool fail{};
};

constexpr std::string_view created_body =
    R"({"dataType":"psn:sessionManager:sys:remotePlaySession:created","to":{"onlineId":"userA"}})";
constexpr std::string_view members_body =
    R"({"dataType":"psn:sessionManager:sys:rps:members:created"})";
constexpr std::string_view ready_members_body =
    R"({"dataType":"psn:sessionManager:sys:rps:members:created","body":{"data":{"members":[{"deviceUniqueId":"0000000000000000000000000000000000000000000000000000000000000000"}]}}})";
constexpr std::string_view ready_custom_body =
    R"({"dataType":"psn:sessionManager:sys:rps:customData1:updated","body":{"data":{"customData1":"QUFBQUFBQUFBQUFBQUFBQUFBQUFBQT09"}}})";
constexpr std::string_view route_offer_body =
    R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"OFFER\",\"reqId\":2,\"error\":0,\"connRequest\":{\"sid\":3,\"peerSid\":4,\"skey\":\"AAAAAAAAAAAAAAAAAAAAAA==\",\"natType\":2,\"candidate\":[{\"type\":\"LOCAL\",\"addr\":\"127.0.0.1\",\"mappedAddr\":\"0.0.0.0\",\"port\":9999,\"mappedPort\":0}],\"defaultRouteMacAddr\":\"\",\"localPeerAddr\":,\"localHashedId\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAA=\"}}"}}}})";
constexpr std::string_view lan_route_offer_body =
    R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"OFFER\",\"reqId\":2,\"error\":0,\"connRequest\":{\"sid\":3,\"peerSid\":4,\"skey\":\"AAAAAAAAAAAAAAAAAAAAAA==\",\"natType\":2,\"candidate\":[{\"type\":\"STATIC\",\"addr\":\"203.0.113.7\",\"mappedAddr\":\"0.0.0.0\",\"port\":9999,\"mappedPort\":0},{\"type\":\"LOCAL\",\"addr\":\"127.0.0.1\",\"mappedAddr\":\"0.0.0.0\",\"port\":9999,\"mappedPort\":0}],\"defaultRouteMacAddr\":\"\",\"localPeerAddr\":,\"localHashedId\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAA=\"}}"}}}})";
constexpr std::string_view static_route_offer_body =
    R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"OFFER\",\"reqId\":2,\"error\":0,\"connRequest\":{\"sid\":3,\"peerSid\":4,\"skey\":\"AAAAAAAAAAAAAAAAAAAAAA==\",\"natType\":2,\"candidate\":[{\"type\":\"LOCAL\",\"addr\":\"192.168.255.7\",\"mappedAddr\":\"0.0.0.0\",\"port\":9999,\"mappedPort\":0},{\"type\":\"STATIC\",\"addr\":\"127.0.0.1\",\"mappedAddr\":\"0.0.0.0\",\"port\":9999,\"mappedPort\":0}],\"defaultRouteMacAddr\":\"\",\"localPeerAddr\":,\"localHashedId\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAA=\"}}"}}}})";
constexpr std::string_view route_result_body =
    R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"RESULT\",\"reqId\":2,\"error\":0,\"connRequest\":{}}"}}}})";
constexpr std::string_view route_accept_body =
    R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"ACCEPT\",\"reqId\":2,\"error\":0,\"connRequest\":{}}"}}}})";
constexpr std::string_view data_offer_body =
    R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"OFFER\",\"reqId\":4,\"error\":0,\"connRequest\":{\"sid\":3,\"peerSid\":4,\"skey\":\"AAAAAAAAAAAAAAAAAAAAAA==\",\"natType\":2,\"candidate\":[{\"type\":\"LOCAL\",\"addr\":\"127.0.0.1\",\"mappedAddr\":\"0.0.0.0\",\"port\":9999,\"mappedPort\":0}],\"defaultRouteMacAddr\":\"\",\"localPeerAddr\":,\"localHashedId\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAA=\"}}"}}}})";
constexpr std::string_view data_result_body =
    R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"RESULT\",\"reqId\":5,\"error\":0,\"connRequest\":{}}"}}}})";
constexpr std::string_view data_accept_body =
    R"({"dataType":"psn:sessionManager:sys:rps:sessionMessage:created","body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"ACCEPT\",\"reqId\":4,\"error\":0,\"connRequest\":{}}"}}}})";
constexpr std::string_view members_deleted_body =
    R"({"dataType":"psn:sessionManager:sys:rps:members:deleted","body":{"data":{"sessionId":"01234567-89ab-cdef-0123-456789abcdef"}}})";

struct ScriptProgress {
  std::atomic<unsigned> stun_queries{};
  std::atomic<unsigned> stun_bindings{};
  std::atomic<bool> released{};
};

class ScriptHttp final : public platform::HttpProvider {
 public:
  std::function<void(const platform::HttpRequest&)> on_request;
  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    if (on_request) on_request(request);
    const bool stun_lookup =
        request.method == platform::HttpMethod::get &&
        (request.url.view().find("valid_hosts.txt") != std::string::npos ||
         request.url.view().find("valid_ipv6s.txt") != std::string::npos);
    if (!stun_lookup) {
      EXPECT_TRUE(!request.headers.empty() &&
                  request.headers.front().value.view() ==
                      "Bearer SYNTHETIC_TEST_TOKEN")
          << "the request carries the remote play bearer";
    }
    platform::HttpResponse response;
    switch (request.method) {
      case platform::HttpMethod::get:
        if (stun_lookup &&
            request.url.view().find("valid_ipv6s.txt") != std::string::npos) {
          order.emplace_back("stun_ipv6");
          response.status_code = 200;
        } else if (stun_lookup) {
          ++progress->stun_queries;
          order.emplace_back("stun");
          response.status_code = 200;
          set_body(response, "preferred.example:9999\n");
        } else if (request.url.view().find("/remotePlaySessions") !=
                   std::string::npos) {
          order.emplace_back(
              request.url.view().ends_with("?view=v1.0") ? "view" : "status");
          response.status_code = 200;
        } else if (request.url.view().find("baseUrls/userProfile") !=
                   std::string::npos) {
          order.emplace_back("profile");
          response.status_code = 200;
          set_body(response, R"({"url":"https://profile.example.test/base"})");
        } else {
          order.emplace_back("fqdn");
          response.status_code = 200;
          set_body(
              response,
              R"({"fqdn":"mobile-pushcl.np.communication.playstation.net"})");
        }
        break;
      case platform::HttpMethod::post:
        if (request.url.view().find("/sessionMessage") != std::string::npos) {
          order.emplace_back("message");
          response.status_code = 204;
        } else if (request.url.view().find("/commands") != std::string::npos) {
          order.emplace_back("start");
          response.status_code = start_status;
        } else if (request.url.view().find("profile.example.test") !=
                   std::string::npos) {
          order.emplace_back("wake");
          response.status_code = 200;
        } else {
          order.emplace_back("create");
          response.status_code = 201;
          set_body(
              response,
              R"({"remotePlaySessions":[{"sessionId":"01234567-89ab-cdef-0123-456789abcdef","members":[{"accountId":"123456789"}]}]})");
        }
        break;
      case platform::HttpMethod::delete_:
        progress->released = true;
        order.emplace_back("release");
        response.status_code = release_status;
        break;
      case platform::HttpMethod::head:
      case platform::HttpMethod::put:
      case platform::HttpMethod::patch:
        ADD_FAILURE() << "an unscripted HTTP method was used";
        break;
    }
    return response;
  }
  std::shared_ptr<ScriptProgress> progress = std::make_shared<ScriptProgress>();
  std::vector<std::string> order;
  std::uint16_t start_status{401};
  std::uint16_t release_status{204};

 private:
  static void set_body(platform::HttpResponse& response,
                       std::string_view body) {
    response.body.assign(body);
  }
};

struct ScriptCounts {
  std::atomic<unsigned> connect{}, close{}, receive{};
  std::function<void()> on_exhaustion;
  std::atomic<bool> exhausted{};
  std::function<void()> on_close;
  bool fail_close{};
  std::atomic<bool> fail_live{};
  std::atomic<unsigned> live_failures{};
  std::vector<std::string_view> after_release;
  std::size_t after_release_index{};
  std::shared_ptr<ScriptProgress> progress;
};

class ScriptConnection final : public platform::WebSocketConnection {
 public:
  ScriptConnection(std::shared_ptr<ScriptCounts> counts,
                   std::vector<std::string_view> script)
      : counts_(std::move(counts)), script_(std::move(script)) {}
  auto receive(const platform::StopToken& stop)
      -> libchiaking::Expected<platform::WebSocketMessage,
                               platform::WebSocketError> override {
    ++counts_->receive;
    if (index_ == script_.size()) {
      if (counts_->on_exhaustion && !counts_->exhausted.exchange(true)) {
        while (!stop.stop_requested()) {
          counts_->on_exhaustion();
          test_support::LiveProviders::nap(std::chrono::milliseconds{1});
        }
        return libchiaking::unexpected(platform::WebSocketError::cancelled);
      }
      // Once the peer has offered a route, an empty script is an idle
      // live WebSocket, not an invented disconnect during negotiation.
      if (counts_->progress && !counts_->progress->released.load()) {
        const bool offered = std::find(script_.begin(), script_.end(),
                                       route_offer_body) != script_.end();
        while (!stop.stop_requested() && !counts_->progress->released.load()) {
          if (counts_->fail_live.load()) {
            ++counts_->live_failures;
            return libchiaking::unexpected(
                platform::WebSocketError::backend_failure);
          }
          if (offered && counts_->progress->stun_bindings.load() >= 2 &&
              std::find(script_.begin(), script_.end(), data_offer_body) ==
                  script_.end())
            return libchiaking::unexpected(
                platform::WebSocketError::backend_failure);
          test_support::LiveProviders::nap(std::chrono::milliseconds{1});
        }
        if (stop.stop_requested())
          return libchiaking::unexpected(platform::WebSocketError::cancelled);
      }
      if (counts_->progress && counts_->progress->released.load() &&
          counts_->after_release_index < counts_->after_release.size()) {
        const auto body =
            counts_->after_release[counts_->after_release_index++];
        platform::WebSocketMessage message;
        message.payload.assign(body);
        return message;
      }
      return libchiaking::unexpected(platform::WebSocketError::backend_failure);
    }
    if (counts_->progress && (script_[index_] == data_offer_body ||
                              script_[index_] == members_deleted_body)) {
      while (!stop.stop_requested() && !counts_->fail_live.load() &&
             (script_[index_] == data_offer_body
                  ? (counts_->progress->stun_bindings.load() < 2 &&
                     !counts_->progress->released.load())
                  : !counts_->progress->released.load()))
        test_support::LiveProviders::nap(std::chrono::milliseconds{1});
      if (counts_->fail_live.load()) {
        ++counts_->live_failures;
        return libchiaking::unexpected(
            platform::WebSocketError::backend_failure);
      }
      if (stop.stop_requested())
        return libchiaking::unexpected(platform::WebSocketError::cancelled);
    }
    const auto body = script_[index_++];
    platform::WebSocketMessage message;
    message.payload.assign(body);
    return message;
  }
  auto close()
      -> libchiaking::Expected<void, platform::WebSocketError> override {
    ++counts_->close;
    if (counts_->on_close) counts_->on_close();
    if (counts_->fail_close)
      return libchiaking::unexpected(platform::WebSocketError::backend_failure);
    return {};
  }

 private:
  std::shared_ptr<ScriptCounts> counts_;
  std::vector<std::string_view> script_;
  std::size_t index_{};
};

class ScriptWebSocket final : public platform::WebSocketProvider {
 public:
  ScriptWebSocket(std::shared_ptr<ScriptCounts> counts,
                  std::vector<std::string_view> script)
      : counts_(std::move(counts)), script_(std::move(script)) {}
  auto connect(const platform::WebSocketRequest& request,
               const platform::StopToken&)
      -> libchiaking::Expected<std::unique_ptr<platform::WebSocketConnection>,
                               platform::WebSocketError> override {
    EXPECT_TRUE(!request.headers.empty() &&
                request.headers.front().value.view() ==
                    "Bearer SYNTHETIC_TEST_TOKEN")
        << "the connection carries the remote play bearer";
    ++counts_->connect;
    return std::unique_ptr<platform::WebSocketConnection>{
        new ScriptConnection(counts_, script_)};
  }

 private:
  std::shared_ptr<ScriptCounts> counts_;
  std::vector<std::string_view> script_;
};

}  // namespace remote_start_script
