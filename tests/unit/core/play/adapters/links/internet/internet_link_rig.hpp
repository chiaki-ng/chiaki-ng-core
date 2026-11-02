#pragma once

#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/platform/factory.hpp"
#include "libchiaking/platform/net/default.hpp"
#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"
#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "src/core/play/adapters/links/internet/internet_link.hpp"
#include "src/core/play/adapters/links/internet/internet_link_configuration.hpp"
#include "src/core/play/adapters/links/internet/internet_start.hpp"
#include "src/core/play/adapters/links/internet/wire/remote_ctrl.hpp"
#include "src/core/play/adapters/links/internet/wire/remote_ctrl_effects.hpp"
#include "tests/support/live_providers.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/unit/core/play/remote_route_network_fixture.hpp"
#include "tests/unit/core/play/remote_start_script.hpp"
#include "tests/unit/core/play/takion_defaults.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace play_test::internet_links {

using test_support::play::SignedInPlayer;

namespace internet = libchiaking::play::internet;
namespace platform = libchiaking::platform;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace rudp = libchiaking::net::transport::rudp;
namespace script = remote_start_script;
using Target = remote_play::RegistrationTarget;

class ScriptedInternetLinkTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!TakionDefaults::consoles())
      GTEST_SKIP() << "the build pins non-default console Takion versions";
  }
};

class NullHttp final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest&, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    return libchiaking::unexpected(error_);
  }

  void disconnect() noexcept { error_ = platform::HttpError::disconnected; }

 private:
  platform::HttpError error_{platform::HttpError::backend_failure};
};

class RefusedAccess final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest&, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls_;
    platform::HttpResponse response;
    response.status_code = 401;
    return response;
  }

  auto calls() const -> unsigned { return calls_; }

 private:
  unsigned calls_{};
};

struct Heard {
  std::vector<remote_play::event::SetupProgress> setup_progress;
  std::vector<bool> pin_prompts;
  unsigned failed_events{};
  unsigned stopped_events{};
  std::vector<std::string> nicknames;
  std::vector<bool> holepunch_finished;
  bool holepunch_before_info{};
  std::string registered_nickname;
  unsigned registered_events{};
  unsigned state_events{};
  bool registered_before_state{};
  Target registered_target{};
  std::byte registered_key_tail{};
  std::byte registered_mac_tail{};
  unsigned events{};
  unsigned info_events{};
  unsigned streaming_events{};
  bool info_before_streaming{};
  std::optional<remote_play::event::StreamInfoReady> last_info;
  remote_play::StreamEvent last{
      remote_play::event::StateChanged{remote_play::StreamState::idle}};
};

class RecordingSink final : public play::LinkEventSink {
 public:
  void on_event(remote_play::StreamEvent event) override {
    if (const auto* progress =
            std::get_if<remote_play::event::SetupProgress>(&event)) {
      heard_.setup_progress.push_back(*progress);
      return;
    }
    if (const auto* info =
            std::get_if<remote_play::event::StreamInfoReady>(&event)) {
      ++heard_.info_events;
      heard_.last_info = *info;
    }
    if (const auto* nickname =
            std::get_if<remote_play::event::NicknameReceived>(&event)) {
      heard_.nicknames.push_back(nickname->nickname);
      if (stop_on_nickname_ && stop_) stop_();
      return;
    }
    if (const auto* holepunch =
            std::get_if<remote_play::event::Holepunch>(&event)) {
      heard_.holepunch_finished.push_back(holepunch->finished);
      heard_.holepunch_before_info = heard_.info_events == 0;
      return;
    }
    if (const auto* registered =
            std::get_if<remote_play::event::Registered>(&event)) {
      ++heard_.registered_events;
      heard_.registered_nickname.assign(registered->console.nickname.data());
      heard_.registered_target = registered->console.target;
      heard_.registered_key_tail = registered->console.registration_key[15];
      heard_.registered_mac_tail = registered->console.mac[5];
      heard_.registered_before_state = heard_.state_events == 0;
      if (stop_on_registered_ && stop_) stop_();
      return;
    }
    if (std::holds_alternative<remote_play::event::Failed>(event))
      ++heard_.failed_events;
    if (const auto* pin = std::get_if<remote_play::event::PinRequired>(&event))
      heard_.pin_prompts.push_back(pin->incorrect);
    if (const auto* changed =
            std::get_if<remote_play::event::StateChanged>(&event)) {
      ++heard_.state_events;
      if (changed->state == remote_play::StreamState::stopped)
        ++heard_.stopped_events;
      if (changed->state == remote_play::StreamState::streaming) {
        ++heard_.streaming_events;
        heard_.info_before_streaming = heard_.info_events == 1;
      }
    }
    ++heard_.events;
    heard_.last = std::move(event);
  }

