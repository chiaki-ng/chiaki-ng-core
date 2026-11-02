#pragma once

#include "src/core/net/kakusan/protocol_error.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "libchiaking/platform/net/types.hpp"
#include "libchiaking/testing/split_mix.hpp"
#include "src/core/kakusan/model/player_events.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/peer_link.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/net/nat/ice/agent.hpp"
#include "src/core/net/nat/stun/message.hpp"
#include "src/core/utils/memory/constant_time.hpp"
#include "src/core/utils/timing/deadline.hpp"
#include "tests/support/kakusan/messages.hpp"
#include "tests/support/net/addresses.hpp"
#include "tests/support/random.hpp"
#include "tests/support/setup.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace test_support::net {

enum class NatKind : std::uint8_t {
  none,
  full_cone,
  address_restricted,
  port_restricted,
  symmetric,
};

struct Delivery {
  std::size_t socket{};
  libchiaking::platform::SocketEndpoint source;
  std::vector<std::byte> bytes;
};

struct TrafficStats {
  std::size_t check_requests{};
  std::uint64_t min_gap_us{std::numeric_limits<std::uint64_t>::max()};
  std::optional<std::uint64_t> last_request_us;
  std::set<std::pair<std::size_t, std::string>> checked;
};

class Network {
 public:
  Network(libchiaking::platform::CryptoProvider& crypto, std::uint64_t seed)
      : crypto_(crypto), random_(seed) {}

  std::function<void(std::vector<std::byte>&)> tamper;

  auto add_nat(NatKind kind,
               const libchiaking::platform::IpAddress& public_address)
      -> std::size_t {
    nats_.push_back(Nat{kind,
                        public_address,
                        static_cast<std::uint16_t>(40000 + nats_.size() * 1000),
                        {}});
    return nats_.size() - 1;
  }

  auto add_socket(const libchiaking::platform::SocketEndpoint& local,
                  std::optional<std::size_t> nat, std::size_t owner)
      -> std::size_t {
    sockets_.push_back(Socket{local, nat, owner});
    if (stats_.size() <= owner) stats_.resize(owner + 1);
    return sockets_.size() - 1;
  }

  void add_stun_server(const libchiaking::platform::SocketEndpoint& endpoint,
                       bool responsive) {
    servers_.push_back(Server{endpoint, responsive});
  }

  void set_latency(std::uint64_t base_us, std::uint64_t jitter_us) noexcept {
    latency_us_ = std::max<std::uint64_t>(base_us, 1);
    jitter_us_ = jitter_us;
  }

  void set_loss_permille(std::uint64_t loss) noexcept { loss_permille_ = loss; }
  void set_binding_timeout(std::uint64_t timeout_us) noexcept {
    binding_timeout_us_ = timeout_us;
  }
  void set_cut(bool cut) noexcept { cut_ = cut; }

  void rebind(std::size_t nat) {
    nats_[nat].mappings.clear();
    nats_[nat].next_port =
        static_cast<std::uint16_t>(nats_[nat].next_port + 500);
  }

  auto socket(std::size_t handle) const
      -> const libchiaking::platform::SocketEndpoint& {
    return sockets_[handle].local;
  }

  auto owner(std::size_t handle) const -> std::size_t {
    return sockets_[handle].owner;
  }

  auto stats(std::size_t owner) const -> const TrafficStats& {
    return stats_[owner];
  }

  auto packets_to(const libchiaking::platform::SocketEndpoint& endpoint) const
      -> std::size_t {
    const auto found = addressed_.find(Addresses::endpoint_key(endpoint));
    return found == addressed_.end() ? 0 : found->second;
  }

