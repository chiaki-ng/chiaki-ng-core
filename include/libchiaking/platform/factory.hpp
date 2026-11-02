/// @file
/// @brief Declares PlatformFactory, the single object through which a host
/// hands its providers to the library.
#pragma once

#include <libchiaking/platform/crypto/provider.hpp>
#include <libchiaking/platform/diagnostics/provider.hpp>
#include <libchiaking/platform/dtls/provider.hpp>
#include <libchiaking/platform/error.hpp>
#include <libchiaking/platform/gateway/provider.hpp>
#include <libchiaking/platform/http/provider.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/random_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>
#include <libchiaking/platform/websocket/provider.hpp>

#include <cstdint>

namespace libchiaking::platform {

/// @brief Bit flags naming the ports a PlatformFactory supplies.
///
/// Combine with the bitwise operators declared below. A factory sets a flag
/// exactly when the matching accessor returns a usable provider rather than a
/// stand-in; PlatformFactory::supplies reads them.
enum class PlatformCapability : std::uint32_t {
  /// @brief No port is supplied.
  none = 0,
  /// @brief PlatformFactory::threading is usable. Required.
  threading = 1u << 0,
  /// @brief PlatformFactory::clock is usable. Required.
  monotonic_clock = 1u << 1,
  /// @brief PlatformFactory::random is usable. Required.
  secure_random = 1u << 2,
  /// @brief PlatformFactory::network is usable.
  network = 1u << 3,
  /// @brief PlatformFactory::crypto returns a host-supplied provider.
  crypto = 1u << 4,
  /// @brief PlatformFactory::http returns a host-supplied provider.
  http = 1u << 6,
  /// @brief PlatformFactory::diagnostics returns a host-supplied provider.
  diagnostics = 1u << 7,
  /// @brief The network provider can set the don't-fragment flag on IPv4
  /// sockets.
  socket_dontfrag_v4 = 1u << 8,
  /// @brief The network provider can set the don't-fragment flag on IPv6
  /// sockets.
  socket_dontfrag_v6 = 1u << 9,
  /// @brief PlatformFactory::websocket returns a host-supplied provider.
  websocket = 1u << 10,
  /// @brief PlatformFactory::gateway returns a host-supplied provider.
  gateway_mapping = 1u << 11,
  /// @brief PlatformFactory::secure_datagram returns a host-supplied
  /// provider.
  secure_datagram = 1u << 12,
};

/// @brief Unions two capability sets.
///
/// @param left One set.
/// @param right The other set.
/// @return Every flag present in either set.
constexpr PlatformCapability operator|(PlatformCapability left,
                                       PlatformCapability right) noexcept {
  return static_cast<PlatformCapability>(static_cast<std::uint32_t>(left) |
                                         static_cast<std::uint32_t>(right));
}

/// @brief Intersects two capability sets.
///
/// @param left One set.
/// @param right The other set.
/// @return Only the flags present in both sets.
constexpr PlatformCapability operator&(PlatformCapability left,
                                       PlatformCapability right) noexcept {
  return static_cast<PlatformCapability>(static_cast<std::uint32_t>(left) &
                                         static_cast<std::uint32_t>(right));
}

/// @brief Complements a capability set, including bits no flag names.
///
/// @param value Set to complement.
/// @return Every bit that is clear in @p value.
constexpr PlatformCapability operator~(PlatformCapability value) noexcept {
  return static_cast<PlatformCapability>(~static_cast<std::uint32_t>(value));
}

/// @brief Adds the flags in @p right to @p left.
///
/// @param left Set to extend in place.
/// @param right Flags to add.
/// @return @p left, after the change.
constexpr PlatformCapability& operator|=(PlatformCapability& left,
                                         PlatformCapability right) noexcept {
  left = left | right;
  return left;
}

/// @brief Keeps only the flags of @p left that are also in @p right.
///
/// @param left Set to narrow in place.
/// @param right Flags to keep.
/// @return @p left, after the change.
constexpr PlatformCapability& operator&=(PlatformCapability& left,
                                         PlatformCapability right) noexcept {
  left = left & right;
  return left;
}

/// @brief Version of the provider interfaces in this tree.
///
/// A PlatformFactory reports it in PlatformInfo::abi_version; the conformance
/// checks reject a factory built against any other value.
inline constexpr std::uint32_t platform_abi_version = 9;

/// @brief Self-description a PlatformFactory returns from
/// PlatformFactory::info.
struct PlatformInfo {
  /// @brief Short label for the platform, for example "libchiaki-default".
  /// Must not be null and must outlive the factory.
  const char* name;
  /// @brief The platform_abi_version the factory was built against.
  std::uint32_t abi_version;
  /// @brief The ports the factory supplies.
  PlatformCapability capabilities;
};

/// @brief The set of providers a host gives the library.
///
/// Implemented by the host frontend, or by DefaultPlatformFactory when the
/// stock providers are enough. The library borrows the factory and every
/// provider it returns; they must outlive every library object constructed from
/// the factory. The library keeps the references the accessors return.
///
/// Every accessor returns a provider. The runtime and network ports are
/// mandatory; the others are optional, and when the host does not supply one
/// the accessor returns a stand-in that refuses every request or discards
/// every line. supplies() tells the two apart. Every accessor is called
/// from whichever library thread needs the port, so each must be safe to call
/// concurrently and must return the same object every time. None of them may
/// block.
/// @ingroup api_platform
class PlatformFactory {
 public:
  /// @brief Destroys the factory. The library never deletes a factory it was
  /// given.
  virtual ~PlatformFactory() = default;
  /// @brief Describes the factory.
  ///
  /// @return The name, ABI version and capability flags. An optional
  ///     port's flag must be set exactly when its accessor returns a
  ///     host-supplied provider rather than the stand-in.
  virtual PlatformInfo info() const noexcept = 0;
  /// @brief Tells whether the factory supplies one or more optional ports.
  ///
  /// The accessor of an optional port always returns a provider; when the
  /// host supplies none it is a stand-in that refuses every request. This is
  /// how the library tells the two apart.
  ///
  /// @param capability The ports to ask about.
  /// @return True when info() reports every flag in @p capability.
  bool supplies(PlatformCapability capability) const noexcept {
    return (info().capabilities & capability) == capability;
  }
  /// @brief The provider of threads, mutexes, condition variables and stop
  /// sources.
  ///
  /// @return The provider; it must outlive every object built from the
  ///     factory.
  virtual ThreadingProvider& threading() noexcept = 0;
  /// @brief The monotonic clock the library reads to measure elapsed time.
  ///
  /// @return The clock; it must outlive every object built from the factory.
  virtual ClockProvider& clock() noexcept = 0;
  /// @brief The wall clock the library reads for calendar time: token
  /// expiry, server dates and timestamps it stores.
  ///
  /// @return The clock, counting microseconds since the Unix epoch; it must
  ///     outlive every object built from the factory.
  virtual ClockProvider& wall_clock() noexcept = 0;
  /// @brief The cryptographically secure random source.
  ///
  /// @return The source; it must outlive every object built from the
  ///     factory.
  virtual SecureRandomProvider& random() noexcept = 0;
  /// @brief The provider of sockets, name resolution and interface
  /// enumeration.
  ///
  /// @return The provider; it must outlive every object built from the
  ///     factory.
  virtual NetworkProvider& network() noexcept = 0;
  /// @brief The cryptography provider.
  ///
  /// @return The provider. When the host supplies none it is an
  ///     AbsentCryptoProvider, PlatformCapability::crypto is not reported
  ///     and ChiakiNg::open refuses the factory. Kakusan additionally needs
  ///     the optional identity primitives.
  virtual CryptoProvider& crypto() noexcept = 0;
  /// @brief The HTTP client used for PlayStation Network requests.
  ///
  /// @return The provider. When the host supplies none it is an
  ///     AbsentHttpProvider, PlatformCapability::http is not reported and
  ///     ChiakiNg::open refuses the factory.
  virtual HttpProvider& http() noexcept = 0;
  /// @brief The WebSocket client used for PlayStation Network push
  /// notifications.
  ///
  /// @return The provider. When the host supplies none it is an
  ///     AbsentWebSocketProvider and PlatformCapability::websocket is not
  ///     reported.
  virtual WebSocketProvider& websocket() noexcept = 0;
  /// @brief The router port-mapping provider.
  ///
  /// @return The provider. When the host supplies none it is an
  ///     AbsentGatewayProvider and PlatformCapability::gateway_mapping is
  ///     not reported.
  virtual GatewayProvider& gateway() noexcept = 0;
  /// @brief The DTLS provider used to protect Kakusan traffic between
  /// peers.
  ///
  /// @return The provider. When the host supplies none it is an
  ///     AbsentSecureDatagramProvider, PlatformCapability::secure_datagram
  ///     is not reported and Kakusan is unavailable.
  virtual SecureDatagramProvider& secure_datagram() noexcept = 0;
  /// @brief The sink for the library's log lines.
  ///
  /// @return The provider. When the host supplies none it is an
  ///     AbsentDiagnosticsProvider, which discards every line, and
  ///     PlatformCapability::diagnostics is not reported.
  virtual DiagnosticsProvider& diagnostics() noexcept = 0;
};

/// @brief Host-supplied overrides for DefaultPlatformFactory.
///
/// Every pointer is borrowed and must outlive the factory. A null threading,
/// clock, wall clock, random or network pointer selects the stock
/// implementation the factory owns; any other null pointer leaves that port
/// absent, so the factory hands out its stand-in and does not report it.
struct PlatformProviders {
  /// @brief Threading provider, or nullptr for DefaultThreadingProvider.
  ThreadingProvider* threading{nullptr};
  /// @brief Monotonic clock, or nullptr for SteadyClock.
  ClockProvider* clock{nullptr};
  /// @brief Wall clock, or nullptr for SystemClock.
  ClockProvider* wall_clock{nullptr};
  /// @brief Random source, or nullptr for SystemSecureRandom.
  SecureRandomProvider* random{nullptr};
  /// @brief Network provider, or nullptr for DefaultNetworkProvider.
  NetworkProvider* network{nullptr};
  /// @brief Cryptography provider, or nullptr for none.
  CryptoProvider* crypto{nullptr};
  /// @brief HTTP client, or nullptr for none.
  HttpProvider* http{nullptr};
  /// @brief WebSocket client, or nullptr for none.
  WebSocketProvider* websocket{nullptr};
  /// @brief Log sink, or nullptr to discard diagnostics.
  DiagnosticsProvider* diagnostics{nullptr};
  /// @brief Port-mapping provider, or nullptr for none.
  GatewayProvider* gateway{nullptr};
  /// @brief DTLS provider, or nullptr for none.
  SecureDatagramProvider* secure_datagram{nullptr};
};

}  // namespace libchiaking::platform
