#pragma once

#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/platform/net/default.hpp"
#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"
#include "src/core/play/adapters/links/home_link_configuration.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/live_providers.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/play/takion_defaults.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace play_test::home_links {

namespace play = libchiaking::play;
namespace platform = libchiaking::platform;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;

using Deadline = std::chrono::steady_clock::time_point;
using Target = remote_play::RegistrationTarget;
using StreamError = remote_play::StreamError;
using StreamState = remote_play::StreamState;
using NetError = platform::NetError;

class HomeLinkTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!TakionDefaults::consoles())
      GTEST_SKIP() << "the build pins non-default console Takion versions";
  }
};

class CapturingDiagnostics final : public platform::DiagnosticsProvider {
 public:
  void write(platform::LogRecord record) noexcept override {
    lines_.emplace_back(std::string{record.message}, record.severity);
  }

  auto contains(std::string_view text) const -> bool {
    return std::ranges::any_of(lines_, [text](const auto& line) {
      return line.first.find(text) != std::string::npos;
    });
  }

  auto has_line(std::string_view text) const -> bool {
    return std::ranges::any_of(
        lines_, [text](const auto& line) { return line.first == text; });
  }

  auto severity_of(std::string_view text) const
      -> std::optional<platform::LogSeverity> {
    const auto found = std::ranges::find_if(lines_, [text](const auto& line) {
      return line.first.find(text) != std::string::npos;
    });
    if (found == lines_.end()) return std::nullopt;
    return found->second;
  }

  auto severity_of_exact(std::string_view text) const
      -> std::optional<platform::LogSeverity> {
    const auto found = std::ranges::find_if(
        lines_, [text](const auto& line) { return line.first == text; });
    if (found == lines_.end()) return std::nullopt;
    return found->second;
  }

 private:
  std::vector<std::pair<std::string, platform::LogSeverity>> lines_;
};

class FixedClock final : public platform::ClockProvider {
 public:
  auto now_microseconds() const noexcept -> std::uint64_t override {
    if (!released_at_) return now_;
    const auto passed = std::chrono::duration_cast<std::chrono::microseconds>(
        test_support::LiveProviders::now() - *released_at_);
    return now_ + static_cast<std::uint64_t>(passed.count());
  }

  void advance(std::chrono::microseconds by) noexcept {
    now_ += static_cast<std::uint64_t>(by.count());
  }

  void release() noexcept { released_at_ = test_support::LiveProviders::now(); }

 private:
  std::uint64_t now_{1'000'000};
  std::optional<Deadline> released_at_;
};

class EventCounter final : public play::LinkEventSink {
 public:
  void on_event(remote_play::StreamEvent event) override {
    if (std::holds_alternative<remote_play::event::PinRequired>(event))
      ++pin_events_;
    const auto* media = std::get_if<remote_play::event::EncodedMedia>(&event);
    if (media && media->profile_header) {
      ++profile_headers_;
      return;
    }
    if (media) ++media_events_;
    if (media && media->kind == remote_play::EncodedMediaKind::rumble &&
        streaming_events_ == 0)
      ++rumble_before_streaming_;
    if (std::holds_alternative<remote_play::event::Failed>(event))
      ++failed_events_;
    if (const auto* notice =
            std::get_if<remote_play::event::ConnectionWaiting>(&event))
      waiting_.push_back(notice->waiting);
    if (const auto* info =
            std::get_if<remote_play::event::StreamInfoReady>(&event)) {
      ++stream_info_events_;
      last_info_ = *info;
    }
    if (const auto* state =
            std::get_if<remote_play::event::StateChanged>(&event);
        state && state->state == StreamState::streaming) {
      ++streaming_events_;
      info_before_streaming_ = stream_info_events_ != 0;
    }
    ++events_;
    last_ = std::move(event);
  }

  template <class Event>
  auto last_as() const -> const Event* {
    return std::get_if<Event>(&last_);
  }

  void forget_media() noexcept { media_events_ = 0; }