  [[nodiscard]] auto external(
      std::size_t handle,
      const libchiaking::platform::SocketEndpoint& remote) const
      -> std::optional<libchiaking::platform::SocketEndpoint> {
    const auto& socket = sockets_[handle];
    if (!socket.nat) return socket.local;
    const auto& nat = nats_[*socket.nat];
    for (const auto& mapping : nat.mappings) {
      if (mapping.internal == socket.local &&
          (nat.kind != NatKind::symmetric || mapping.destination == remote))
        return libchiaking::platform::SocketEndpoint{nat.public_address,
                                                     mapping.external_port};
    }
    return std::nullopt;
  }

  void send(std::size_t handle,
            const libchiaking::platform::SocketEndpoint& remote,
            std::vector<std::byte> bytes, std::uint64_t now_us) {
    const auto& socket = sockets_[handle];
    Setup::require(socket.local.address.family() == remote.address.family(),
                   "a socket sends to its own address family");
    ++addressed_[Addresses::endpoint_key(remote)];
    record(socket.owner, handle, remote, bytes, now_us);
    if (tamper && !is_server(remote)) tamper(bytes);
    auto source = socket.local;
    if (socket.nat)
      source = translate(*socket.nat, socket.local, remote, now_us);
    schedule(source, remote, std::move(bytes), now_us);
  }

  void inject(const libchiaking::platform::SocketEndpoint& from,
              const libchiaking::platform::SocketEndpoint& to,
              std::vector<std::byte> bytes, std::uint64_t now_us) {
    schedule(from, to, std::move(bytes), now_us);
  }

  auto next_delivery_us() const noexcept -> std::optional<std::uint64_t> {
    if (in_flight_.empty()) return std::nullopt;
    return std::min_element(in_flight_.begin(), in_flight_.end(), earlier)
        ->deliver_us;
  }

  void deliver_due(std::uint64_t now_us, std::vector<Delivery>& deliveries) {
    while (!in_flight_.empty()) {
      const auto next =
          std::min_element(in_flight_.begin(), in_flight_.end(), earlier);
      if (next->deliver_us > now_us) return;
      auto packet = std::move(*next);
      in_flight_.erase(next);
      route(std::move(packet), now_us, deliveries);
    }
  }

 private:
  struct Socket {
    libchiaking::platform::SocketEndpoint local;
    std::optional<std::size_t> nat;
    std::size_t owner{};
  };

  struct Mapping {
    libchiaking::platform::SocketEndpoint internal;
    libchiaking::platform::SocketEndpoint destination;
    std::uint16_t external_port{};
    std::uint64_t last_us{};
    std::vector<std::pair<libchiaking::platform::SocketEndpoint, std::uint64_t>>
        permissions;
  };

  struct Nat {
    NatKind kind{NatKind::none};
    libchiaking::platform::IpAddress public_address;
    std::uint16_t next_port{};
    std::vector<Mapping> mappings;
  };

  struct Server {
    libchiaking::platform::SocketEndpoint endpoint;
    bool responsive{};
  };

  struct InFlight {
    std::uint64_t deliver_us{};
    std::uint64_t sequence{};
    libchiaking::platform::SocketEndpoint from;
    libchiaking::platform::SocketEndpoint to;
    std::vector<std::byte> bytes;
  };

  static auto earlier(const InFlight& left, const InFlight& right) noexcept
      -> bool {
    return left.deliver_us != right.deliver_us
               ? left.deliver_us < right.deliver_us
               : left.sequence < right.sequence;
  }

  auto is_server(const libchiaking::platform::SocketEndpoint& endpoint) const
      -> bool {
    return std::any_of(
        servers_.begin(), servers_.end(),
        [&](const Server& server) { return server.endpoint == endpoint; });
  }

  void record(std::size_t owner, std::size_t handle,
              const libchiaking::platform::SocketEndpoint& remote,
              std::span<const std::byte> bytes, std::uint64_t now_us) {
    if (is_server(remote)) return;
    const auto decoded =
        libchiaking::net::nat::stun::ReceivedMessage::decode(bytes);
    if (!decoded || decoded.value().message.kind !=
                        libchiaking::net::nat::stun::MessageClass::request)
      return;
    auto& stats = stats_[owner];
    ++stats.check_requests;
    if (stats.last_request_us)
      stats.min_gap_us =
          std::min(stats.min_gap_us, now_us - *stats.last_request_us);
    stats.last_request_us = now_us;
    stats.checked.insert({handle, Addresses::endpoint_key(remote)});
  }

