/// @file
/// @brief Declares hole-punch candidates, their codec, and the gathering that
/// decides which endpoints a client offers the console.
#pragma once

#include "libchiaking/platform/gateway/provider.hpp"
#include "libchiaking/platform/net/dns.hpp"
#include "libchiaking/platform/net/interfaces.hpp"
#include "src/core/net/nat/holepunch/holepunch_types.hpp"
#include "src/core/net/nat/holepunch/remote_defaults.hpp"
#include "src/core/net/nat/stun/mapping_estimate.hpp"
#include "src/core/utils/diagnostics/trace.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::net::nat::holepunch {

/// @brief The kind of a hole-punch candidate, as PS names them.
enum class CandidateType {
  /// @brief LOCAL: the sender's LAN address.
  local,
  /// @brief STUN: a public endpoint observed or predicted from STUN.
  stun,
  /// @brief DERIVED: an endpoint learned from where probes came from.
  derived,
  /// @brief STATIC: the public address with the local port, for NATs that
  /// keep ports; also any unknown type.
  static_
};

/// @brief A hole-punch candidate: an endpoint the other side should probe.
struct Candidate {
  /// @brief The kind.
  CandidateType type{CandidateType::static_};
  /// @brief The address, numeric text of at most 45 characters.
  std::string addr;
  /// @brief A second address; this client always writes `0.0.0.0`.
  std::string mapped_addr;
  /// @brief The port.
  std::uint16_t port{};
  /// @brief A second port; this client always writes 0.
  std::uint16_t mapped_port{};
};

/// @brief A candidate as parsed, every field optional until checked.
struct CandidateWire {
  /// @brief Members map to camelCase JSON keys.
  using keys = json::ps::CamelCaseKeys;
  /// @brief `type`: LOCAL, STUN, DERIVED or STATIC.
  std::optional<std::string> type;
  /// @brief `addr`.
  std::optional<std::string> addr;
  /// @brief `mappedAddr`.
  std::optional<std::string> mapped_addr;
  /// @brief `port`.
  std::optional<json::ps::Number> port;
  /// @brief `mappedPort`.
  std::optional<json::ps::Number> mapped_port;
};

/// @brief A candidate as written.
struct CandidateEntry {
  /// @brief Members map to camelCase JSON keys.
  using keys = json::ps::CamelCaseKeys;
  /// @brief `type`.
  std::string_view type;
  /// @brief `addr`.
  std::string addr;
  /// @brief `mappedAddr`.
  std::string mapped_addr;
  /// @brief `port`.
  std::uint16_t port{};
  /// @brief `mappedPort`.
  std::uint16_t mapped_port{};
};

/// @brief Why a candidate could not be written.
enum class CandidateEncodeError : std::uint8_t {
  /// @brief An address is longer than 45 characters, holds a NUL or
  /// non-ASCII.
  invalid_address,
  /// @brief The type has no name.
  invalid_type,
};

/// @brief Why a candidate list could not be written.
enum class CandidateListEncodeError : std::uint8_t {
  /// @brief More than 512 candidates.
  too_many,
  /// @brief A candidate could not be written.
  invalid_candidate,
  /// @brief The list would exceed 16 KiB of JSON.
  too_large,
};

/// @brief Reads and writes hole-punch candidates. Stateless.
class CandidateCodec {
 public:
  /// @brief Names a candidate type.
  ///
  /// @param type The text.
  /// @return The type; STATIC for anything unknown.
  static CandidateType classify_type(std::string_view type);

  /// @brief Whether an address text is acceptable.
  ///
  /// @param value The text.
  /// @return True for at most 45 characters without NUL.
  static bool addr_valid(std::string_view value);

  /// @brief Reads a candidate.
  ///
  /// @param root The parsed candidate.
  /// @return The candidate, ports wrapped to 16 bits; nothing when a field is
  ///     missing, an address is invalid or a port is not an integer.
  [[nodiscard]] static auto decode(const CandidateWire& root)
      -> std::optional<Candidate>;