  auto events() const noexcept -> unsigned { return events_; }
  auto pin_events() const noexcept -> unsigned { return pin_events_; }
  auto media_events() const noexcept -> unsigned { return media_events_; }
  auto profile_headers() const noexcept -> unsigned { return profile_headers_; }
  auto failed_events() const noexcept -> unsigned { return failed_events_; }
  auto stream_info_events() const noexcept -> unsigned {
    return stream_info_events_;
  }
  auto streaming_events() const noexcept -> unsigned {
    return streaming_events_;
  }
  auto rumble_before_streaming() const noexcept -> unsigned {
    return rumble_before_streaming_;
  }
  auto info_before_streaming() const noexcept -> bool {
    return info_before_streaming_;
  }
  auto waiting() const -> const std::vector<bool>& { return waiting_; }
  auto last_info() const noexcept
      -> const std::optional<remote_play::event::StreamInfoReady>& {
    return last_info_;
  }

 private:
  unsigned events_{};
  unsigned pin_events_{};
  unsigned media_events_{};
  unsigned profile_headers_{};
  unsigned failed_events_{};
  unsigned stream_info_events_{};
  unsigned streaming_events_{};
  unsigned rumble_before_streaming_{};
  bool info_before_streaming_{};
  std::vector<bool> waiting_;
  std::optional<remote_play::event::StreamInfoReady> last_info_;
  remote_play::StreamEvent last_{remote_play::event::PinRequired{}};
};

class RecordingSink final : public play::LinkEventSink {
 public:
  void on_event(remote_play::StreamEvent event) override {
    events_.push_back(std::move(event));
  }

  template <class Event>
  auto all() const -> std::vector<Event> {
    std::vector<Event> result;
    for (const auto& event : events_)
      if (const auto* value = std::get_if<Event>(&event))
        result.push_back(*value);
    return result;
  }

 private:
  std::vector<remote_play::StreamEvent> events_;
};

class FakeEcdhKeyPair final : public platform::EcdhKeyPair {
 public:
  explicit FakeEcdhKeyPair(platform::EcdhCurve curve) : curve_(curve) {}

  auto public_coordinates(std::span<std::byte> output) const
      -> libchiaking::Expected<void, platform::CryptoError> override {
    if (output.size() != platform::ecdh_public_coordinates_size(curve_))
      return libchiaking::unexpected(platform::CryptoError::invalid_argument);
    for (std::size_t index = 0; index < output.size(); ++index)
      output[index] = static_cast<std::byte>(index);
    return {};
  }

  auto derive_secret(std::span<const std::byte> peer,
                     std::span<std::byte> secret) const
      -> libchiaking::Expected<void, platform::CryptoError> override {
    if (peer.size() != platform::ecdh_public_coordinates_size(curve_) ||
        secret.size() != platform::ecdh_shared_secret_size(curve_))
      return libchiaking::unexpected(platform::CryptoError::invalid_argument);
    for (std::size_t index = 0; index < secret.size(); ++index)
      secret[index] = static_cast<std::byte>(index + 1);
    return {};
  }

 private:
  platform::EcdhCurve curve_;
};

class ZeroCrypto final : public platform::CryptoProvider {
 public:
  using Result = libchiaking::Expected<void, platform::CryptoError>;

  auto generate_ecdh(platform::EcdhCurve curve)
      -> libchiaking::Expected<std::unique_ptr<platform::EcdhKeyPair>,
                               platform::CryptoError> override {
    return std::unique_ptr<platform::EcdhKeyPair>{new FakeEcdhKeyPair(curve)};
  }
  auto sha256(std::span<const std::byte>, std::span<std::byte> output)
      -> Result override {
    std::ranges::fill(output, std::byte{});
    return {};
  }
  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<std::byte> output) -> Result override {
    std::ranges::fill(output, std::byte{});
    return {};
  }
  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>,
                            std::span<std::byte> output) -> Result override {
    std::ranges::fill(output, std::byte{});
    return {};
  }
  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte> tag)
      -> Result override {
    std::ranges::fill(tag, std::byte{});
    return {};
  }
};