  auto translate(std::size_t index,
                 const libchiaking::platform::SocketEndpoint& internal,
                 const libchiaking::platform::SocketEndpoint& remote,
                 std::uint64_t now_us)
      -> libchiaking::platform::SocketEndpoint {
    auto& nat = nats_[index];
    if (nat.kind == NatKind::none) return internal;
    const auto stale = [&](const Mapping& mapping) {
      return !live(mapping.last_us, now_us);
    };
    nat.mappings.erase(
        std::remove_if(nat.mappings.begin(), nat.mappings.end(), stale),
        nat.mappings.end());
    Mapping* mapping = nullptr;
    for (auto& candidate : nat.mappings) {
      if (candidate.internal == internal &&
          (nat.kind != NatKind::symmetric || candidate.destination == remote)) {
        mapping = &candidate;
        break;
      }
    }
    if (mapping == nullptr) {
      nat.mappings.push_back(
          Mapping{internal, remote, nat.next_port++, now_us, {}});
      mapping = &nat.mappings.back();
    }
    mapping->last_us = now_us;
    const auto permission =
        std::find_if(mapping->permissions.begin(), mapping->permissions.end(),
                     [&](const auto& entry) { return entry.first == remote; });
    if (permission == mapping->permissions.end())
      mapping->permissions.emplace_back(remote, now_us);
    else
      permission->second = now_us;
    return libchiaking::platform::SocketEndpoint{nat.public_address,
                                                 mapping->external_port};
  }

  void schedule(const libchiaking::platform::SocketEndpoint& from,
                const libchiaking::platform::SocketEndpoint& to,
                std::vector<std::byte> bytes, std::uint64_t now_us) {
    if (cut_ || random_.below(1000) < loss_permille_) return;
    const auto delay = latency_us_ + random_.below(jitter_us_ + 1);
    in_flight_.push_back(
        InFlight{now_us + delay, sequence_++, from, to, std::move(bytes)});
  }

  void answer_stun(const InFlight& packet, std::uint64_t now_us) {
    const auto decoded =
        libchiaking::net::nat::stun::ReceivedMessage::decode(packet.bytes);
    if (!decoded || decoded.value().message.kind !=
                        libchiaking::net::nat::stun::MessageClass::request)
      return;
    libchiaking::net::nat::stun::Message response;
    response.kind = libchiaking::net::nat::stun::MessageClass::success;
    response.transaction = decoded.value().message.transaction;
    response.xor_mapped = packet.from;
    schedule(packet.to, packet.from,
             Setup::value(response.encode(
                              crypto_, {},
                              libchiaking::net::nat::stun::Fingerprint::present),
                          "the binding response encodes"),
             now_us);
  }

  auto live(std::uint64_t last_us, std::uint64_t now_us) const noexcept
      -> bool {
    return now_us < last_us + binding_timeout_us_;
  }

  auto admits(NatKind kind, const Mapping& mapping,
              const libchiaking::platform::SocketEndpoint& from,
              std::uint64_t now_us) const -> bool {
    if (!live(mapping.last_us, now_us)) return false;
    const auto permitted = [&](bool match_port) {
      return std::any_of(
          mapping.permissions.begin(), mapping.permissions.end(),
          [&](const auto& entry) {
            return entry.first.address == from.address &&
                   (!match_port || entry.first.port == from.port) &&
                   live(entry.second, now_us);
          });
    };
    switch (kind) {
      case NatKind::none:
      case NatKind::full_cone:
        return true;
      case NatKind::address_restricted:
        return permitted(false);
      case NatKind::port_restricted:
        return permitted(true);
      case NatKind::symmetric:
        return mapping.destination == from && permitted(true);
    }
    return false;
  }

