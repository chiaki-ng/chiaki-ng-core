/// @file
/// @brief Declares what a home network search is told to cover and what it
/// reports: Subnet, SearchRanges, NetworkView, NetworkWatcher and
/// SearchFailure.
#pragma once

#include "libchiaking/detail/consoles/home_network.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/net/types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::consoles {

class Console;

/// @brief An IPv4 address range in CIDR form that a search may probe address by
/// address.
///
/// Only small ranges are allowed, from a /24 down to a single address, and only
/// ordinary unicast space.
class Subnet {
 public:
  /// @brief Shortest prefix accepted, which is the largest range: 254 hosts.
  static constexpr std::uint8_t shortest_prefix = 24;
  /// @brief Longest prefix accepted: a single address.
  static constexpr std::uint8_t longest_prefix = 32;
  /// @brief Longest text parse() accepts.
  static constexpr std::size_t text_limit = 18;

  /// @brief Reads a range from CIDR text such as "192.168.1.0/24".
  ///
  /// Host bits in the address are ignored, so "192.168.1.77/24" yields the
  /// same range.
  ///
  /// @param cidr An IPv4 address, a slash and a prefix length.
  /// @return The range, or nothing when the text is malformed or longer than
  ///     text_limit, the address is not IPv4, the prefix lies outside
  ///     shortest_prefix to longest_prefix, or the first octet is 0, 127, or
  ///     224 and above.
  [[nodiscard]] static auto parse(std::string_view cidr)
      -> std::optional<Subnet>;

  /// @brief The first address of the range, with the host bits cleared.
  ///
  /// @return The first address with the host bits cleared.
  auto network() const noexcept -> platform::IpAddress;
  /// @brief The number of leading bits that are fixed, from 24 to 32.
  ///
  /// @return The prefix length.
  auto prefix_length() const noexcept -> std::uint8_t { return prefix_length_; }
  /// @brief How many addresses a search probes in this range.
  ///
  /// The network and broadcast addresses are left out of ranges that have
  /// more than two addresses, so a /24 counts 254, a /31 counts 2 and a /32
  /// counts 1.
  ///
  /// @return The number of probed addresses, at least 1.
  auto host_count() const noexcept -> std::size_t;
  /// @brief Returns one of the addresses a search probes.
  ///
  /// @param index Position from 0.
  /// @pre index is below host_count().
  /// @return The address at that position, in ascending order.
  auto host(std::size_t index) const noexcept -> platform::IpAddress;
  /// @brief Whether an address lies inside the range.
  ///
  /// @param address The address to test.
  /// @return False for any IPv6 address.
  auto contains(platform::IpAddress address) const noexcept -> bool;
  /// @brief The range in CIDR text, for example "192.168.1.0/24".
  ///
  /// @return The CIDR text.
  auto text() const -> std::string;

  /// @brief Compares the network address and the prefix length.
  friend auto operator==(const Subnet&, const Subnet&) noexcept
      -> bool = default;

 private:
  Subnet(std::uint32_t network, std::uint8_t prefix_length) noexcept
      : network_(network), prefix_length_(prefix_length) {}

  static auto mask_of(std::uint8_t prefix_length) noexcept -> std::uint32_t;
  static auto number_of(platform::IpAddress address) noexcept -> std::uint32_t;
  static auto address_of(std::uint32_t number) noexcept -> platform::IpAddress;

  std::uint32_t network_;
  std::uint8_t prefix_length_;
};

/// @brief The extra subnets a home network search covers beyond local
/// broadcast.
///
/// A search always broadcasts on the local interfaces. Ranges add subnets a
/// broadcast cannot reach, such as another VLAN, which are probed one address
/// at a time. The number of subnets and of addresses is capped.
///
/// A subnet can be given a network provider of its own with over(), such as a
/// tunnel or VPN that reaches a single console at a /32. Its addresses are
/// then probed through that provider alone, as unicast, while broadcast and
/// the other subnets keep the platform's provider. An address in several
/// subnets is probed through the provider of the narrowest one. The search
/// holds the providers until it ends; nothing about them is saved.
///
/// @see KnownConsoles::find_on_my_network
class SearchRanges {
 public:
  /// @brief Why a set of ranges was not accepted.
  enum class Invalid : std::uint8_t {
    /// @brief One of the texts is not a valid Subnet, or over() was given a
    /// subnet the ranges do not hold.
    subnet,
    /// @brief More than subnet_limit subnets were given.
    too_many_subnets,
    /// @brief The subnets cover more than target_limit distinct addresses.
    too_many_targets,
  };

  /// @brief Most subnets a search accepts.
  static constexpr std::size_t subnet_limit = 32;
  /// @brief Most distinct addresses a search probes.
  static constexpr std::size_t target_limit = 1024;

  /// @brief Creates empty ranges: the search broadcasts and probes nothing
  /// else.
  SearchRanges() noexcept = default;

