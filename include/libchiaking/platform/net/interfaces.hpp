/// @file
/// @brief Declares the port that lists the host's network interfaces and their
/// addresses.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/net/error.hpp>
#include <libchiaking/platform/net/types.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace libchiaking::platform {

/// @brief Physical kind of a network interface, where the host knows it.
enum class InterfaceMedium : std::uint8_t {
  /// @brief The kind is not known. The stock POSIX implementation reports
  /// every interface this way.
  unclassified,
  /// @brief A wired Ethernet adapter.
  ethernet,
  /// @brief A wireless (802.11) adapter.
  wireless,
};

/// @brief One address assigned to one network interface.
///
/// InterfacesProvider::enumerate returns an entry per address, so an interface
/// with an IPv4 and an IPv6 address appears twice with the same name and index.
struct InterfaceInfo {
  /// @brief System name of the interface. Never empty.
  std::string name;
  /// @brief System index of the interface; never 0. It is the value used as
  /// an IPv6 scope id.
  std::uint32_t index{0};
  /// @brief The address this entry describes.
  IpAddress address;
  /// @brief IPv4 broadcast address of the subnet. Meaningful only when
  /// has_broadcast is true.
  IpAddress broadcast;
  /// @brief True when broadcast holds a usable IPv4 broadcast address. The
  /// address is then IPv4 as well, and broadcast is not 0.0.0.0.
  bool has_broadcast{false};
  /// @brief True for the loopback interface.
  bool loopback{false};
  /// @brief True when the interface is up and able to carry traffic. The
  /// library skips interfaces that are not operational when it chooses local
  /// addresses.
  bool operational{false};
  /// @brief False when the host knows the interface is not a wired or
  /// wireless LAN adapter, such as a tunnel or virtual adapter. The library
  /// leaves such interfaces out when it gathers local addresses for an
  /// internet connection. Hosts that cannot tell leave it true.
  bool lan_adapter{true};
  /// @brief Physical kind of the interface.
  InterfaceMedium medium{InterfaceMedium::unclassified};
  /// @brief Whether the interface is administratively enabled, or empty when
  /// the host cannot tell. Console search uses it in preference to
  /// operational and falls back to operational when it is empty.
  std::optional<bool> administrative_up{};
  /// @brief Length of the IPv4 subnet prefix in bits (24 for 255.255.255.0),
  /// or empty for IPv6 entries and when the host cannot tell.
  std::optional<std::uint8_t> ipv4_prefix_length{};
};

/// @brief Port that lists the host's network interfaces.
///
/// Implemented by the host frontend, or by DefaultInterfacesProvider in this
/// tree. The library reaches it through NetworkProvider::interfaces to pick
/// broadcast targets for console search and local addresses for hole punching.
class InterfacesProvider {
 public:
  /// @brief Allows destruction through the interface.
  virtual ~InterfacesProvider() = default;
  /// @brief Takes a snapshot of every interface address on the host.
  ///
  /// The call is synchronous. The library calls it again whenever it needs a
  /// fresh view, so the result need not be cached.
  ///
  /// @return One InterfaceInfo per assigned IPv4 or IPv6 address. The
  ///     conformance suite expects at least one entry and a loopback entry
  ///     among them.
  /// @retval NetError::system_failure The host could not read its interface
  ///     list. Other NetError values may be returned when they describe the
  ///     cause better.
  [[nodiscard]] virtual auto enumerate()
      -> Expected<std::vector<InterfaceInfo>, NetError> = 0;
};

}  // namespace libchiaking::platform