  void stop_inside(std::function<void()> stop, bool on_registered,
                   bool on_nickname) {
    stop_ = std::move(stop);
    stop_on_registered_ = on_registered;
    stop_on_nickname_ = on_nickname;
  }

  auto heard() const -> const Heard& { return heard_; }

 private:
  Heard heard_;
  std::function<void()> stop_;
  bool stop_on_registered_{};
  bool stop_on_nickname_{};
};

class RecordingDiagnostics final : public platform::DiagnosticsProvider {
 public:
  void write(platform::LogRecord record) noexcept override {
    const platform::Lock lock(mutex_);
    messages_.emplace_back(record.message);
  }

  auto count(std::string_view text) -> std::ptrdiff_t {
    const platform::Lock lock(mutex_);
    return std::ranges::count_if(messages_, [text](const std::string& message) {
      return message.find(text) != std::string::npos;
    });
  }

  auto contains(std::string_view text) -> bool { return count(text) > 0; }

  auto messages() -> std::vector<std::string> {
    const platform::Lock lock(mutex_);
    return messages_;
  }

 private:
  platform::Mutex mutex_{test_support::LiveProviders::threading()};
  std::vector<std::string> messages_;
};

class RecordingChannel final : public rudp::HandshakeChannel {
 public:
  auto send(std::span<const std::byte> bytes) -> libchiaking::Expected<
      std::size_t,
      libchiaking::net::transport::datagram::TransportError> override {
    history_.emplace_back(bytes.begin(), bytes.end());
    if (fail_send_)
      return libchiaking::unexpected(
          libchiaking::net::transport::datagram::TransportError::io_failure);
    sent_.assign(bytes.begin(), bytes.end());
    return bytes.size();
  }

  auto receive_for(std::chrono::microseconds, const platform::StopToken&)
      -> libchiaking::Expected<
          std::optional<libchiaking::net::transport::datagram::OwnedDatagram>,
          libchiaking::net::transport::datagram::TransportError> override {
    return std::optional<
        libchiaking::net::transport::datagram::OwnedDatagram>{};
  }

  void fail_sends(bool fail) { fail_send_ = fail; }
  void keep_first(std::size_t count) { history_.resize(count); }
  auto history() const -> const std::vector<std::vector<std::byte>>& {
    return history_;
  }
  auto sent() const -> const std::vector<std::byte>& { return sent_; }

 private:
  std::vector<std::byte> sent_;
  std::vector<std::vector<std::byte>> history_;
  bool fail_send_{};
};

class PairingsHeard final : public play::InternetStart::PairingListener {
 public:
  explicit PairingsHeard(bool refuse) : refuse_(refuse) {}

  auto on_paired(libchiaking::consoles::PairedConsole paired)
      -> libchiaking::Expected<void, Refusal> override {
    heard_.push_back(std::move(paired));
    if (refuse_) return libchiaking::unexpected(Refusal::unpaired_console);
    return {};
  }

  auto heard() const
      -> const std::vector<libchiaking::consoles::PairedConsole>& {
    return heard_;
  }

 private:
  std::vector<libchiaking::consoles::PairedConsole> heard_;
  bool refuse_{};
};

class FailSockets final : public platform::SocketProvider {
 public:
  auto create_udp_socket(const platform::IpAddress&)
      -> libchiaking::Expected<std::unique_ptr<platform::UdpSocket>,
                               platform::NetError> override {
    ++udp_calls_;
    return libchiaking::unexpected(platform::NetError::not_supported);
  }
  auto create_tcp_socket(const platform::IpAddress&)
      -> libchiaking::Expected<std::unique_ptr<platform::TcpSocket>,
                               platform::NetError> override {
    return libchiaking::unexpected(platform::NetError::not_supported);
  }
  auto create_tcp_listener(const platform::IpAddress&)
      -> libchiaking::Expected<std::unique_ptr<platform::TcpListener>,
                               platform::NetError> override {
    return libchiaking::unexpected(platform::NetError::not_supported);
  }

  auto udp_calls() const -> unsigned { return udp_calls_; }

 private:
  unsigned udp_calls_{};
};

class FailNetwork final : public platform::NetworkProvider {
 public:
  auto sockets() noexcept -> platform::SocketProvider& override {
    return sockets_;
  }
  auto pollers() noexcept -> platform::PollerProvider& override {
    return fallback_.pollers();
  }
  auto dns() noexcept -> platform::DnsProvider& override {
    return fallback_.dns();
  }
  auto interfaces() noexcept -> platform::InterfacesProvider& override {
    return fallback_.interfaces();
  }
  auto capabilities() const noexcept -> platform::NetworkCapabilities override {
    return fallback_.capabilities();
  }

  auto udp_calls() const -> unsigned { return sockets_.udp_calls(); }