class ScriptedDns final : public platform::DnsProvider {
 public:
  using Answer =
      libchiaking::Expected<std::vector<platform::IpAddress>, NetError>;

  void answer(std::vector<platform::IpAddress> addresses) {
    answers_ = std::move(addresses);
  }
  void fail_with(NetError error) { error_ = error; }
  void hang_until_stopped() { hang_until_stopped_ = true; }

  auto resolve(std::string_view requested_host,
               std::string_view requested_service,
               const platform::StopToken& stop) -> Answer override {
    ++stop_aware_calls_;
    if (!hang_until_stopped_) return resolve(requested_host, requested_service);
    ++calls_;
    const auto deadline =
        test_support::LiveProviders::now() + std::chrono::seconds{5};
    while (!stop.stop_requested() &&
           test_support::LiveProviders::now() < deadline)
      test_support::LiveProviders::nap(std::chrono::milliseconds{1});
    return libchiaking::unexpected(NetError::interrupted);
  }

  auto resolve(std::string_view requested_host,
               std::string_view requested_service) -> Answer override {
    ++calls_;
    if (hang_until_stopped_) {
      test_support::LiveProviders::nap(std::chrono::seconds{5});
      return libchiaking::unexpected(NetError::timed_out);
    }
    host_ = requested_host;
    service_ = requested_service;
    if (error_) return libchiaking::unexpected(*error_);
    return answers_;
  }

  auto calls() const noexcept -> unsigned { return calls_; }
  auto stop_aware_calls() const noexcept -> unsigned {
    return stop_aware_calls_;
  }
  auto host() const -> const std::string& { return host_; }
  auto service() const -> const std::string& { return service_; }

 private:
  std::vector<platform::IpAddress> answers_;
  std::optional<NetError> error_;
  bool hang_until_stopped_{};
  std::string host_;
  std::string service_;
  unsigned calls_{};
  unsigned stop_aware_calls_{};
};

class HeldTcpSocket final : public platform::TcpSocket {
 public:
  HeldTcpSocket(std::unique_ptr<platform::TcpSocket> inner,
                std::shared_ptr<std::atomic<bool>> hold)
      : inner_(std::move(inner)), hold_(std::move(hold)) {}

  auto owner() const noexcept -> const platform::NetworkProvider& override {
    return inner_->owner();
  }
  auto bind(const platform::SocketEndpoint& endpoint)
      -> libchiaking::Expected<void, NetError> override {
    return inner_->bind(endpoint);
  }
  auto connect(const platform::SocketEndpoint& endpoint)
      -> libchiaking::Expected<void, NetError> override {
    return inner_->connect(endpoint);
  }
  auto complete_connect() -> libchiaking::Expected<void, NetError> override {
    return inner_->complete_connect();
  }
  auto send(std::span<const std::byte> data)
      -> libchiaking::Expected<std::size_t, NetError> override {
    if (hold_->load()) return libchiaking::unexpected(NetError::would_block);
    return inner_->send(data);
  }
  auto receive(std::span<std::byte> buffer)
      -> libchiaking::Expected<std::size_t, NetError> override {
    return inner_->receive(buffer);
  }
  auto local_endpoint()
      -> libchiaking::Expected<platform::SocketEndpoint, NetError> override {
    return inner_->local_endpoint();
  }
  auto peer_endpoint()
      -> libchiaking::Expected<platform::SocketEndpoint, NetError> override {
    return inner_->peer_endpoint();
  }
  auto close() -> libchiaking::Expected<void, NetError> override {
    return inner_->close();
  }

 private:
  std::unique_ptr<platform::TcpSocket> inner_;
  std::shared_ptr<std::atomic<bool>> hold_;
};