  /// @brief Most candidates in a list.
  static constexpr std::size_t max_count = 512U;

  /// @brief Reads a candidate list.
  ///
  /// @param listed The parsed list.
  /// @return The candidates; nothing when absent, longer than 512, or any
  ///     candidate is invalid.
  [[nodiscard]] static auto decode_list(
      const std::optional<std::vector<CandidateWire>>& listed)
      -> std::optional<std::vector<Candidate>>;

  /// @brief The wire name of a type.
  ///
  /// @param type The type.
  /// @return LOCAL, STUN, DERIVED or STATIC.
  static auto type_name(CandidateType type) noexcept -> std::string_view;

  /// @brief Prepares a candidate for writing.
  ///
  /// @param candidate The candidate.
  /// @return The entry.
  /// @retval CandidateEncodeError::invalid_address An address is invalid.
  /// @retval CandidateEncodeError::invalid_type The type has no name.
  [[nodiscard]] static auto encode(const Candidate& candidate)
      -> Expected<CandidateEntry, CandidateEncodeError>;

  /// @brief Largest candidate list written: 16 KiB of JSON.
  static constexpr std::size_t max_list_bytes = 16U * 1024U;

  /// @brief Prepares a candidate list for writing, checking its JSON size.
  ///
  /// @param candidates The candidates.
  /// @return The entries.
  /// @retval CandidateListEncodeError The first limit broken.
  [[nodiscard]] static auto encode_list(std::span<const Candidate> candidates)
      -> Expected<std::vector<CandidateEntry>, CandidateListEncodeError>;

 private:
  /// @brief Whether text is 7-bit ASCII.
  ///
  /// @param value The text.
  /// @return True when every byte is below 0x80.
  static auto ascii_valid(std::string_view value) noexcept -> bool;
};

/// @brief A candidate resolved to a socket endpoint.
struct ResolvedCandidateEndpoint {
  /// @brief Its index in the candidate list.
  std::size_t candidate_index{};
  /// @brief Its first specified address, with its port.
  platform::SocketEndpoint endpoint{};
};

/// @brief Why no local candidate could be chosen.
enum class LocalCandidateError : std::uint8_t {
  /// @brief The socket has no port.
  zero_port,
  /// @brief The interfaces could not be listed.
  provider_failure,
  /// @brief No operational IPv4 LAN interface.
  no_usable_interface,
};

/// @brief Why an offer's candidates could not be assembled.
enum class OfferCandidatesError : std::uint8_t {
  /// @brief The local candidate is not LOCAL, has port 0 or an invalid
  /// address.
  invalid_local_candidate,
  /// @brief The public mapping is not a specified IPv4 endpoint.
  invalid_public_mapping,
  /// @brief The allocation step is beyond ±65535.
  invalid_allocation,
  /// @brief The guess count is 0 or above 80.
  invalid_guess_count,
};

/// @brief Why gathering failed.
enum class GatherError : std::uint8_t {
  /// @brief The stop token fired.
  cancelled,
  /// @brief The socket is not bound to a port.
  socket,
  /// @brief No local candidate.
  local,
  /// @brief STUN discovery could not draw randomness.
  stun,
  /// @brief The offer could not be assembled.
  candidate,
};

/// @brief What gathering found.
struct GatheredCandidates {
  /// @brief The local candidate.
  Candidate local;
  /// @brief How the NAT allocates ports.
  net::nat::stun::StunAllocationEstimate allocation;
  /// @brief The candidates to offer, best guesses first, then STATIC and
  /// LOCAL.
  std::vector<Candidate> offer_candidates;
};

