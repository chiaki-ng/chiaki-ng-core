#pragma once

#include "src/core/net/kakusan/protocol_error.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/peer_link.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/utils/memory/constant_time.hpp"
#include "src/core/utils/timing/deadline.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/kakusan/messages.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace test_support::kakusan {

using libchiaking::net::kakusan::Digest256;
using libchiaking::net::kakusan::Error;
using libchiaking::net::kakusan::Result;

enum class Lane : std::uint8_t {
  record,
  datagram,
};

struct LoopbackLink {
  struct Packet {
    std::uint64_t due_us{};
    Lane lane{Lane::record};
    std::vector<std::byte> bytes;
  };
  struct Side {
    std::optional<libchiaking::net::kakusan::TransportDescription> local;
    std::optional<libchiaking::net::kakusan::TransportDescription> remote;
    std::deque<libchiaking::net::kakusan::TransportEvent> events;
    std::deque<Packet> inbox;
    bool established{};
    bool closed{};
  };

  explicit LoopbackLink(libchiaking::platform::CryptoProvider& provider)
      : crypto(provider) {}

  libchiaking::platform::CryptoProvider& crypto;
  std::array<Side, 2> sides{};
  std::uint64_t delay_us{};
  std::function<bool(libchiaking::net::kakusan::TransportRole, Lane,
                     const std::vector<std::byte>&)>
      drop;
  std::array<std::byte, 32> secret{};
  std::array<bool, 2> attached{};
  bool reject_certificates{};

  static auto index(libchiaking::net::kakusan::TransportRole role) noexcept
      -> std::size_t {
    return role == libchiaking::net::kakusan::TransportRole::host ? 0 : 1;
  }

  void settle() {
    auto& host = sides[0];
    auto& guest = sides[1];
    if (host.established || !host.remote || !guest.remote || !host.local ||
        !guest.local)
      return;
    const bool pinned =
        host.remote->certificate_digest == guest.local->certificate_digest &&
        guest.remote->certificate_digest == host.local->certificate_digest &&
        !reject_certificates;
    for (auto& side : sides) {
      side.events.push_back(
          libchiaking::net::kakusan::transport_event::Nominated{});
      if (pinned) {
        side.established = true;
        side.events.push_back(
            libchiaking::net::kakusan::transport_event::Established{});
      } else {
        side.events.push_back(
            libchiaking::net::kakusan::transport_event::Failed{
                Error::protocol});
      }
    }
  }
};