 private:
  FailSockets sockets_;
  platform::DefaultNetworkProvider fallback_{
      test_support::LiveProviders::threading(),
      test_support::LiveProviders::steady()};
};

template <class Network = public_route_fixture::RouteNetwork>
class ScriptedRemote {
 public:
  explicit ScriptedRemote(std::vector<std::string_view> script)
      : websocket_{counts_, std::move(script)} {
    counts_->progress = http_.progress;
    if constexpr (std::same_as<Network, public_route_fixture::RouteNetwork>)
      network_.dns_.on_stun = [progress = http_.progress] {
        ++progress->stun_bindings;
      };
  }

  ScriptedRemote(const ScriptedRemote&) = delete;
  auto operator=(const ScriptedRemote&) -> ScriptedRemote& = delete;

  static auto configuration(Target target = Target::ps5_1)
      -> play::InternetLinkConfiguration {
    play::InternetLinkConfiguration config;
    config.user = SignedInPlayer::fresh_for_remote_play();
    config.host = "127.0.0.1";
    config.console.target = target;
    return config;
  }

  void answer_control_route()
    requires std::same_as<Network, public_route_fixture::RouteNetwork>
  {
    auto& replies = network_.sockets_;
    replies.respond_to_init = true;
    replies.respond_to_cookie = true;
    replies.respond_to_registration = true;
    replies.respond_to_ctrl = true;
  }

  auto open(play::InternetLinkConfiguration config,
            play::InternetStart::PairingListener* pairings = nullptr)
      -> play::Link* {
    return adopt(
        play::InternetLinks::playing(std::move(config), factory_, pairings));
  }

  auto open_pairing(play::InternetLinkConfiguration config) -> play::Link* {
    return adopt(
        play::InternetLinks::pairing_only(std::move(config), factory_));
  }

  void close_link() { link_.reset(); }

  auto threading() -> script::ScriptThreading& { return threading_; }
  auto clock() -> script::ScriptClock& { return clock_; }
  auto random() -> script::ScriptRandom& { return random_; }
  auto crypto() -> script::RejectCrypto& { return crypto_; }
  auto http() -> script::ScriptHttp& { return http_; }
  auto counts() -> script::ScriptCounts& { return *counts_; }
  auto network() -> Network& { return network_; }
  auto diagnostics() -> RecordingDiagnostics& { return diagnostics_; }
  auto sink() -> RecordingSink& { return sink_; }
  auto heard() const -> const Heard& { return sink_.heard(); }

  auto sockets() -> public_route_fixture::RouteSockets&
    requires std::same_as<Network, public_route_fixture::RouteNetwork>
  {
    return network_.sockets_;
  }

  auto requests(std::string_view kind) const -> std::ptrdiff_t {
    return std::ranges::count(http_.order, kind);
  }

 private:
  auto adopt(libchiaking::Expected<std::unique_ptr<play::Link>,
                                   remote_play::StreamError>
                 made) -> play::Link* {
    if (!made) return nullptr;
    link_ = std::move(made.value());
    link_->set_event_sink(sink_);
    return link_.get();
  }

  script::ScriptThreading threading_;
  script::ScriptClock clock_;
  script::ScriptRandom random_;
  script::RejectCrypto crypto_;
  script::ScriptHttp http_;
  std::shared_ptr<script::ScriptCounts> counts_{
      std::make_shared<script::ScriptCounts>()};
  script::ScriptWebSocket websocket_;
  RecordingDiagnostics diagnostics_;
  Network network_;
  platform::DefaultPlatformFactory factory_{
      platform::PlatformProviders{.threading = &threading_,
                                  .clock = &clock_,
                                  .random = &random_,
                                  .network = &network_,
                                  .crypto = &crypto_,
                                  .http = &http_,
                                  .websocket = &websocket_,
                                  .diagnostics = &diagnostics_}};
  RecordingSink sink_;
  std::unique_ptr<play::Link> link_;
};

class RemoteCtrlRig {
 public:
  static constexpr std::uint64_t attempt = 7;

  explicit RemoteCtrlRig(internet::remote_ctrl::StartupResult startup = {})
      : session_{internet::remote_ctrl::Session::create(std::move(startup),
                                                        attempt)} {}

  RemoteCtrlRig(const RemoteCtrlRig&) = delete;
  auto operator=(const RemoteCtrlRig&) -> RemoteCtrlRig& = delete;

  static auto session_id_bytes() -> std::array<std::byte, 25> {
    std::array<std::byte, 25> bytes;
    bytes.fill(std::byte{'A'});
    bytes[0] = std::byte{0x4a};
    return bytes;
  }

