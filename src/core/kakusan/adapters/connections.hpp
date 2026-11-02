/// @file
/// @brief Declares Connections, the source of the network links a shared
/// session or guest visit opens to the other side.
#pragma once

#include "libchiaking/platform/crypto/provider.hpp"
#include "libchiaking/platform/dtls/provider.hpp"
#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/kakusan/peer_link.hpp"
#include "src/core/net/kakusan/udp_transport_factory.hpp"

#include <optional>
#include <span>
#include <string>

namespace libchiaking::kakusan {

/// @brief Owns or borrows the `net::kakusan` transport factory that opens each
/// direct link between a host and a guest.
///
/// On a real device it builds a UdpTransportFactory: UDP sockets from the
/// platform network, encrypted with the platform's secure datagram provider,
/// with addresses discovered through the configured STUN servers. Tests and
/// embedders may lend a factory of their own instead.
///
/// Made by Surroundings::connections() when a session is shared or a visit
/// begins, so it captures the STUN servers and port guessing in force at that
/// moment; it is owned by that session or visit and lives as long as it.
/// Not copyable. factory() may be called from any thread; the factory's own
/// rules apply to what is done with it.
class Connections {
 public:
  /// @brief Builds and owns a UDP transport factory.
  ///
  /// Resolves the STUN server names through the platform's DNS, so it blocks
  /// for as long as the lookups take. Entries that do not parse or resolve
  /// are skipped, as are entries beyond the ICE limit; IPv6 is not offered.
  ///
  /// @param crypto Crypto for the transports; must outlive this object.
  /// @param secure_datagram Opens the encryption layer of each link; must
  ///     outlive this object.
  /// @param network Sockets and DNS; must outlive this object.
  /// @param random Randomness for the transports; must outlive this object.
  /// @param stun_servers The STUN servers, as `host` or `host:port`; may be
  ///     empty, when only local addresses are offered.
  /// @param guessing When guessed public ports are offered to the peer.
  Connections(platform::CryptoProvider& crypto,
              platform::SecureDatagramProvider& secure_datagram,
              platform::NetworkProvider& network,
              platform::SecureRandomProvider& random,
              std::span<const std::string> stun_servers,
              net::nat::ice::PortGuessing guessing =
                  net::nat::ice::PortGuessing::automatic);
  /// @brief Uses a factory owned by someone else.
  ///
  /// @param borrowed The factory; must outlive this object.
  explicit Connections(net::kakusan::SpokeTransportFactory& borrowed) noexcept;

  /// @brief Not copyable; the factory is owned or borrowed once.
  Connections(const Connections&) = delete;
  /// @brief Not copy-assignable; the factory is owned or borrowed once.
  auto operator=(const Connections&) -> Connections& = delete;

  /// @brief The factory that opens links.
  ///
  /// @return The owned UDP factory, or the borrowed one; valid while this
  ///     object lives.
  auto factory() noexcept -> net::kakusan::SpokeTransportFactory& {
    return *used_;
  }

 private:
  /// @brief The owned UDP factory; empty when a factory is borrowed.
  std::optional<net::kakusan::UdpTransportFactory> udp_;
  /// @brief The factory in use: the owned one or the borrowed one, never
  /// null.
  net::kakusan::SpokeTransportFactory* used_;
};

}  // namespace libchiaking::kakusan
