/// @file
/// @brief Declares AllocationDiscovery, which learns how the local NAT assigns
/// public ports from STUN answers and projects the next one for hole punching.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/net/types.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/nat/stun/binding_client.hpp"
#include "src/core/net/nat/stun/public_servers.hpp"
#include "src/core/utils/diagnostics/trace.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::net::nat::stun {

/// @brief Default wait for each STUN server's answer: 5 seconds.
inline constexpr std::chrono::seconds stun_reply_timeout{5};

/// @brief Longest time discovery keeps asking servers: 10 seconds.
///
/// Servers usually answer within a second, so a network that leaves two in a
/// row unanswered is not passing STUN, and asking every server in turn would
/// use up the whole start.
inline constexpr std::chrono::seconds stun_discovery_budget{10};

/// @brief Why observations could not be turned into an estimate.
enum class StunAllocationError : std::uint8_t {
  /// @brief The observations are of the wrong number, family or shape.
  invalid_input
};

/// @brief A guess at the public port the NAT will give the next mapping, from
/// how it allocated the last ones.
struct StunAllocationEstimate {
  /// @brief The public address and port the console should aim at.
  platform::SocketEndpoint projected;
  /// @brief The step between consecutive public ports; 0 when the port is
  /// kept.
  std::int32_t increment{};
  /// @brief Whether the observed steps disagree, so the NAT seems to allocate
  /// at random.
  bool random_allocation{};
};

/// @brief Why discovery gave no estimate.
enum class StunAllocationDiscoveryError : std::uint8_t {
  /// @brief The stop token fired.
  cancelled,
  /// @brief No STUN server resolved.
  no_servers,
  /// @brief A transaction id or shuffle could not be drawn.
  random_failure,
  /// @brief Servers resolved but none answered usefully.
  exhausted,
};

/// @brief What discovery learns that later attempts may reuse: the fetched
/// lists and the last estimate.
struct StunDiscoveryState {
  /// @brief The fetched host list.
  std::vector<StunHostListEntry> preferred;
  /// @brief The fetched IPv6 list.
  std::vector<StunHostListEntry> preferred_ipv6;
  /// @brief The last estimate; once set, discovery asks one server and keeps
  /// its increment.
  std::optional<StunAllocationEstimate> allocation;
};

/// @brief Learns how the local NAT allocates public ports by asking up to four
/// STUN servers from one socket, and projects the next one for hole punching.
///
/// Observations from the same
/// public address give the port step, observations from different addresses
/// are weighed by which ones match, and disagreeing steps mark the allocation
/// random. Stateless; every member is static.
class AllocationDiscovery {
 public:
  /// @brief Most mappings observed.
  static constexpr std::size_t observation_count = 4;

  [[nodiscard]] static auto
  /// @brief Estimates from four mappings whose first two public addresses
  /// differ.
  ///
  /// @param observations Exactly four IPv4 mappings, in the order observed.
  /// @return The estimate, projected at the fourth mapping.
  /// @retval StunAllocationError::invalid_input Not four, not IPv4, an
  ///     unspecified address or port 0, or the first two addresses match.
  estimate_four_divergent(
      std::span<const platform::SocketEndpoint> observations)
      -> Expected<StunAllocationEstimate, StunAllocationError>;

  /// @brief Estimates the next public port from one to four mappings.
  ///
  /// One mapping is used as is. Two from one address project two steps on
  /// from
  /// the second; from different addresses the second is used as is. Three
  /// project one step on from the third; four keep the fourth.
  ///
  /// @param observations One to four IPv4 mappings, in the order observed.
  /// @return The estimate.
  /// @retval StunAllocationError::invalid_input None, more than four, not
  ///     IPv4,
  ///     an unspecified address or port 0.
  [[nodiscard]] static auto estimate(
      std::span<const platform::SocketEndpoint> observations)
      -> Expected<StunAllocationEstimate, StunAllocationError>;

  /// @brief Observes the socket's mappings and estimates the next one.
  ///
  /// Fetches the server lists unless `cached` already holds an estimate, then
  /// asks servers in build_host_order(), shuffling the fixed hosts after the
  /// first, until four answered. With a cached estimate it asks until one
  /// answers and returns that mapping with the cached step. Stops asking once
  /// `budget` has passed since the first server was asked and estimates from
  /// the mappings it has. Blocks for the list fetch (up to 10
  /// seconds per list), DNS, and up to `per_server_timeout` per server.
  ///
  /// @param socket The bound IPv4 socket that will punch.
  /// @param poller A poller of the same owner.
  /// @param clock The monotonic clock timing the waits.
  /// @param random Draws transaction ids and the shuffle.
  /// @param http Fetches the lists.
  /// @param dns Resolves the servers.
  /// @param stop Cancels discovery.
  /// @param per_server_timeout Wait for each server.
  /// @param trace Records each step; may be null.
  /// @param cached State kept across attempts; may be null.
  /// @param budget How long to keep asking servers.
  /// @return The estimate.
  /// @retval StunAllocationDiscoveryError::cancelled The stop token fired.
  /// @retval StunAllocationDiscoveryError::random_failure Randomness failed.
  /// @retval StunAllocationDiscoveryError::no_servers No server resolved.
  /// @retval StunAllocationDiscoveryError::exhausted No server answered.
  [[nodiscard]] static auto discover(
      platform::UdpSocket& socket, platform::Poller& poller,
      const platform::ClockProvider& clock,
      platform::SecureRandomProvider& random, platform::HttpProvider& http,
      platform::DnsProvider& dns, const platform::StopToken& stop,
      std::chrono::nanoseconds per_server_timeout = stun_reply_timeout,
      const diagnostics::Trace* trace = nullptr,
      StunDiscoveryState* cached = nullptr,
      std::chrono::nanoseconds budget = stun_discovery_budget)
      -> Expected<StunAllocationEstimate, StunAllocationDiscoveryError>;
};

}  // namespace libchiaking::net::nat::stun