class RecordingSockets final : public platform::SocketProvider {
 public:
  void delegate_to(platform::SocketProvider& sockets) noexcept {
    delegate_ = &sockets;
  }
  void refuse(platform::IpAddress address) { refused_ = address; }
  void hold_sends(bool held) {
    if (!held_) held_ = std::make_shared<std::atomic<bool>>(false);
    held_->store(held);
  }

  auto create_udp_socket(const platform::IpAddress&)
      -> libchiaking::Expected<std::unique_ptr<platform::UdpSocket>,
                               NetError> override {
    return libchiaking::unexpected(NetError::not_supported);
  }

  auto create_tcp_socket(const platform::IpAddress& family_of)
      -> libchiaking::Expected<std::unique_ptr<platform::TcpSocket>,
                               NetError> override {
    tcp_family_ = family_of;
    tcp_attempts_.push_back(family_of);
    if (!delegate_ || family_of == refused_)
      return libchiaking::unexpected(NetError::not_supported);
    auto created = delegate_->create_tcp_socket(family_of);
    if (!created || !held_) return created;
    return std::unique_ptr<platform::TcpSocket>{
        new HeldTcpSocket(std::move(created.value()), held_)};
  }

  auto create_tcp_listener(const platform::IpAddress&)
      -> libchiaking::Expected<std::unique_ptr<platform::TcpListener>,
                               NetError> override {
    return libchiaking::unexpected(NetError::not_supported);
  }

  auto tcp_family() const -> const std::optional<platform::IpAddress>& {
    return tcp_family_;
  }
  auto tcp_attempts() const -> const std::vector<platform::IpAddress>& {
    return tcp_attempts_;
  }

 private:
  platform::SocketProvider* delegate_{};
  std::optional<platform::IpAddress> refused_;
  std::shared_ptr<std::atomic<bool>> held_;
  std::optional<platform::IpAddress> tcp_family_;
  std::vector<platform::IpAddress> tcp_attempts_;
};

class ScriptedNetwork final : public platform::NetworkProvider {
 public:
  auto sockets() noexcept -> platform::SocketProvider& override {
    return sockets_;
  }
  auto pollers() noexcept -> platform::PollerProvider& override {
    return default_.pollers();
  }
  auto dns() noexcept -> platform::DnsProvider& override { return dns_; }
  auto interfaces() noexcept -> platform::InterfacesProvider& override {
    return default_.interfaces();
  }
  auto capabilities() const noexcept -> platform::NetworkCapabilities override {
    return default_.capabilities();
  }

  auto scripted_sockets() noexcept -> RecordingSockets& { return sockets_; }
  auto scripted_dns() noexcept -> ScriptedDns& { return dns_; }
  auto default_sockets() noexcept -> platform::SocketProvider& {
    return default_.sockets();
  }

 private:
  platform::DefaultNetworkProvider default_{
      test_support::LiveProviders::threading(),
      test_support::LiveProviders::steady()};
  RecordingSockets sockets_;
  ScriptedDns dns_;
};

class FirstUdpFailsSockets final : public platform::SocketProvider {
 public:
  explicit FirstUdpFailsSockets(platform::SocketProvider& inner)
      : inner_(inner) {}

  auto create_udp_socket(const platform::IpAddress& family_of)
      -> libchiaking::Expected<std::unique_ptr<platform::UdpSocket>,
                               NetError> override {
    if (udp_calls_++ == 0)
      return libchiaking::unexpected(NetError::permission_denied);
    return inner_.create_udp_socket(family_of);
  }
  auto create_tcp_socket(const platform::IpAddress& family_of)
      -> libchiaking::Expected<std::unique_ptr<platform::TcpSocket>,
                               NetError> override {
    return inner_.create_tcp_socket(family_of);
  }
  auto create_tcp_listener(const platform::IpAddress& family_of)
      -> libchiaking::Expected<std::unique_ptr<platform::TcpListener>,
                               NetError> override {
    return inner_.create_tcp_listener(family_of);
  }

  auto udp_calls() const noexcept -> unsigned { return udp_calls_; }