  void route(InFlight packet, std::uint64_t now_us,
             std::vector<Delivery>& deliveries) {
    for (const auto& server : servers_) {
      if (server.endpoint == packet.to) {
        if (server.responsive) answer_stun(packet, now_us);
        return;
      }
    }
    for (const auto& nat : nats_) {
      if (nat.kind == NatKind::none || nat.public_address != packet.to.address)
        continue;
      for (const auto& mapping : nat.mappings) {
        if (mapping.external_port != packet.to.port ||
            !admits(nat.kind, mapping, packet.from, now_us))
          continue;
        for (std::size_t handle = 0; handle < sockets_.size(); ++handle) {
          if (sockets_[handle].nat && &nats_[*sockets_[handle].nat] == &nat &&
              sockets_[handle].local == mapping.internal) {
            deliveries.push_back(
                Delivery{handle, packet.from, std::move(packet.bytes)});
            return;
          }
        }
      }
      return;
    }
    for (std::size_t handle = 0; handle < sockets_.size(); ++handle) {
      const auto& socket = sockets_[handle];
      const auto direct =
          !socket.nat || nats_[*socket.nat].kind == NatKind::none;
      if (direct && socket.local == packet.to) {
        deliveries.push_back(
            Delivery{handle, packet.from, std::move(packet.bytes)});
        return;
      }
    }
  }

  libchiaking::platform::CryptoProvider& crypto_;
  libchiaking::testing::SplitMix64 random_;
  std::vector<Socket> sockets_;
  std::vector<Nat> nats_;
  std::vector<Server> servers_;
  std::vector<InFlight> in_flight_;
  std::vector<TrafficStats> stats_;
  std::map<std::string, std::size_t> addressed_;
  std::uint64_t latency_us_{20'000};
  std::uint64_t jitter_us_{};
  std::uint64_t loss_permille_{};
  std::uint64_t binding_timeout_us_{30'000'000};
  std::uint64_t sequence_{};
  bool cut_{};
};

struct PeerSocket {
  libchiaking::platform::SocketEndpoint local;
  std::optional<std::size_t> nat;
  std::uint16_t preference{};
};

struct Recorded {
  std::uint64_t at_us{};
  libchiaking::net::nat::ice::Event event;
};

class Simulation {
 public:
  Simulation(libchiaking::platform::CryptoProvider& crypto, std::uint64_t seed)
      : crypto_(crypto), network_(crypto, seed ^ 0x5eedULL), seed_(seed) {}

  static constexpr auto socket_id(std::size_t index) noexcept
      -> libchiaking::net::nat::ice::SocketId {
    return static_cast<libchiaking::net::nat::ice::SocketId>(100 + index);
  }

  auto network() noexcept -> Network& { return network_; }
  auto now() const noexcept -> std::uint64_t { return now_us_; }

  auto add_peer(
      libchiaking::net::kakusan::TransportRole role,
      const std::vector<PeerSocket>& sockets,
      const std::vector<libchiaking::platform::SocketEndpoint>& servers,
      libchiaking::net::nat::ice::PortGuessing guessing =
          libchiaking::net::nat::ice::PortGuessing::automatic) -> std::size_t {
    const auto index = peers_.size();
    Peer peer;
    peer.entropy =
        std::make_unique<SeededRandom>(seed_ * 1'000'003ULL + index + 1);
    libchiaking::net::nat::ice::AgentConfig config;
    config.role = role;
    config.stun_servers = servers;
    config.guessing = guessing;
    for (std::size_t socket = 0; socket < sockets.size(); ++socket) {
      peer.sockets.push_back(network_.add_socket(sockets[socket].local,
                                                 sockets[socket].nat, index));
      config.bases.push_back(libchiaking::net::nat::ice::Base{
          socket_id(socket), sockets[socket].local,
          sockets[socket].preference});
    }
    peer.agent = Setup::value(libchiaking::net::nat::ice::Agent::create(
                                  crypto_, *peer.entropy, std::move(config),
                                  now_us_),
                              "the agent is created");
    peers_.push_back(std::move(peer));
    drain(index);
    return index;
  }

