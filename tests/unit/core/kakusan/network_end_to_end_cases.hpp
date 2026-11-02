#pragma once

#include "src/core/net/kakusan/protocol_error.hpp"
#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/platform/factory.hpp"
#include "src/core/kakusan/adapters/controller_map.hpp"
#include "src/core/kakusan/adapters/guest_visit/guest_visit_core.hpp"
#include "src/core/kakusan/adapters/shared_session/shared_session_core.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/net/kakusan/udp_transport_factory.hpp"
#include "src/core/utils/memory/constant_time.hpp"
#include "src/core/utils/timing/deadline.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/messages.hpp"
#include "tests/support/kakusan/secure_datagrams.hpp"
#include "tests/support/net/ice_network.hpp"
#include "tests/support/net/simulated_network.hpp"
#include "tests/support/play/source_stream.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/kakusan/forwarded_media.hpp"
#include "tests/unit/core/kakusan/identities.hpp"
#include "tests/unit/core/kakusan/source_description.hpp"
#include "tests/unit/core/kakusan/source_vectors.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kakusan_test {

using test_support::net::Addresses;
using test_support::play::SourceStream;
namespace {

namespace wire = libchiaking::net::kakusan;
namespace platform = libchiaking::platform;
using libchiaking::controllers::ControllerButton;
using libchiaking::controllers::SeatNumber;
using libchiaking::kakusan::Clock;
using libchiaking::kakusan::ControllerMap;
using libchiaking::kakusan::GuestVisit;
using libchiaking::kakusan::Progress;
using libchiaking::kakusan::SharedSession;
using libchiaking::kakusan::SharingRules;
using libchiaking::kakusan::SourceRumble;

class NetworkRig : public ::testing::Test {
 public:
  static constexpr std::uint64_t start_wall = 1'791'115'200;
  static constexpr std::uint64_t max_step_us = 10'000;
  static constexpr std::uint64_t loss_permille = 10;
  static constexpr std::uint64_t latency_us = 4'000;
  static constexpr std::uint64_t jitter_us = 2'000;

  static auto crypto() -> test_support::SoftwareCrypto& {
    static test_support::SoftwareCrypto shared;
    return shared;
  }

  static auto steady() -> const platform::SteadyClock& {
    static const platform::SteadyClock clock;
    return clock;
  }

  static auto stun_server() -> platform::SocketEndpoint {
    return Addresses::at(Addresses::ipv4(192, 0, 2, 200), 3478);
  }

  static auto transport_config() -> wire::UdpTransportConfig {
    wire::UdpTransportConfig config;
    config.stun_servers.push_back(stun_server());
    return config;
  }

  static auto identity(std::uint8_t tag)
      -> libchiaking::kakusan::DeviceIdentity {
    return Identities::tagged(crypto(), tag);
  }

  static auto text_of(const wire::Code& code) -> std::string {
    return std::string{code.begin(), code.end()};
  }

  static auto pressed() -> wire::InputState {
    wire::InputState state;
    state.buttons = static_cast<std::uint32_t>(wire::Button::south);
    state.left_x = 12000;
    state.right_y = -32767;
    state.left_trigger = 65535;
    return state;
  }

  static auto east() -> wire::InputState {
    wire::InputState state;
    state.buttons = static_cast<std::uint32_t>(wire::Button::east);
    return state;
  }

  static auto button(ControllerButton value) -> std::uint32_t {
    return static_cast<std::uint32_t>(value);
  }
};

class Handshake {
 public:
  bool nominated{};
  bool settled{};
};

class ObservedTransport final : public wire::SpokeTransport {
 public:
  ObservedTransport(std::unique_ptr<wire::SpokeTransport> inner,
                    std::shared_ptr<Handshake> handshake)
      : inner_(std::move(inner)), handshake_(std::move(handshake)) {}

  ~ObservedTransport() override { handshake_->settled = true; }

  [[nodiscard]] auto gather(std::uint64_t now_us,
                            std::optional<wire::IceCredentials> credentials)
      -> wire::Result<void> override {
    return inner_->gather(now_us, std::move(credentials));
  }