 private:
  platform::SocketProvider& inner_;
  unsigned udp_calls_{};
};

class FirstUdpFailsNetwork final : public platform::NetworkProvider {
 public:
  FirstUdpFailsNetwork() : sockets_(default_.sockets()) {}

  auto sockets() noexcept -> platform::SocketProvider& override {
    return sockets_;
  }
  auto pollers() noexcept -> platform::PollerProvider& override {
    return default_.pollers();
  }
  auto dns() noexcept -> platform::DnsProvider& override {
    return default_.dns();
  }
  auto interfaces() noexcept -> platform::InterfacesProvider& override {
    return default_.interfaces();
  }
  auto capabilities() const noexcept -> platform::NetworkCapabilities override {
    return default_.capabilities();
  }

  auto scripted() noexcept -> FirstUdpFailsSockets& { return sockets_; }

 private:
  platform::DefaultNetworkProvider default_{
      test_support::LiveProviders::threading(),
      test_support::LiveProviders::steady()};
  FirstUdpFailsSockets sockets_;
};

class RejectHttp final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest&, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls_;
    if (cancel_on_call_) cancel_on_call_->request_stop();
    return libchiaking::unexpected(platform::HttpError::unsupported);
  }

  void cancel_on_call(test_support::AtomicStop& state) noexcept {
    cancel_on_call_ = &state;
  }
  auto calls() const noexcept -> unsigned { return calls_; }

 private:
  unsigned calls_{};
  test_support::AtomicStop* cancel_on_call_{};
};

class CtrlWire {
 public:
  static auto bytes(std::string_view text) -> std::vector<std::byte> {
    std::vector<std::byte> result;
    result.reserve(text.size());
    for (const char c : text) result.push_back(static_cast<std::byte>(c));
    return result;
  }

  static auto of(std::initializer_list<int> values) -> std::vector<std::byte> {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const int value : values)
      result.push_back(static_cast<std::byte>(value));
    return result;
  }

  static auto frame(std::uint16_t type, std::span<const std::byte> payload = {})
      -> std::vector<std::byte> {
    const auto header =
        remote_play::CtrlCodec::encode_frame_header(type, payload.size());
    EXPECT_TRUE(header);
    if (!header) return {};
    std::vector<std::byte> result(header.value().begin(), header.value().end());
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
  }

  static auto session_id(char fill = 'A') -> std::vector<std::byte> {
    std::vector<std::byte> id{std::byte{0x4a}};
    id.insert(id.end(), 24, static_cast<std::byte>(fill));
    return id;
  }

  static auto reply_with_session_id(std::string_view http)
      -> std::vector<std::byte> {
    auto reply = bytes(http);
    const auto id = frame(0x33, session_id());
    reply.insert(reply.end(), id.begin(), id.end());
    return reply;
  }
};

class Loopback {
 public:
  static auto listener(
      platform::SocketProvider& sockets, int backlog = 1,
      platform::IpAddress address = platform::IpAddress::loopback_ipv4())
      -> std::unique_ptr<platform::TcpListener> {
    auto created = sockets.create_tcp_listener(address);
    EXPECT_TRUE(created);
    if (!created) return nullptr;
    auto listener = std::move(created.value());
    const auto bound = listener->bind({address, 0});
    EXPECT_TRUE(bound);
    const auto listening = listener->listen(backlog);
    EXPECT_TRUE(listening);
    if (!bound || !listening) return nullptr;
    return listener;
  }

  static auto udp_peer(platform::SocketProvider& sockets)
      -> std::unique_ptr<platform::UdpSocket> {
    auto created =
        sockets.create_udp_socket(platform::IpAddress::loopback_ipv4());
    EXPECT_TRUE(created);
    if (!created) return nullptr;
    auto peer = std::move(created.value());
    const auto bound = peer->bind({platform::IpAddress::loopback_ipv4(), 0});
    EXPECT_TRUE(bound);
    if (!bound) return nullptr;
    return peer;
  }
};