  auto created() const -> bool { return static_cast<bool>(session_); }
  auto session() -> internet::remote_ctrl::Session& {
    return *session_.value();
  }
  auto channel() -> RecordingChannel& { return channel_; }
  auto crypto() -> script::RejectCrypto& { return crypto_; }

  auto accept(remote_play::CtrlMessage message) -> bool {
    remote_play::RudpCtrlMessageResult result{};
    result.message = std::move(message);
    return session().inbox().accept({std::move(result), attempt, false});
  }

  auto deliver(std::uint16_t type, std::span<const std::byte> payload) -> bool {
    return accept(remote_play::CtrlCodec::decode_message(type, payload));
  }

  struct Stepped {
    libchiaking::Expected<internet::remote_ctrl::EffectProgress,
                          internet::remote_ctrl::EffectError>
        progress;
    bool pin_notified;
  };

  auto step(bool pin_notified = false, Target target = Target::ps5_1)
      -> Stepped {
    auto progress = internet::remote_ctrl::Effects::step(
        session(), channel_, crypto_, target, pin_notified);
    const bool notified =
        pin_notified ||
        (progress &&
         progress.value() == internet::remote_ctrl::EffectProgress::pin_required);
    return Stepped{std::move(progress), notified};
  }

  void fall_back_to_session_id(Target target) {
    auto id = remote_play::SessionId::parse(session_id_bytes());
    ASSERT_TRUE(id);
    ASSERT_TRUE(deliver(0x33, {}));
    const auto fallback = step(false, target);
    ASSERT_TRUE(fallback.progress);
    EXPECT_EQ(fallback.progress.value(),
              internet::remote_ctrl::EffectProgress::fallback_required);
    EXPECT_TRUE(session().inbox().publish_fallback(attempt, std::move(*id)));
  }

  auto recorded_frames(std::size_t first = 0)
      -> std::vector<remote_play::CtrlOutboundFrame> {
    std::vector<remote_play::CtrlOutboundFrame> result;
    const auto& history = channel_.history();
    for (std::size_t index = first; index < history.size(); ++index) {
      SCOPED_TRACE(index);
      const auto decoded = decode(history[index]);
      if (!decoded) continue;
      result.push_back(*decoded);
    }
    return result;
  }

  void expect_sent(std::uint16_t first_counter,
                   std::span<const std::uint16_t> types,
                   std::span<const std::vector<std::byte>> payloads) {
    const auto& history = channel_.history();
    ASSERT_LE(history.size(), types.size());
    ASSERT_LE(history.size(), payloads.size());
    for (std::size_t index = 0; index < history.size(); ++index) {
      SCOPED_TRACE(index);
      const auto counter = counter_of(history[index]);
      ASSERT_TRUE(counter);
      EXPECT_EQ(*counter, static_cast<std::uint16_t>(first_counter + index));
      const auto decoded = decode(history[index]);
      ASSERT_TRUE(decoded);
      EXPECT_EQ(decoded->type, types[index]);
      EXPECT_EQ(decoded->payload, payloads[index]);
    }
  }

 private:
  static auto counter_of(const std::vector<std::byte>& wire)
      -> std::optional<std::uint16_t> {
    const auto frames = rudp::FrameCodec::decode(wire);
    if (!frames || frames.value().size() != 1) return std::nullopt;
    const auto& frame = frames.value().front();
    if (frame.payload.size() < 2) return std::nullopt;
    return static_cast<std::uint16_t>(
        (std::to_integer<std::uint16_t>(frame.payload[0]) << 8U) |
        std::to_integer<std::uint16_t>(frame.payload[1]));
  }

  static auto decode(const std::vector<std::byte>& wire)
      -> std::optional<remote_play::CtrlOutboundFrame> {
    const auto frames = rudp::FrameCodec::decode(wire);
    EXPECT_TRUE(frames);
    if (!frames) return std::nullopt;
    EXPECT_EQ(frames.value().size(), 1U);
    if (frames.value().size() != 1) return std::nullopt;
    const auto& frame = frames.value().front();
    EXPECT_EQ(frame.type, 0x0230);
    EXPECT_GE(frame.payload.size(), 2U);
    if (frame.payload.size() < 2) return std::nullopt;
    const auto decoded = remote_play::CtrlCodec::decode_frame(
        std::span<const std::byte>(frame.payload).subspan(2));
    EXPECT_TRUE(decoded);
    if (!decoded) return std::nullopt;
    return remote_play::CtrlOutboundFrame{
        decoded.value().type,
        {decoded.value().payload.begin(), decoded.value().payload.end()}};
  }

  libchiaking::Expected<std::unique_ptr<internet::remote_ctrl::Session>,
                        internet::remote_ctrl::SessionError>
      session_;
  RecordingChannel channel_;
  script::RejectCrypto crypto_;
};

}  // namespace play_test::internet_links