  [[nodiscard]] auto connect(const wire::TransportDescription& remote,
                             std::uint64_t now_us)
      -> wire::Result<void> override {
    return inner_->connect(remote, now_us);
  }

  [[nodiscard]] auto pump(std::uint64_t now_us,
                          std::vector<wire::TransportEvent>& events)
      -> wire::Result<void> override {
    const auto first = events.size();
    auto pumped = inner_->pump(now_us, events);
    for (auto index = first; index < events.size(); ++index) {
      if (std::holds_alternative<wire::transport_event::Nominated>(
              events[index]))
        handshake_->nominated = true;
      else if (std::holds_alternative<wire::transport_event::Established>(
                   events[index]) ||
               std::holds_alternative<wire::transport_event::Failed>(
                   events[index]))
        handshake_->settled = true;
    }
    return pumped;
  }

  [[nodiscard]] auto next_deadline_us() const noexcept
      -> std::optional<std::uint64_t> override {
    return inner_->next_deadline_us();
  }

  [[nodiscard]] auto send_record(std::span<const std::byte> record)
      -> wire::Result<void> override {
    return inner_->send_record(record);
  }

  [[nodiscard]] auto send_datagram(std::span<const std::byte> datagram)
      -> wire::Result<void> override {
    return inner_->send_datagram(datagram);
  }

  [[nodiscard]] auto export_secret(std::string_view label,
                                   std::span<const std::byte> context,
                                   std::span<std::byte> output)
      -> wire::Result<void> override {
    return inner_->export_secret(label, context, output);
  }

  void close() noexcept override {
    inner_->close();
    handshake_->settled = true;
  }

 private:
  std::unique_ptr<wire::SpokeTransport> inner_;
  std::shared_ptr<Handshake> handshake_;
};

class ObservedFactory final : public wire::SpokeTransportFactory {
 public:
  ObservedFactory(wire::UdpTransportDependencies dependencies,
                  wire::UdpTransportConfig config)
      : udp_(std::move(dependencies), std::move(config)) {}

  [[nodiscard]] auto create(wire::TransportRole role)
      -> wire::Result<std::unique_ptr<wire::SpokeTransport>> override {
    auto created = udp_.create(role);
    if (!created) return wire::fail(created.error());
    auto handshake = std::make_shared<Handshake>();
    handshakes_.push_back(handshake);
    ++created_;
    std::unique_ptr<wire::SpokeTransport> transport =
        std::make_unique<ObservedTransport>(std::move(created).value(),
                                            handshake);
    return transport;
  }

  auto handshaking() const -> bool {
    return std::ranges::any_of(handshakes_, [](const auto& handshake) {
      return handshake->nominated && !handshake->settled;
    });
  }

  auto created() const noexcept -> std::size_t { return created_; }

 private:
  wire::UdpTransportFactory udp_;
  std::vector<std::shared_ptr<Handshake>> handshakes_;
  std::size_t created_{};
};

template <class Provider>
class NetworkPlayer {
 public:
  using Datagrams = test_support::kakusan::SecureDatagrams<Provider>;

  NetworkPlayer(test_support::net::SimulatedNetwork& network,
                const platform::IpAddress& public_address,
                const platform::IpAddress& private_address, std::uint8_t tag,
                const std::uint64_t& now)
      : machine(
            network, {Addresses::interface_on("en0", private_address)},
            network.network().add_nat(
                test_support::net::NatKind::port_restricted, public_address)),
        entropy(0x5000 + tag),
        transport_entropy(0x9000 + tag),
        factory(wire::UdpTransportDependencies{machine, NetworkRig::crypto(),
                                               transport_entropy,
                                               Datagrams::factory()},
                NetworkRig::transport_config()),
        clock(now),
        session(GuestVisit::Core::create(
                    GuestVisit::Core::Dependencies{platform, entropy, factory,
                                                   media},
                    NetworkRig::identity(tag))
                    .value()) {}