class SessionInitConsole {
 public:
  SessionInitConsole(play::Link& link, platform::TcpListener& listener,
                     Deadline deadline)
      : link_(link), listener_(listener), deadline_(deadline) {}

  void open_ctrl(std::string_view expected_path, FixedClock* clock = nullptr) {
    ASSERT_NO_FATAL_FAILURE(answer_session_init(expected_path));
    if (clock) {
      ASSERT_NO_FATAL_FAILURE(hold_ctrl(*clock));
    }
    ASSERT_NO_FATAL_FAILURE(accept_ctrl());
  }

  void stop_before_ctrl(std::string_view expected_path, FixedClock& clock) {
    ASSERT_NO_FATAL_FAILURE(answer_session_init(expected_path));
    ASSERT_NO_FATAL_FAILURE(hold_ctrl(clock));
    EXPECT_TRUE(link_.stop());
    const auto stopped = link_.pump_once();
    ASSERT_FALSE(stopped);
    EXPECT_EQ(stopped.error(), StreamError::invalid_state);
    ASSERT_NO_FATAL_FAILURE(expect_nothing_waiting());
  }

  auto ctrl() const noexcept -> platform::TcpSocket* { return ctrl_.get(); }
  auto take_ctrl() noexcept -> std::unique_ptr<platform::TcpSocket> {
    return std::move(ctrl_);
  }