  /// @brief Builds ranges from subnets. Repeated subnets are kept once.
  ///
  /// @param subnets The subnets to cover.
  /// @param unicast Whether the search probes the subnets' addresses. When
  ///     false the subnets are carried but nothing in them is probed.
  /// @return The ranges.
  /// @retval Invalid::too_many_subnets More than subnet_limit subnets were
  ///     given.
  /// @retval Invalid::too_many_targets The subnets cover more than
  ///     target_limit distinct addresses.
  [[nodiscard]] static auto create(std::vector<Subnet> subnets, bool unicast)
      -> Expected<SearchRanges, Invalid>;
  /// @brief Builds ranges from CIDR texts.
  ///
  /// @param cidrs One text per subnet, each as Subnet::parse accepts.
  /// @param unicast Whether the search probes the subnets' addresses.
  /// @return The ranges.
  /// @retval Invalid::subnet A text is not a valid Subnet.
  /// @retval Invalid::too_many_subnets More than subnet_limit texts were
  ///     given.
  /// @retval Invalid::too_many_targets The subnets cover more than
  ///     target_limit distinct addresses.
  [[nodiscard]] static auto parse(std::span<const std::string> cidrs,
                                  bool unicast)
      -> Expected<SearchRanges, Invalid>;

  /// @brief Returns these ranges with one subnet probed over a network
  /// provider of its own.
  ///
  /// The subnet's addresses are probed through that provider even when the
  /// device is itself on the subnet. Giving a default HomeNetwork returns the
  /// subnet to the platform's provider.
  ///
  /// @param subnet One of subnets().
  /// @param network The provider its probes go through.
  /// @return The ranges with the subnet's provider replaced.
  /// @retval Invalid::subnet The ranges do not hold @p subnet.
  [[nodiscard]] auto over(const Subnet& subnet, HomeNetwork network) const
      -> Expected<SearchRanges, Invalid>;
  /// @brief Returns the network provider a subnet is probed over.
  ///
  /// @param subnet The subnet.
  /// @return The provider given with over(), or a default HomeNetwork for
  ///     the platform's own, including for a subnet the ranges do not hold.
  ///     Lives as long as this object.
  auto network_of(const Subnet& subnet) const noexcept -> const HomeNetwork&;

  /// @brief The distinct subnets, in the order given.
  ///
  /// @return The subnets, which live as long as this object; empty when none
  /// were given.
  auto subnets() const noexcept -> const std::vector<Subnet>& {
    return subnets_;
  }
  /// @brief Whether the search probes the subnets' addresses one by one.
  ///
  /// @return True when addresses are probed one by one, false otherwise.
  auto unicast() const noexcept -> bool { return unicast_; }
  /// @brief How many distinct addresses the subnets cover.
  ///
  /// @return The number of distinct addresses.
  auto target_count() const noexcept -> std::size_t { return target_count_; }

  /// @brief Compares the subnets, their providers by identity, the unicast
  /// flag and the target count.
  friend auto operator==(const SearchRanges&, const SearchRanges&) noexcept
      -> bool = default;

 private:
  SearchRanges(std::vector<Subnet> subnets, bool unicast,
               std::size_t target_count);

  std::vector<Subnet> subnets_;
  std::vector<HomeNetwork> networks_;
  bool unicast_{};
  std::size_t target_count_{};
};

/// @brief What a home network search currently sees.
///
/// A new view is delivered whenever any of it changes.
///
/// @see NetworkWatcher
struct NetworkView {
  /// @brief The consoles answering the search right now, in order of hardware
  /// id.
  std::vector<std::shared_ptr<Console>> nearby;
  /// @brief How many addresses the search probes one by one in each sweep.
  std::size_t targets{};
  /// @brief How many of those addresses the current sweep has probed so far.
  std::size_t visited{};
  /// @brief Whether the device has at least one network interface the search
  /// can use.
  bool network_available{};
  /// @brief Whether the operating system refused to let the search send, as
  /// happens when local network access has not been granted.
  bool permission_denied{};
  /// @brief The local networks being broadcast on, each as CIDR text.
  std::vector<std::string> broadcast_ranges;
  /// @brief The extra subnets being probed. A subnet the device is itself on
  /// is left out, since broadcast already covers it, unless it has a network
  /// provider of its own.
  std::vector<Subnet> unicast_ranges;
};

/// @brief Receives the views of a running home network search.
///
/// @see KnownConsoles::find_on_my_network
class NetworkWatcher {
 public:
  /// @brief Allows deletion through the interface.
  virtual ~NetworkWatcher() = default;
  /// @brief Called on the thread that called
  /// KnownConsoles::find_on_my_network whenever the view changes.
  ///
  /// @param view The new view. Valid for the duration of the call; copy what
  ///     is needed.
  virtual void on_view(const NetworkView& view) = 0;
};

/// @brief Why a home network search ended on its own.
enum class SearchFailure : std::uint8_t {
  /// @brief Another search with different ranges is running.
  busy,
  /// @brief The search could not be started, or sending or receiving failed.
  network,
  /// @brief The operating system denied access to the local network.
  permission_denied,
  /// @brief Searching is not possible in this set-up.
  /// KnownConsoles::find_on_my_network does not return it.
  not_available,
};

}  // namespace libchiaking::consoles