  void drain(std::uint64_t now) {
    while (auto output = session->take_output()) {
      if (auto* ready = std::get_if<GuestVisit::Core::ReplyReady>(&*output))
        reply = std::string{ready->block.view()};
      else if (const auto* shown =
                   std::get_if<GuestVisit::Core::CodeReady>(&*output))
        code = NetworkRig::text_of(shown->code);
      else if (const auto* state =
                   std::get_if<GuestVisit::Core::State>(&*output))
        states.push_back(*state);
      else if (auto* stream =
                   std::get_if<GuestVisit::Core::StreamReady>(&*output)) {
        EXPECT_FALSE(media.descriptions.empty());
        if (!media.descriptions.empty()) info = media.descriptions.back();
        fps = stream->video_fps;
      } else if (const auto* requested =
                     std::get_if<GuestVisit::Core::ControllerRequested>(
                         &*output)) {
        controller = *requested;
        EXPECT_TRUE(session->controller_ready(now));
      } else if (const auto* rumble =
                     std::get_if<GuestVisit::Core::Rumble>(&*output))
        rumbles.push_back(*rumble);
      else if (std::holds_alternative<GuestVisit::Core::StreamStopped>(*output))
        ++stopped;
    }
  }

  test_support::net::SimulatedMachine machine;
  test_support::SeededRandom entropy;
  test_support::SeededRandom transport_entropy;
  ObservedFactory factory;
  ForwardedMedia media;
  TrackedClock clock;
  platform::DefaultPlatformFactory platform{platform::PlatformProviders{
      .clock = &clock, .crypto = &NetworkRig::crypto()}};
  std::unique_ptr<GuestVisit::Core> session;
  wire::InvitationId id{};
  std::string reply;
  std::string code;
  std::optional<libchiaking::media::StreamDescription> info;
  std::uint32_t fps{};
  std::optional<GuestVisit::Core::ControllerRequested> controller;
  std::vector<GuestVisit::Core::State> states;
  std::vector<GuestVisit::Core::Rumble> rumbles;
  std::size_t stopped{};
};

template <class Provider>
class NetworkWorld {
 public:
  using Datagrams = test_support::kakusan::SecureDatagrams<Provider>;
  using Player = NetworkPlayer<Provider>;

  struct Input : libchiaking::controllers::ControllerState {
    std::uint8_t controller_id{};
  };

  NetworkWorld() {
    network.network().set_latency(NetworkRig::latency_us,
                                  NetworkRig::jitter_us);
    network.network().add_stun_server(NetworkRig::stun_server(), true);
    network.network().set_loss_permille(NetworkRig::loss_permille);
    network.advance_to(now);
  }

  auto clock() const -> Clock {
    return Clock{now, NetworkRig::start_wall + now / 1'000'000};
  }

  void start_source() {
    host->source_keys(SourceStream::takion_version,
                      SourceStream::receive_keys());
    EXPECT_TRUE(host->source_started(SourceStream::description(),
                                     SourceStream::video_fps, now));
  }

  void drain_host() {
    while (auto output = host->take_output()) {
      if (auto* ready =
              std::get_if<SharedSession::Core::InvitationReady>(&*output))
        invitations.push_back(std::move(*ready));
      else if (const auto* reply =
                   std::get_if<SharedSession::Core::GuestIdentified>(&*output))
        bound.push_back(*reply);
      else if (const auto* code =
                   std::get_if<SharedSession::Core::CodeReady>(&*output))
        codes.push_back(
            SharedSession::Core::CodeReady{code->guest, code->code.clone()});
      else if (const auto* state =
                   std::get_if<SharedSession::Core::GuestState>(&*output))
        states.push_back(*state);
      else if (const auto* join =
                   std::get_if<SharedSession::Core::JoinRequested>(&*output)) {
        joins.push_back(*join);
        pads[join->guest] = next_pad;
        EXPECT_TRUE(host->source_joined(
            join->seat, SeatNumber::create(next_pad++).value(), now));
      } else if (const auto* removal =
                     std::get_if<SharedSession::Core::RemovalRequested>(
                         &*output))
        removals.push_back(*removal);
      else if (const auto* input =
                   std::get_if<SharedSession::Core::ControllerInput>(&*output))
        inputs.push_back(Input{input->state, pads.at(input->guest)});
      else if (std::holds_alternative<SharedSession::Core::IdrRequested>(
                   *output))
        ++idr_requests;
      else if (const auto* changed =
                   std::get_if<SharedSession::Core::SeatsChanged>(&*output))
        seats.push_back(changed->seats);
    }
  }