/// @brief What gathering needs; every reference must outlive the call.
struct GatherRequest {
  /// @brief The bound IPv4 socket that will punch.
  platform::UdpSocket& socket;
  /// @brief A poller of the socket's owner.
  platform::Poller& poller;
  /// @brief Lists local interfaces.
  platform::InterfacesProvider& interfaces;
  /// @brief The monotonic clock.
  const platform::ClockProvider& clock;
  /// @brief Draws STUN transaction ids.
  platform::SecureRandomProvider& random;
  /// @brief Fetches the STUN server lists.
  platform::HttpProvider& http;
  /// @brief Resolves STUN servers.
  platform::DnsProvider& dns;
  /// @brief Offer guessed ports even when STUN saw a steady mapping that
  /// differs from the local port.
  bool force_port_guessing{};
  /// @brief Ports to guess, 1 to 80.
  std::size_t guess_count{default_port_guess_count};
  /// @brief Wait for each STUN server.
  std::chrono::nanoseconds per_server_timeout{
      net::nat::stun::stun_reply_timeout};
  /// @brief A UPnP or NAT-PMP gateway to map the port with; null for none.
  platform::GatewaySession* gateway{};
};

/// @brief Finds the endpoints to offer the console: the local address, a
/// gateway mapping or the STUN-observed public mapping, and predicted or
/// guessed
/// ports around it. Stateless.
class CandidateGathering {
 public:
  /// @brief Resolves candidates to endpoints.
  ///
  /// Blocks for a DNS lookup per candidate (numeric addresses resolve at
  /// once).
  /// Candidates with port 0 or no specified address are skipped.
  ///
  /// @param candidates The candidates.
  /// @param dns Resolves addresses.
  /// @param stop Stops before the next lookup.
  /// @return One endpoint per resolved candidate; nothing when none resolved.
  [[nodiscard]] static auto resolve_endpoints(
      const std::vector<Candidate>& candidates, platform::DnsProvider& dns,
      const platform::StopToken& stop = {})
      -> std::optional<std::vector<ResolvedCandidateEndpoint>>;

  /// @brief Chooses the local candidate: the first operational IPv4 LAN
  /// interface that is not Ethernet, or failing that the last Ethernet one.
  ///
  /// @param interfaces Lists the interfaces.
  /// @param port The socket's port.
  /// @return A LOCAL candidate.
  /// @retval LocalCandidateError::zero_port `port` is 0.
  /// @retval LocalCandidateError::provider_failure Listing failed.
  /// @retval LocalCandidateError::no_usable_interface None qualifies.
  [[nodiscard]] static auto select_local(
      platform::InterfacesProvider& interfaces, std::uint16_t port)
      -> Expected<Candidate, LocalCandidateError>;

  /// @brief Most ports guessed.
  static constexpr std::size_t max_port_guess_count = 80U;

  /// @brief Builds the candidates to offer from the local candidate and NAT
  /// behaviour.
  ///
  /// A NAT that allocates in steps gets 8 predicted STUN candidates, one step
  /// apart from the projection. A random NAT, or a moved mapping with forced
  /// guessing, gets `guess_count` STUN candidates alternating above and below
  /// the mapping. Otherwise the mapping is offered when its port differs from
  /// the local one. Every offer ends with STATIC at the public address and
  /// the
  /// local port, then LOCAL.
  ///
  /// @param local The local candidate.
  /// @param allocation The NAT's behaviour and projected mapping.
  /// @param force_port_guessing Guess ports whenever the mapping moved.
  /// @param guess_count Ports to guess.
  /// @return The candidates.
  /// @retval OfferCandidatesError The first input that is invalid.
  [[nodiscard]] static auto assemble_offer(
      const Candidate& local,
      const net::nat::stun::StunAllocationEstimate& allocation,
      bool force_port_guessing = false,
      std::size_t guess_count = holepunch::default_port_guess_count)
      -> Expected<std::vector<Candidate>, OfferCandidatesError>;