class LoopbackTransport final
    : public libchiaking::net::kakusan::SpokeTransport {
 public:
  LoopbackTransport(std::shared_ptr<LoopbackLink> link,
                    libchiaking::net::kakusan::TransportRole role)
      : link_(std::move(link)), role_(role) {}

  [[nodiscard]] auto gather(
      std::uint64_t,
      std::optional<libchiaking::net::kakusan::IceCredentials> credentials)
      -> Result<void> override {
    const bool host = role_ == libchiaking::net::kakusan::TransportRole::host;
    const auto address = libchiaking::platform::parse_ip_address(
        host ? "192.0.2.10" : "198.51.100.20");
    if (!address) {
      ADD_FAILURE() << "the loopback address does not parse";
      return libchiaking::unexpected(
          libchiaking::net::kakusan::Error::internal);
    }
    auto fixed = libchiaking::net::kakusan::IceCredentials::make(
        libchiaking::SecretString{host ? "1111111111111111"
                                       : "2222222222222222"},
        libchiaking::SecretString{std::string(48, host ? '3' : '4')});
    if (!fixed) {
      ADD_FAILURE() << "the loopback credentials are invalid";
      return libchiaking::unexpected(
          libchiaking::net::kakusan::Error::internal);
    }
    auto ice = libchiaking::net::kakusan::IceParameters::make(
        credentials ? credentials->clone() : std::move(fixed.value()),
        {libchiaking::net::kakusan::Candidate{
            "1",
            2130706431,
            libchiaking::platform::SocketEndpoint{address.value(), 50000},
            libchiaking::net::kakusan::CandidateType::host,
            {}}},
        0);
    if (!ice) {
      ADD_FAILURE() << "the loopback candidate is invalid";
      return libchiaking::unexpected(
          libchiaking::net::kakusan::Error::internal);
    }
    libchiaking::net::kakusan::TransportDescription local{
        std::move(ice.value()), {}};
    local.certificate_digest.fill(host ? std::byte{0xa1} : std::byte{0xb2});
    side().local = local.clone();
    side().events.push_back(
        libchiaking::net::kakusan::transport_event::Gathered{std::move(local)});
    return {};
  }

  [[nodiscard]] auto connect(
      const libchiaking::net::kakusan::TransportDescription& remote,
      std::uint64_t) -> Result<void> override {
    if (!side().local || side().remote)
      return libchiaking::net::kakusan::fail(Error::internal);
    side().remote = remote.clone();
    link_->settle();
    return {};
  }

  [[nodiscard]] auto pump(
      std::uint64_t now_us,
      std::vector<libchiaking::net::kakusan::TransportEvent>& events)
      -> Result<void> override {
    auto& self = side();
    while (!self.events.empty()) {
      events.push_back(std::move(self.events.front()));
      self.events.pop_front();
    }
    while (!self.inbox.empty() && self.inbox.front().due_us <= now_us) {
      auto packet = std::move(self.inbox.front());
      self.inbox.pop_front();
      if (self.closed) continue;
      if (packet.lane == Lane::record) {
        auto owned = libchiaking::SecretBytes::from_bytes(packet.bytes);
        if (!owned) return libchiaking::net::kakusan::fail(Error::internal);
        events.push_back(libchiaking::net::kakusan::transport_event::Record{
            std::move(owned).value()});
      } else
        events.push_back(libchiaking::net::kakusan::transport_event::Datagram{
            std::move(packet.bytes)});
    }
    now_us_ = now_us;
    return {};
  }

  auto next_deadline_us() const noexcept
      -> std::optional<std::uint64_t> override {
    const auto& self = link_->sides[LoopbackLink::index(role_)];
    if (!self.events.empty()) return now_us_;
    if (self.inbox.empty()) return std::nullopt;
    return self.inbox.front().due_us;
  }

  [[nodiscard]] auto send_record(std::span<const std::byte> record)
      -> Result<void> override {
    return send(Lane::record, record);
  }

  [[nodiscard]] auto send_datagram(std::span<const std::byte> datagram)
      -> Result<void> override {
    return send(Lane::datagram, datagram);
  }

  [[nodiscard]] auto export_secret(std::string_view label,
                                   std::span<const std::byte> context,
                                   std::span<std::byte> output)
      -> Result<void> override {
    if (!side().established || output.size() > 32)
      return libchiaking::net::kakusan::fail(Error::internal);
    std::vector<std::byte> input;
    Messages::append_label(input, label);
    Bytes::append(input, context);
    const auto digest = libchiaking::net::kakusan::Hash::hmac_sha256(
        link_->crypto, link_->secret, input);
    if (!digest) return libchiaking::net::kakusan::fail(digest.error());
    for (std::size_t index = 0; index < output.size(); ++index)
      output[index] = digest.value()[index];
    return {};
  }

  void close() noexcept override {
    side().closed = true;
    side().established = false;
  }

  std::size_t records_sent{};
  std::size_t datagrams_sent{};

 private:
  auto side() -> LoopbackLink::Side& {
    return link_->sides[LoopbackLink::index(role_)];
  }

  auto peer() -> LoopbackLink::Side& {
    return link_->sides[1 - LoopbackLink::index(role_)];
  }

  [[nodiscard]] auto send(Lane lane, std::span<const std::byte> bytes)
      -> Result<void> {
    if (!side().established || side().closed)
      return libchiaking::net::kakusan::fail(Error::no_path);
    if (bytes.empty() || bytes.size() > 1200)
      return libchiaking::net::kakusan::fail(Error::internal);
    (lane == Lane::record ? records_sent : datagrams_sent) += 1;
    std::vector<std::byte> copy(bytes.begin(), bytes.end());
    if (link_->drop && link_->drop(role_, lane, copy)) return {};
    if (!peer().closed)
      peer().inbox.push_back(LoopbackLink::Packet{now_us_ + link_->delay_us,
                                                  lane, std::move(copy)});
    return {};
  }

  std::shared_ptr<LoopbackLink> link_;
  libchiaking::net::kakusan::TransportRole role_;
  std::uint64_t now_us_{};
};

struct LoopbackExchange {
  explicit LoopbackExchange(libchiaking::platform::CryptoProvider& provider)
      : crypto(provider) {}

  libchiaking::platform::CryptoProvider& crypto;
  std::vector<std::shared_ptr<LoopbackLink>> links;

  [[nodiscard]] auto attach(libchiaking::net::kakusan::TransportRole role)
      -> std::shared_ptr<LoopbackLink> {
    const auto side = LoopbackLink::index(role);
    for (auto& link : links) {
      if (!link->attached[side]) {
        link->attached[side] = true;
        return link;
      }
    }
    links.push_back(std::make_shared<LoopbackLink>(crypto));
    links.back()->attached[side] = true;
    return links.back();
  }
};

class LoopbackFactory final
    : public libchiaking::net::kakusan::SpokeTransportFactory {
 public:
  explicit LoopbackFactory(std::shared_ptr<LoopbackExchange> exchange)
      : exchange_(std::move(exchange)) {}

  [[nodiscard]] auto create(libchiaking::net::kakusan::TransportRole role)
      -> Result<
          std::unique_ptr<libchiaking::net::kakusan::SpokeTransport>> override {
    if (fail_next) {
      fail_next = false;
      return libchiaking::net::kakusan::fail(Error::no_path);
    }
    std::unique_ptr<libchiaking::net::kakusan::SpokeTransport> transport =
        std::make_unique<LoopbackTransport>(exchange_->attach(role), role);
    return transport;
  }

  bool fail_next{};

 private:
  std::shared_ptr<LoopbackExchange> exchange_;
};

}  // namespace test_support::kakusan