  auto handshaking() const -> bool {
    return host_factory.handshaking() ||
           std::ranges::any_of(players, [](const auto& player) {
             return player->factory.handshaking();
           });
  }

  void step() {
    auto next = libchiaking::timing::Deadline::earliest(
        network.next_delivery_us(), host->next_deadline_us());
    for (const auto& player : players)
      next = libchiaking::timing::Deadline::earliest(
          next, player->session->next_deadline_us());
    auto target = now + NetworkRig::max_step_us;
    if (next) target = std::min(target, std::max(*next, now));
    if (network.backlog() != 0) target = now;
    if (target > now && handshaking())
      static_cast<void>(sleeper.sleep_for(std::chrono::microseconds{
          static_cast<std::chrono::microseconds::rep>(target - now)}));
    now = target;
    network.advance_to(now);
    host->pump(clock());
    drain_host();
    for (auto& player : players) {
      player->session->pump(clock());
      player->drain(now);
    }
    drain_host();
  }

  void run_for(std::uint64_t duration_us) {
    const auto until_us = now + duration_us;
    for (std::size_t index = 0; index < 1'000'000 && now < until_us; ++index)
      step();
  }

  template <class Done>
  void until(Done done, std::uint64_t limit_us = 60'000'000) {
    const auto deadline = now + limit_us;
    for (std::size_t index = 0; index < 1'000'000 && !done() && now < deadline;
         ++index)
      step();
    EXPECT_TRUE(done());
  }

  auto host_state(const wire::InvitationId& guest) const -> Progress {
    auto found = Progress::waiting;
    for (const auto& state : states) {
      if (state.guest == guest) found = state.state;
    }
    return found;
  }

  auto host_code(const wire::InvitationId& guest) const -> std::string {
    for (const auto& code : codes) {
      if (code.guest == guest) return NetworkRig::text_of(code.code);
    }
    return {};
  }