  /// @brief Gathers the offer for a socket.
  ///
  /// With a gateway that maps the port, its mapping is used and STUN skipped.
  /// Otherwise STUN discovery runs; with forced guessing a moved, steady
  /// mapping is treated as random. When no STUN server answers, only the
  /// local address is offered, so the route can still open from the console's
  /// answers to our probes. Blocks for the gateway, list fetches, DNS and STUN
  /// queries.
  ///
  /// @param request What gathering needs.
  /// @param stop Cancels gathering.
  /// @param discovery STUN state kept across attempts.
  /// @param trace Records each step; may be null.
  /// @return The local candidate, allocation and offer.
  /// @retval GatherError::cancelled The stop token fired.
  /// @retval GatherError::socket The socket has no port.
  /// @retval GatherError::local No local candidate.
  /// @retval GatherError::stun Discovery could not draw randomness.
  /// @retval GatherError::candidate The offer could not be assembled.
  [[nodiscard]] static auto gather(
      const GatherRequest& request, const platform::StopToken& stop,
      net::nat::stun::StunDiscoveryState& discovery,
      const diagnostics::Trace* trace = nullptr)
      -> Expected<GatheredCandidates, GatherError>;

 private:
  /// @brief Offers only the local address, for a socket no STUN server
  /// mapped.
  ///
  /// @param local The local candidate.
  /// @param why Why discovery gave no mapping, for the trace.
  /// @param trace Records the decision; may be null.
  /// @return The local candidate, its own address as the allocation, and
  ///     STATIC and LOCAL at that address.
  /// @retval GatherError::local The local address does not parse.
  /// @retval GatherError::candidate The offer could not be assembled.
  static auto unmapped(const Candidate& local,
                       net::nat::stun::StunAllocationDiscoveryError why,
                       const diagnostics::Trace* trace)
      -> Expected<GatheredCandidates, GatherError>;
  static auto
  /// @brief Makes a candidate with no second address (`0.0.0.0`, port 0).
  ///
  /// @param type The kind.
  /// @param address The address.
  /// @param port The port.
  /// @return The candidate.
  offered(CandidateType type, const platform::IpAddress& address,
          std::uint16_t port) -> Candidate;
  /// @brief Appends STATIC at the public address with the local port, then
  /// the
  /// local candidate.
  ///
  /// @param candidates The candidates so far.
  /// @param public_address The public address.
  /// @param local The local candidate.
  /// @return The completed list.
  static auto completed(std::vector<Candidate> candidates,
                        const platform::IpAddress& public_address,
                        const Candidate& local) -> std::vector<Candidate>;
  /// @brief Offers a steady mapping.
  ///
  /// @param local The local candidate.
  /// @param public_mapping The public mapping.
  /// @return A STUN candidate when the port moved, then STATIC and LOCAL.
  static auto mapped_offer(const Candidate& local,
                           const platform::SocketEndpoint& public_mapping)
      -> std::vector<Candidate>;
  /// @brief Offers 8 ports one allocation step apart, wrapping out-of-range
  /// ports.
  ///
  /// @param local The local candidate.
  /// @param allocation The allocation.
  /// @return The candidates.
  /// @retval OfferCandidatesError::invalid_allocation The step is beyond
  ///     ±65535.
  static auto predicted_offer(
      const Candidate& local,
      const net::nat::stun::StunAllocationEstimate& allocation)
      -> Expected<std::vector<Candidate>, OfferCandidatesError>;
  /// @brief Offers ports around a mapping: +0, +1, −1, +2, −2 and so on.
  ///
  /// @param local The local candidate.
  /// @param public_mapping The public mapping.
  /// @param guess_count How many.
  /// @return The candidates.
  /// @retval OfferCandidatesError::invalid_guess_count 0 or above 80.
  static auto guessed_offer(const Candidate& local,
                            const platform::SocketEndpoint& public_mapping,
                            std::size_t guess_count)
      -> Expected<std::vector<Candidate>, OfferCandidatesError>;
};

}  // namespace libchiaking::net::nat::holepunch