 private:
  void answer_session_init(std::string_view expected_path) {
    std::unique_ptr<platform::TcpSocket> init;
    while (!init && test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(link_.pump_once());
      auto accepted = listener_.accept();
      if (accepted)
        init = std::move(accepted.value());
      else
        ASSERT_EQ(accepted.error(), NetError::would_block);
      test_support::LiveProviders::spin_pause();
    }
    ASSERT_NE(init, nullptr);
    std::string request;
    while (request.find("\r\n\r\n") == std::string::npos &&
           test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(link_.pump_once());
      std::array<std::byte, 1024> buffer{};
      const auto received = init->receive(buffer);
      if (received)
        request.append(reinterpret_cast<const char*>(buffer.data()),
                       received.value());
      else
        ASSERT_EQ(received.error(), NetError::would_block);
      test_support::LiveProviders::spin_pause();
    }
    EXPECT_NE(request.find(expected_path), std::string::npos) << request;
    EXPECT_EQ(request.find("/ctrl"), std::string::npos) << request;
    ASSERT_NO_FATAL_FAILURE(expect_nothing_waiting());

    constexpr std::string_view response =
        "HTTP/1.1 200 OK\r\nRP-Nonce: AAECAwQFBgcICQoLDA0ODw==\r\n\r\n";
    auto remaining = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(response.data()), response.size());
    while (!remaining.empty() &&
           test_support::LiveProviders::now() < deadline_) {
      const auto sent = init->send(remaining);
      if (sent)
        remaining = remaining.subspan(sent.value());
      else
        ASSERT_EQ(sent.error(), NetError::would_block);
      test_support::LiveProviders::spin_pause();
    }
    ASSERT_TRUE(remaining.empty());
    bool reply_taken = false;
    while (!reply_taken && test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(link_.pump_once());
      std::array<std::byte, 16> rest{};
      const auto hung_up = init->receive(rest);
      reply_taken = hung_up ? hung_up.value() == 0
                            : hung_up.error() != NetError::would_block;
    }
    ASSERT_TRUE(reply_taken);
    EXPECT_TRUE(init->close());
  }

  void hold_ctrl(FixedClock& clock) {
    for (int round = 0; round < 100; ++round) {
      ASSERT_TRUE(link_.pump_once());
      ASSERT_NO_FATAL_FAILURE(expect_nothing_waiting());
      test_support::LiveProviders::spin_pause();
    }
    clock.advance(std::chrono::microseconds{9'999});
    ASSERT_TRUE(link_.pump_once());
    ASSERT_NO_FATAL_FAILURE(expect_nothing_waiting());
    clock.advance(std::chrono::microseconds{1});
  }

  void accept_ctrl() {
    while (test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(link_.pump_once());
      auto accepted = listener_.accept();
      if (accepted) {
        ctrl_ = std::move(accepted.value());
        return;
      }
      ASSERT_EQ(accepted.error(), NetError::would_block);
      test_support::LiveProviders::spin_pause();
    }
    FAIL() << "the link never opened Ctrl";
  }

  void expect_nothing_waiting() {
    const auto early = listener_.accept();
    ASSERT_FALSE(early);
    EXPECT_EQ(early.error(), NetError::would_block);
  }

  play::Link& link_;
  platform::TcpListener& listener_;
  Deadline deadline_;
  std::unique_ptr<platform::TcpSocket> ctrl_;
};

class CtrlFeatureRig {
 public:
  explicit CtrlFeatureRig(
      bool keyboard, std::string_view ctrl_response = "HTTP/1.1 200 OK\r\n\r\n",
      Target target = Target::ps5_1)
      : keyboard_(keyboard), ctrl_response_(ctrl_response), target_(target) {}

  CtrlFeatureRig(const CtrlFeatureRig&) = delete;
  auto operator=(const CtrlFeatureRig&) -> CtrlFeatureRig& = delete;

  ~CtrlFeatureRig() {
    if (session_) {
      EXPECT_TRUE(session_->stop());
    }
    if (console_) {
      EXPECT_TRUE(console_->close());
    }
    if (listener_) {
      EXPECT_TRUE(listener_->close());
    }
    if (calibration_peer_) {
      EXPECT_TRUE(calibration_peer_->close());
    }
    if (active_peer_) {
      EXPECT_TRUE(active_peer_->close());
    }
  }

  void open() {
    auto& sockets = providers_.network().sockets();
    listener_ = Loopback::listener(sockets);
    ASSERT_NE(listener_, nullptr);
    calibration_peer_ = Loopback::udp_peer(sockets);
    active_peer_ = Loopback::udp_peer(sockets);
    ASSERT_NE(calibration_peer_, nullptr);
    ASSERT_NE(active_peer_, nullptr);

    play::HomeLinkConfiguration config;
    config.console.target = target_;
    config.host = "127.0.0.1";
    config.ctrl_endpoint = listener_->local_endpoint().value();
    config.enable_dualsense_ctrl = target_ == Target::ps5_1;
    config.enable_keyboard = keyboard_;
    config.route.local = {platform::IpAddress::loopback_ipv4(), 0};
    config.route.remote = calibration_peer_->local_endpoint().value();
    config.route.dont_fragment = remote_play::DontFragmentPolicy::disabled;
    config.active_stream_port = active_peer_->local_endpoint().value().port;
    auto made =
        play::HomeLinkConfiguration::link(std::move(config), providers_);
    ASSERT_TRUE(made);
    session_ = std::move(made.value());
    session_->set_event_sink(sink_);
    EXPECT_FALSE(session_->execute(remote_play::command::GotoBed{}));
    ASSERT_TRUE(session_->start());
    SessionInitConsole init{*session_, *listener_, deadline_};
    ASSERT_NO_FATAL_FAILURE(init.open_ctrl(target_ == Target::ps5_1
                                               ? "/sie/ps5/rp/sess/init"
                                               : "/sie/ps4/rp/sess/init",
                                           &clock_));
    console_ = init.take_ctrl();
    ASSERT_NO_FATAL_FAILURE(
        send(CtrlWire::reply_with_session_id(ctrl_response_)));
    while (session_->state() != StreamState::negotiating_stream &&
           test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(session_->pump_once());
      test_support::LiveProviders::spin_pause();
    }
    ASSERT_EQ(session_->state(), StreamState::negotiating_stream);
  }

  auto link() -> play::Link& { return *session_; }
  auto sink() const -> const RecordingSink& { return sink_; }
  auto diagnostics() const -> const CapturingDiagnostics& {
    return diagnostics_;
  }

  void send(std::span<const std::byte> bytes) {
    std::size_t sent{};
    while (sent < bytes.size() &&
           test_support::LiveProviders::now() < deadline_) {
      const auto part = console_->send(bytes.subspan(sent));
      if (part)
        sent += part.value();
      else
        ASSERT_EQ(part.error(), NetError::would_block);
      test_support::LiveProviders::spin_pause();
    }
    ASSERT_EQ(sent, bytes.size());
  }

  void send_frame(std::uint16_t type, std::span<const std::byte> payload) {
    send(CtrlWire::frame(type, payload));
  }

  auto next_frame() -> std::optional<remote_play::CtrlOutboundFrame> {
    for (;;) {
      if (ctrl_offset_ == 0) {
        const std::string_view request(
            reinterpret_cast<const char*>(ctrl_wire_.data()),
            ctrl_wire_.size());
        const auto end = request.find("\r\n\r\n");
        if (end != std::string_view::npos) ctrl_offset_ = end + 4;
      }
      if (ctrl_offset_ != 0 && ctrl_wire_.size() - ctrl_offset_ >= 8) {
        const auto at = [this](std::size_t index) {
          return std::to_integer<std::size_t>(ctrl_wire_[ctrl_offset_ + index]);
        };
        const auto length =
            (at(0) << 24) | (at(1) << 16) | (at(2) << 8) | at(3);
        if (ctrl_wire_.size() - ctrl_offset_ - 8 >= length) {
          const auto begin = ctrl_wire_.begin() +
                             static_cast<std::ptrdiff_t>(ctrl_offset_ + 8);
          remote_play::CtrlOutboundFrame frame{
              static_cast<std::uint16_t>((at(4) << 8) | at(5)),
              {begin, begin + static_cast<std::ptrdiff_t>(length)}};
          ctrl_offset_ += 8 + length;
          return frame;
        }
      }
      if (test_support::LiveProviders::now() >= deadline_) {
        ADD_FAILURE() << "no further Ctrl frame before the deadline";
        return std::nullopt;
      }
      const auto pumped = session_->pump_once();
      EXPECT_TRUE(pumped);
      if (!pumped) return std::nullopt;
      std::array<std::byte, 1024> buffer{};
      const auto received = console_->receive(buffer);
      if (received) {
        ctrl_wire_.insert(
            ctrl_wire_.end(), buffer.begin(),
            buffer.begin() + static_cast<std::ptrdiff_t>(received.value()));
      } else {
        EXPECT_EQ(received.error(), NetError::would_block);
        if (received.error() != NetError::would_block) return std::nullopt;
      }
      test_support::LiveProviders::spin_pause();
    }
  }

  template <class Event>
  void pump_until_event(std::size_t count) {
    while (sink_.all<Event>().size() < count &&
           test_support::LiveProviders::now() < deadline_) {
      ASSERT_TRUE(session_->pump_once());
      test_support::LiveProviders::spin_pause();
    }
    ASSERT_EQ(sink_.all<Event>().size(), count);
  }

 private:
  bool keyboard_;
  std::string ctrl_response_;
  Target target_;
  ZeroCrypto crypto_;
  FixedClock clock_;
  CapturingDiagnostics diagnostics_;
  platform::DefaultPlatformFactory providers_{platform::PlatformProviders{
      .clock = &clock_, .crypto = &crypto_, .diagnostics = &diagnostics_}};
  std::unique_ptr<platform::TcpListener> listener_;
  std::unique_ptr<platform::UdpSocket> calibration_peer_;
  std::unique_ptr<platform::UdpSocket> active_peer_;
  std::unique_ptr<play::Link> session_;
  std::unique_ptr<platform::TcpSocket> console_;
  RecordingSink sink_;
  std::vector<std::byte> ctrl_wire_;
  std::size_t ctrl_offset_{};
  Deadline deadline_{test_support::LiveProviders::now() +
                     std::chrono::seconds{3}};
};

}  // namespace play_test::home_links