  auto join(std::uint8_t tag) -> Player& {
    const auto issued = invitations.size();
    const auto guest = host->invite(clock());
    EXPECT_TRUE(guest);
    until([&] { return invitations.size() > issued; });
    EXPECT_EQ(invitations.back().guest, guest.value());

    const auto index = static_cast<std::uint8_t>(players.size());
    players.push_back(std::make_unique<Player>(
        network,
        Addresses::ipv4(198, 51, 100, static_cast<std::uint8_t>(20 + index)),
        Addresses::ipv4(10, 0, index, 20), tag, now));
    auto& player = *players.back();
    player.id = guest.value();
    const auto preview = GuestVisit::Core::preview(
        NetworkRig::crypto(), invitations.back().block.view(),
        clock().wall_seconds);
    EXPECT_TRUE(preview);
    const auto summary = player.session->accept_invitation(
        invitations.back().block.view(), clock());
    EXPECT_TRUE(summary);
    if (summary && preview) {
      EXPECT_EQ(summary.value().host_hint, preview.value().host_hint);
    }
    until([&] { return !player.reply.empty(); });

    const auto imported = host->import_reply(player.reply, clock());
    EXPECT_TRUE(imported);
    if (imported) {
      EXPECT_EQ(imported.value(), guest.value());
    }
    EXPECT_TRUE(host->approve(guest.value(), now));
    until(
        [&] { return !player.code.empty() && !host_code(player.id).empty(); });

    EXPECT_TRUE(host->enter_code(player.id, player.code, now));
    EXPECT_TRUE(player.session->enter_code(host_code(player.id), now));
    until([&] {
      return player.session->state() == Progress::playing &&
             host_state(player.id) == Progress::playing;
    });
    run_for(200'000);
    EXPECT_EQ(player.factory.created(), 1U);
    EXPECT_GE(host_machine.open_sockets(), 1U);
    EXPECT_EQ(player.machine.open_sockets(), 1U);
    return player;
  }

  void forward_source() {
    network.network().set_loss_permille(0);
    network.network().set_latency(NetworkRig::latency_us, 0);
    for (std::size_t index = 0; index < SourceStream::vectors.size(); ++index) {
      const auto bytes = SourceStream::datagram(index);
      host->source_datagram(SourceDescriptions::describe(
          SourceStream::takion_version, SourceStream::vectors[index].media,
          SourceStream::vectors[index].key_position, bytes));
      step();
    }
    run_for(500'000);
    network.network().set_latency(NetworkRig::latency_us,
                                  NetworkRig::jitter_us);
    network.network().set_loss_permille(NetworkRig::loss_permille);
  }

  platform::Sleeper sleeper{test_support::process_threading(),
                            NetworkRig::steady()};
  test_support::net::SimulatedNetwork network{NetworkRig::crypto(), 0x6b616b75};
  test_support::net::SimulatedMachine host_machine{
      network,
      {Addresses::interface_on("en0", Addresses::ipv4(192, 168, 1, 10))},
      network.network().add_nat(test_support::net::NatKind::port_restricted,
                                Addresses::ipv4(203, 0, 113, 10))};
  test_support::SeededRandom host_entropy{0x1001};
  test_support::SeededRandom host_transport_entropy{0x2001};
  ObservedFactory host_factory{
      wire::UdpTransportDependencies{host_machine, NetworkRig::crypto(),
                                     host_transport_entropy,
                                     Datagrams::factory()},
      NetworkRig::transport_config()};
  std::unique_ptr<SharedSession::Core> host{
      SharedSession::Core::create(
          SharedSession::Core::Dependencies{NetworkRig::crypto(), host_entropy,
                                            host_factory,
                                            test_support::process_threading()},
          NetworkRig::identity(0x10),
          *SharingRules::create(4, 1, wire::button_mask))
          .value()};
  std::vector<std::unique_ptr<Player>> players;
  std::uint64_t now{2'000'000};

  std::vector<SharedSession::Core::InvitationReady> invitations;
  std::vector<SharedSession::Core::GuestIdentified> bound;
  std::vector<SharedSession::Core::CodeReady> codes;
  std::vector<SharedSession::Core::GuestState> states;
  std::vector<SharedSession::Core::JoinRequested> joins;
  std::vector<SharedSession::Core::RemovalRequested> removals;
  std::vector<Input> inputs;
  std::map<wire::InvitationId, std::uint8_t> pads;
  std::vector<libchiaking::kakusan::SeatSummary> seats;
  std::size_t idr_requests{};
  std::uint8_t next_pad{1};
};

template <class Provider>
class NetworkEndToEnd : public NetworkRig {
 protected:
  using World = NetworkWorld<Provider>;
  using Player = NetworkPlayer<Provider>;

  void SetUp() override {
    world.start_source();
    first = &world.join(0x40);
    second = &world.join(0x70);
  }

  static void expect_decoded(const Player& player) {
    const auto video = player.media.video;
    const auto audio = player.media.audio;
    ASSERT_EQ(video.size(), SourceStream::video_frames.size());
    ASSERT_EQ(audio.size(), SourceStream::audio_frames.size());
    for (std::size_t index = 0; index < video.size(); ++index)
      EXPECT_EQ(video[index],
                test_support::Bytes::unhex(SourceStream::video_frames[index]));
    for (std::size_t index = 0; index < audio.size(); ++index)
      EXPECT_EQ(audio[index],
                test_support::Bytes::unhex(SourceStream::audio_frames[index]));
  }

  auto has_input(std::uint8_t controller, std::uint32_t buttons) const -> bool {
    return std::ranges::any_of(world.inputs, [=](const auto& state) {
      return state.controller_id == controller && state.buttons == buttons;
    });
  }

  void leave_first() {
    first->session->leave(world.now);
    world.until([&] {
      return world.host_state(first->id) == Progress::gone &&
             first->session->state() == Progress::gone;
    });
  }

  World world;
  Player* first{};
  Player* second{};
};

TYPED_TEST_SUITE_P(NetworkEndToEnd);

TYPED_TEST_P(NetworkEndToEnd, SeatsTwoGuestsWithTheStream) {
  auto& world = this->world;
  auto& first = *this->first;
  auto& second = *this->second;
  EXPECT_EQ(world.bound.size(), 2U);
  ASSERT_EQ(world.joins.size(), 2U);
  ASSERT_TRUE(first.controller);
  ASSERT_TRUE(second.controller);
  EXPECT_EQ(first.controller->seat, world.joins.front().seat);
  EXPECT_EQ(second.controller->seat, world.joins.back().seat);
  EXPECT_NE(first.controller->seat.seat, second.controller->seat.seat);
  EXPECT_EQ(first.controller->capacity, 4U);
  EXPECT_EQ(first.controller->local_seats, 1U);
  for (const auto* player : {&first, &second}) {
    ASSERT_TRUE(player->info);
    EXPECT_EQ(player->fps, SourceStream::video_fps);
    EXPECT_EQ(player->info->profiles, SourceStream::description().profiles);
    EXPECT_EQ(player->info->audio_header,
              SourceStream::description().audio_header);
  }
  EXPECT_EQ(world.host->seats().remote_seats, 2U);
  EXPECT_GE(world.idr_requests, 1U);
}

TYPED_TEST_P(NetworkEndToEnd, DeliversTheSourceToBothGuests) {
  this->world.forward_source();
  this->expect_decoded(*this->first);
  this->expect_decoded(*this->second);
}

TYPED_TEST_P(NetworkEndToEnd, MapsTheFirstGuestsInputOntoTheFirstSeat) {
  auto& world = this->world;
  world.forward_source();
  EXPECT_TRUE(
      this->first->session->set_controller(NetworkRig::pressed(), world.now));
  const auto pressed_on_first = [&world] {
    return std::ranges::find_if(world.inputs, [](const auto& state) {
      return state.controller_id == 1 && state.buttons != 0;
    });
  };
  world.until([&] { return pressed_on_first() != world.inputs.end(); });
  const auto expected =
      ControllerMap::to_source_state(NetworkRig::pressed(), wire::button_mask);
  ASSERT_TRUE(expected);
  const auto found = pressed_on_first();
  ASSERT_NE(found, world.inputs.end());
  const auto applied = *found;
  EXPECT_EQ(applied.buttons, expected.value().buttons);
  EXPECT_EQ(applied.buttons, NetworkRig::button(ControllerButton::cross));
  EXPECT_EQ(applied.left_x, 12000);
  EXPECT_EQ(applied.right_y, -32767);
  EXPECT_EQ(applied.l2, 255U);
}

TYPED_TEST_P(NetworkEndToEnd, KeepsEachGuestsInputOnItsOwnSeat) {
  auto& world = this->world;
  world.forward_source();
  EXPECT_TRUE(
      this->first->session->set_controller(NetworkRig::pressed(), world.now));
  world.until([&] {
    return this->has_input(1, NetworkRig::button(ControllerButton::cross));
  });
  EXPECT_TRUE(
      this->second->session->set_controller(NetworkRig::east(), world.now));
  world.until([&] {
    return this->has_input(2, NetworkRig::button(ControllerButton::moon));
  });
  EXPECT_FALSE(this->has_input(1, NetworkRig::button(ControllerButton::moon)));
  EXPECT_FALSE(this->has_input(2, NetworkRig::button(ControllerButton::cross)));
}

TYPED_TEST_P(NetworkEndToEnd, SendsRumbleOnlyToTheGuestOfItsSeat) {
  auto& world = this->world;
  auto& first = *this->first;
  auto& second = *this->second;
  world.forward_source();
  world.host->source_rumble(SourceRumble{SeatNumber::create(2).value(), 7, 9},
                            world.now);
  world.host->source_rumble(SourceRumble{SeatNumber::create(3).value(), 1, 1},
                            world.now);
  world.until([&] { return !second.rumbles.empty(); });
  ASSERT_FALSE(second.rumbles.empty());
  EXPECT_EQ(second.rumbles.front().low_frequency, 7U);
  EXPECT_EQ(second.rumbles.front().high_frequency, 9U);
  EXPECT_TRUE(first.rumbles.empty());
  world.host->source_rumble(
      SourceRumble{SeatNumber::create(1).value(), 40000, 900}, world.now);
  world.until([&] { return !first.rumbles.empty(); });
  ASSERT_FALSE(first.rumbles.empty());
  EXPECT_EQ(first.rumbles.front().low_frequency, 40000U);
  EXPECT_EQ(first.rumbles.front().high_frequency, 900U);
}

TYPED_TEST_P(NetworkEndToEnd, ReleasesTheSeatOfAGuestThatLeaves) {
  auto& world = this->world;
  auto& first = *this->first;
  world.forward_source();
  this->leave_first();
  EXPECT_EQ(first.stopped, 1U);
  world.until([&] { return !first.session->next_deadline_us(); });
  ASSERT_EQ(world.removals.size(), 1U);
  EXPECT_EQ(world.removals.front().console_seat, SeatNumber::create(1));
  const auto first_closed =
      std::find_if(world.states.rbegin(), world.states.rend(),
                   [&](const auto& state) { return state.guest == first.id; });
  ASSERT_NE(first_closed, world.states.rend());
  EXPECT_TRUE(first_closed->by_peer);
  EXPECT_EQ(first_closed->reason, wire::control::Reason::user);
  world.until([&] { return first.machine.open_sockets() == 0; });
  EXPECT_EQ(this->second->session->state(), Progress::playing);
  EXPECT_EQ(world.host->seats().remote_seats, 1U);
}

TYPED_TEST_P(NetworkEndToEnd, RemovesAThirdGuestOnTheFreedSeat) {
  auto& world = this->world;
  world.forward_source();
  this->leave_first();
  world.until([&] { return this->first->machine.open_sockets() == 0; });

  auto& third = world.join(0x90);
  EXPECT_EQ(world.host->seats().remote_seats, 2U);
  EXPECT_TRUE(world.host->remove(third.id, world.now));
  world.until([&] { return third.session->state() == Progress::gone; });
  ASSERT_FALSE(third.states.empty());
  EXPECT_TRUE(third.states.back().by_peer);
  EXPECT_EQ(third.states.back().reason, wire::control::Reason::user);
  EXPECT_EQ(third.stopped, 1U);
  world.until([&] { return third.machine.open_sockets() == 0; });
  EXPECT_EQ(this->second->session->state(), Progress::playing);
  EXPECT_EQ(world.host->seats().remote_seats, 1U);
}

TYPED_TEST_P(NetworkEndToEnd,
             EndsTheLastGuestAndClosesEverySocketWithTheSource) {
  auto& world = this->world;
  auto& second = *this->second;
  world.forward_source();
  this->leave_first();
  world.until([&] { return this->first->machine.open_sockets() == 0; });

  world.host->source_ended(world.now);
  world.until([&] { return second.session->state() == Progress::gone; });
  ASSERT_FALSE(second.states.empty());
  EXPECT_TRUE(second.states.back().by_peer);
  EXPECT_EQ(second.states.back().reason, wire::control::Reason::source);
  EXPECT_EQ(second.stopped, 1U);
  world.until([&] { return !world.host->next_deadline_us(); });
  EXPECT_EQ(world.host->seats().remote_seats, 0U);
  EXPECT_FALSE(world.host->invite(world.clock()));
  world.until([&] {
    return world.host_machine.open_sockets() == 0 &&
           second.machine.open_sockets() == 0;
  });
}

REGISTER_TYPED_TEST_SUITE_P(NetworkEndToEnd, SeatsTwoGuestsWithTheStream,
                            DeliversTheSourceToBothGuests,
                            MapsTheFirstGuestsInputOntoTheFirstSeat,
                            KeepsEachGuestsInputOnItsOwnSeat,
                            SendsRumbleOnlyToTheGuestOfItsSeat,
                            ReleasesTheSeatOfAGuestThatLeaves,
                            RemovesAThirdGuestOnTheFreedSeat,
                            EndsTheLastGuestAndClosesEverySocketWithTheSource);

}  // namespace
}  // namespace kakusan_test