  auto agent(std::size_t peer) -> libchiaking::net::nat::ice::Agent& {
    return *peers_[peer].agent;
  }

  auto events(std::size_t peer) const -> const std::vector<Recorded>& {
    return peers_[peer].events;
  }

  auto socket_handle(std::size_t peer,
                     libchiaking::net::nat::ice::SocketId id) const
      -> std::size_t {
    return peers_[peer].sockets[id - socket_id(0)];
  }

  auto polls() const noexcept -> std::size_t { return polls_; }

  template <class Done>
  auto run_until(std::uint64_t limit_us, Done done) -> bool {
    std::size_t spins = 0;
    while (!done()) {
      auto next = network_.next_delivery_us();
      for (const auto& peer : peers_)
        next = libchiaking::timing::Deadline::earliest(
            next, peer.agent->next_deadline_us());
      if (!next || *next > limit_us) {
        now_us_ = std::max(now_us_, limit_us);
        return done();
      }
      if (*next <= now_us_) {
        ++spins;
        Setup::require(spins < 1000, "the simulation makes progress");
      } else {
        spins = 0;
      }
      now_us_ = std::max(now_us_, *next);
      step();
    }
    return true;
  }

  void run_for(std::uint64_t duration_us) {
    run_until(now_us_ + duration_us, [] { return false; });
  }

 private:
  struct Peer {
    std::unique_ptr<SeededRandom> entropy;
    std::unique_ptr<libchiaking::net::nat::ice::Agent> agent;
    std::vector<std::size_t> sockets;
    std::vector<Recorded> events;
  };

  void step() {
    std::vector<Delivery> deliveries;
    network_.deliver_due(now_us_, deliveries);
    for (auto& delivery : deliveries) {
      const auto owner = network_.owner(delivery.socket);
      auto& peer = peers_[owner];
      const auto position =
          std::find(peer.sockets.begin(), peer.sockets.end(), delivery.socket);
      const auto id =
          socket_id(static_cast<std::size_t>(position - peer.sockets.begin()));
      std::vector<libchiaking::net::nat::ice::Outgoing> outgoing;
      peer.agent->receive(id, delivery.source, delivery.bytes, now_us_,
                          outgoing);
      send(owner, outgoing);
      drain(owner);
    }
    for (std::size_t index = 0; index < peers_.size(); ++index) {
      std::vector<libchiaking::net::nat::ice::Outgoing> outgoing;
      peers_[index].agent->poll(now_us_, outgoing);
      ++polls_;
      send(index, outgoing);
      drain(index);
      const auto deadline = peers_[index].agent->next_deadline_us();
      Setup::require(!deadline || *deadline > now_us_,
                     "the next deadline lies ahead");
    }
  }

  void send(std::size_t peer,
            std::vector<libchiaking::net::nat::ice::Outgoing>& outgoing) {
    for (auto& packet : outgoing) {
      Setup::require(packet.socket >= socket_id(0) &&
                         packet.socket < socket_id(peers_[peer].sockets.size()),
                     "the agent sends from one of its sockets");
      network_.send(socket_handle(peer, packet.socket), packet.remote,
                    std::move(packet.bytes), now_us_);
    }
  }

  void drain(std::size_t peer) {
    while (auto event = peers_[peer].agent->take_event())
      peers_[peer].events.push_back(Recorded{now_us_, std::move(*event)});
  }

  libchiaking::platform::CryptoProvider& crypto_;
  Network network_;
  std::uint64_t seed_;
  std::uint64_t now_us_{};
  std::size_t polls_{};
  std::vector<Peer> peers_;
};

}  // namespace test_support::net
