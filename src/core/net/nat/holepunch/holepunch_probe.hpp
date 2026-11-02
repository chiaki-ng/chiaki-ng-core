/// @file
/// @brief Declares the hole-punch probe exchange: the 88-byte probe datagrams,
/// the probing sockets and plan, and the rounds that find a working path to the
/// console.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/net/poller.hpp"
#include "libchiaking/platform/net/socket.hpp"
#include "libchiaking/platform/net/types.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/nat/holepunch/holepunch_candidate.hpp"
#include "src/core/net/nat/holepunch/holepunch_conn_request.hpp"
#include "src/core/net/nat/holepunch/limits.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace libchiaking::net::nat::holepunch {

/// @brief Why a probe could not be decoded.
enum class ProbeCodecError {
  /// @brief Not 88 bytes.
  bad_size,
  /// @brief Not a request.
  bad_type
};

/// @brief What a datagram received while probing is.
enum class ProbeDatagramKind {
  /// @brief A probe request from the console, to answer.
  incoming_request,
  /// @brief The response to the outstanding request: the path works.
  matching_response,
  /// @brief A response to an earlier request.
  stale_response_id,
  /// @brief Not 88 bytes.
  invalid_size,
  /// @brief Neither a request nor a response.
  invalid_type,
};

/// @brief The fields of an 88-byte hole-punch probe.
///
/// All integers big-endian; unnamed bytes are zero:
///
/// | Offset | Size | Field |
/// | ------ | ---- | ----- |
/// | 0 | 4 | type: 06 00 00 00 request, 07 00 00 00 response |
/// | 4 | 20 | sender's hashed id |
/// | 36 | 20 | receiver's hashed id |
/// | 68 | 2 | sender's session id |
/// | 70 | 2 | receiver's session id |
/// | 75 | 5 | request id |
/// | 80 | 6 | response only: (sender sid, receiver sid, sender sid) XOR
/// (requester's IPv4 address, port) |
struct CandidateProbe {
  /// @brief Bytes in a probe.
  static constexpr std::size_t kEncodedSize = 88;
  /// @brief The sender's hashed id.
  std::array<std::byte, holepunch_hashed_id_size> local_hashed_id{};
  /// @brief The receiver's hashed id.
  std::array<std::byte, holepunch_hashed_id_size> console_hashed_id{};
  /// @brief The sender's session id.
  std::uint16_t local_sid{};
  /// @brief The receiver's session id.
  std::uint16_t console_sid{};
  /// @brief The random request id; a response echoes the request's.
  std::array<std::byte, holepunch_request_id_size> request_id{};
};

/// @brief Encodes, decodes and classifies probes. Stateless.
class ProbeCodec {
 public:
  /// @brief Classifies a received datagram.
  ///
  /// @param packet The datagram.
  /// @param outstanding_request_id The id of the request awaiting an answer.
  /// @return What it is.
  static auto classify(std::span<const std::byte> packet,
                       const std::array<std::byte, holepunch_request_id_size>&
                           outstanding_request_id) -> ProbeDatagramKind;

  /// @brief Encodes a request.
  ///
  /// @param probe The fields.
  /// @return The 88 bytes.
  static auto encode(const CandidateProbe& probe)
      -> std::array<std::byte, CandidateProbe::kEncodedSize>;

  static auto
  /// @brief Encodes a response, with the masked echo of the requester's
  /// endpoint at offset 80.
  ///
  /// @param probe The fields, carrying the request's id.
  /// @param peer Where the request came from; for IPv6 its first 4 address
  ///     bytes are used.
  /// @return The 88 bytes.
  encode_response(const CandidateProbe& probe,
                  const platform::SocketEndpoint& peer)
      -> std::array<std::byte, CandidateProbe::kEncodedSize>;

  /// @brief Decodes a request.
  ///
  /// @param packet The datagram.
  /// @return The fields.
  /// @retval ProbeCodecError::bad_size Not 88 bytes.
  /// @retval ProbeCodecError::bad_type Not a request.
  [[nodiscard]] static auto decode(std::span<const std::byte> packet)
      -> Expected<CandidateProbe, ProbeCodecError>;

 private:
  /// @brief Encodes the shared fields under a type byte.
  ///
  /// @param probe The fields.
  /// @param type 0x06 or 0x07.
  /// @return The 88 bytes, echo zero.
  static auto encode(const CandidateProbe& probe, std::byte type)
      -> std::array<std::byte, CandidateProbe::kEncodedSize>;

  /// @brief First byte of a request.
  static constexpr std::byte request_type{0x06};
  /// @brief First byte of a response.
  static constexpr std::byte response_type{0x07};
  /// @brief Offset of a response's masked echo.
  static constexpr std::size_t peer_echo_offset = 0x50;
  /// @brief Offset of the sender's hashed id.
  static constexpr std::size_t local_hash_offset = 0x04;
  /// @brief Offset of the receiver's hashed id.
  static constexpr std::size_t console_hash_offset = 0x24;
  /// @brief Offset of the sender's session id.
  static constexpr std::size_t local_sid_offset = 0x44;
  /// @brief Offset of the receiver's session id.
  static constexpr std::size_t console_sid_offset = 0x46;
  /// @brief Offset of the request id.
  static constexpr std::size_t request_id_offset = 0x4B;
};

/// @brief Why a probe request could not be made.
enum class ProbeRequestError : std::uint8_t {
  /// @brief The local session id exceeds 16 bits.
  local_sid_out_of_range,
  /// @brief The console's session id exceeds 16 bits.
  console_sid_out_of_range,
  /// @brief The request id could not be drawn.
  random_failed,
};

/// @brief The identities a probe request carries.
struct ProbeRequestInput {
  /// @brief This client's hashed id.
  std::array<std::byte, holepunch_hashed_id_size> local_hashed_id{};
  /// @brief The console's hashed id.
  std::array<std::byte, holepunch_hashed_id_size> console_hashed_id{};
  /// @brief This client's session id; must fit 16 bits.
  std::uint32_t local_sid{};
  /// @brief The console's session id; must fit 16 bits.
  std::uint32_t console_sid{};
};

/// @brief A request ready to send, and the profile its answers are built from.
struct ProbeRequest {
  /// @brief The encoded request.
  std::array<std::byte, CandidateProbe::kEncodedSize> packet{};
  /// @brief Its id, which the matching response echoes.
  std::array<std::byte, holepunch_request_id_size> request_id{};
  /// @brief The fields used to answer the console's requests.
  CandidateProbe response_profile{};
};

/// @brief Why waiting for a probe response failed.
enum class ProbeWaitError {
  /// @brief The stop token was stopped by its caller.
  cancelled,
  /// @brief Nothing matched in time, or the token's deadline ran out.
  timeout,
  /// @brief A socket or the poller failed, or nothing could be sent.
  net,
  /// @brief Bad groups or a malformed probe.
  protocol
};

/// @brief How many probe rounds to run and how long each waits.
struct ProbeRetryPolicy {
  /// @brief Rounds after the first.
  std::uint8_t max_retries{holepunch_probe_max_retries};
  /// @brief Quiet time that ends a round.
  std::chrono::nanoseconds attempt_timeout{holepunch_probe_attempt_timeout};
};

/// @brief One socket and the peers it probes.
struct ProbeSocketGroup {
  /// @brief The socket; unique among the groups.
  platform::UdpSocket* socket{nullptr};
  /// @brief The endpoints probed, of the socket's family.
  std::span<const platform::SocketEndpoint> peers{};
  /// @brief Whether it is a low-TTL NAT probe socket, which probes only in
  /// the first round.
  bool nat_probe{};
  /// @brief Whether answering a request also sends our own request back.
  bool reciprocal{};
};

/// @brief Which socket and peer answered.
struct ProbeSocketPeerMatch {
  /// @brief The group whose socket got the response.
  std::size_t group_index{};
  /// @brief Where the response came from.
  platform::SocketEndpoint peer{};
  /// @brief Whether we had already answered one of the console's requests.
  bool answered_peer_request{};
  /// @brief Whether the peer was not among the candidates.
  bool derived{};
};

/// @brief What probing learned across rounds.
struct ProbeDiscoveryState {
  /// @brief Unlisted endpoints the console's traffic came from, at most 3;
  /// probed in later rounds.
  std::vector<platform::SocketEndpoint> derived;
};

/// @brief Why the probing sockets could not be made.
enum class BoundSocketsError : std::uint8_t {
  /// @brief The IPv4 socket could not be made.
  ipv4_create,
  /// @brief Port sharing could not be enabled.
  ipv4_shared_port,
  /// @brief Binding failed.
  ipv4_bind,
  /// @brief The bound endpoint is not IPv4 with a port.
  ipv4_endpoint,
};

/// @brief The sockets a hole punch probes from: one IPv4 socket, an optional
/// IPv6 socket on the same port, and optional NAT probe sockets. Owns them;
/// movable.
struct BoundSockets {
  /// @brief The IPv4 socket bound to the wildcard address with port sharing.
  std::unique_ptr<platform::UdpSocket> ipv4;
  /// @brief The IPv6 socket on the same port; null when not wanted or not
  /// available.
  std::unique_ptr<platform::UdpSocket> ipv6;
  /// @brief The shared port.
  std::uint16_t local_port{};
  /// @brief NAT probe sockets, each on its own port with TTL 2.
  std::vector<std::unique_ptr<platform::UdpSocket>> probes;
  /// @brief Whether NAT probe sockets were added, so probing answers with
  /// reciprocal requests.
  bool random_allocation{};

  /// @brief Makes the IPv4 socket and, if asked, the IPv6 one.
  ///
  /// A failure to make the IPv6 socket is not reported; it is left out.
  ///
  /// @param sockets Makes the sockets.
  /// @param enable_ipv6 Whether to try IPv6.
  /// @return The sockets.
  /// @retval BoundSocketsError The IPv4 step that failed.
  [[nodiscard]] static auto create(platform::SocketProvider& sockets,
                                   bool enable_ipv6)
      -> Expected<BoundSockets, BoundSocketsError>;

  /// @brief Adds NAT probe sockets, skipping any that cannot be configured
  /// and
  /// stopping when one cannot be made.
  ///
  /// @param bound The sockets to add to.
  /// @param provider Makes the sockets.
  /// @param count How many to try.
  /// @param stop Stops early.
  static void add_nat_sockets(BoundSockets& bound,
                              platform::SocketProvider& provider,
                              std::size_t count,
                              const platform::StopToken& stop);
};

/// @brief Which socket probes which resolved candidate, and how to map a
/// response back to its candidate.
///
/// IPv4 endpoints go to the IPv4 socket and IPv6 to the IPv6 socket; STUN and
/// STATIC IPv4 endpoints are also probed from every NAT probe socket. Holds
/// pointers to the sockets, which must outlive it.
class ProbeGroupPlan {
 public:
  /// @brief Plans the groups.
  ///
  /// @param sockets The sockets.
  /// @param resolved The resolved candidates.
  /// @param candidates The candidates, to find STUN and STATIC ones.
  /// @return The plan; nothing when no endpoint has a socket of its family.
  [[nodiscard]] static auto make(
      const BoundSockets& sockets,
      std::span<const ResolvedCandidateEndpoint> resolved,
      std::span<const Candidate> candidates = {})
      -> std::optional<ProbeGroupPlan>;

  /// @brief Plans the groups, possibly empty.
  ///
  /// @param sockets The sockets.
  /// @param resolved The resolved candidates.
  /// @param candidates The candidates, to find STUN and STATIC ones.
  ProbeGroupPlan(const BoundSockets& sockets,
                 std::span<const ResolvedCandidateEndpoint> resolved,
                 std::span<const Candidate> candidates = {});

  /// @brief Whether nothing can be probed.
  ///
  /// @return True with no IPv4 or IPv6 peers.
  bool empty() const noexcept;

  /// @brief The groups, in order: IPv4, IPv6, then each NAT probe socket.
  ///
  /// @return The groups; their peer spans view this plan.
  auto groups() const -> std::vector<ProbeSocketGroup>;

  /// @brief The candidate a match came from.
  ///
  /// @param match The match.
  /// @return The candidate index; nothing for a derived peer or bad group.
  [[nodiscard]] auto source_index(const ProbeSocketPeerMatch& match) const
      -> std::optional<std::size_t>;

 private:
  /// @brief Finds a peer's candidate index.
  ///
  /// @param peers The group's peers.
  /// @param indices Their candidate indices.
  /// @param peer The peer.
  /// @return The index; nothing when absent.
  [[nodiscard]] static auto find_source(
      const std::vector<platform::SocketEndpoint>& peers,
      const std::vector<std::size_t>& indices,
      const platform::SocketEndpoint& peer) -> std::optional<std::size_t>;

  /// @brief Endpoints the NAT probe sockets probe.
  std::vector<platform::SocketEndpoint> nat_peers_;
  /// @brief Their candidate indices.
  std::vector<std::size_t> nat_sources_;
  /// @brief The NAT probe sockets; empty without STUN or STATIC peers.
  std::vector<platform::UdpSocket*> nat_sockets_;
  /// @brief Endpoints the IPv4 socket probes.
  std::vector<platform::SocketEndpoint> ipv4_peers_;
  /// @brief Endpoints the IPv6 socket probes.
  std::vector<platform::SocketEndpoint> ipv6_peers_;
  /// @brief Candidate indices of the IPv4 peers.
  std::vector<std::size_t> ipv4_sources_;
  /// @brief Candidate indices of the IPv6 peers.
  std::vector<std::size_t> ipv6_sources_;
  /// @brief The IPv4 socket.
  platform::UdpSocket* ipv4_socket_{};
  /// @brief The IPv6 socket; may be null.
  platform::UdpSocket* ipv6_socket_{};
  /// @brief Whether the main sockets send reciprocal requests.
  bool reciprocal_{};
};

/// @brief Why the winning socket could not be taken.
enum class SelectSocketError : std::uint8_t {
  /// @brief The match names no group.
  group_out_of_range,
  /// @brief The peer is neither a candidate nor derived.
  unknown_peer,
  /// @brief The group's socket is not among the sockets.
  socket_mismatch,
};

/// @brief The socket and peer a hole punch settled on.
struct SelectedCandidate {
  /// @brief The winning socket, now owned by the caller.
  std::unique_ptr<platform::UdpSocket> socket;
  /// @brief The console's endpoint.
  platform::SocketEndpoint peer;
  /// @brief The candidate it came from; SIZE_MAX for a derived peer.
  std::size_t candidate_index{};
  /// @brief Whether the peer was learned, not offered.
  bool derived{};
};

/// @brief Why probing the console failed.
enum class PeerProbeError : std::uint8_t {
  /// @brief The stop token fired.
  cancelled,
  /// @brief No candidate resolved to an endpoint a socket can reach.
  no_candidates,
  /// @brief A session id exceeds 16 bits.
  invalid_sid,
  /// @brief The request id could not be drawn.
  random_failed,
  /// @brief No response after every round.
  wait_timeout,
  /// @brief A socket or poller failed.
  wait_net,
  /// @brief A malformed probe or bad group arrived.
  wait_protocol,
  /// @brief The winning socket could not be taken.
  selection_failed,
};

/// @brief What probing the console needs.
struct PeerProbeInput {
  /// @brief The console's candidates.
  std::vector<Candidate> candidates;
  /// @brief The identities the probes carry.
  ProbeRequestInput probe;
  /// @brief The rounds to run.
  ProbeRetryPolicy retry{};
};

/// @brief Why the offers cannot be probed.
enum class PeerProbeInputError : std::uint8_t {
  /// @brief Our session id exceeds 16 bits.
  invalid_local_offer,
  /// @brief The console's session id exceeds 16 bits or it offered no
  /// candidates.
  invalid_peer_offer,
};

/// @brief Probes the console's candidates until one answers, answering the
/// console's own probes meanwhile, and keeps the socket that worked.
///
/// Each round sends the request to every peer of every group (NAT probe
/// sockets only in the first round, derived peers in later ones) and waits
/// until 500 ms pass with nothing received. Stateless; every call blocks.
class Probe {
 public:
  /// @brief Builds a request with a fresh random id.
  ///
  /// @param input The identities.
  /// @param random Draws the id.
  /// @return The request and response profile.
  /// @retval ProbeRequestError The first problem.
  [[nodiscard]] static auto make_request(const ProbeRequestInput& input,
                                         platform::SecureRandomProvider& random)
      -> Expected<ProbeRequest, ProbeRequestError>;

  /// @brief Keeps answering the console's requests on the winning socket
  /// until
  /// it is quiet for 1 second.
  ///
  /// @param socket The winning socket.
  /// @param poller A poller of its owner.
  /// @param peer The console's endpoint; other sources are ignored.
  /// @param response_profile The fields answers are built from.
  /// @param stop_token Cancels the wait.
  /// @param already_answered Whether a request was already answered.
  /// @return Nothing once quiet after at least one answer.
  /// @retval ProbeWaitError::timeout Quiet without ever answering.
  /// @retval ProbeWaitError::cancelled The stop token fired.
  /// @retval ProbeWaitError::net Polling or sending failed.
  /// @retval ProbeWaitError::protocol A malformed datagram from the console.
  [[nodiscard]] static auto answer_candidate_requests(
      platform::UdpSocket& socket, platform::Poller& poller,
      const platform::SocketEndpoint& peer,
      const CandidateProbe& response_profile,
      const platform::StopToken& stop_token, bool already_answered)
      -> Expected<void, ProbeWaitError>;

  /// @brief Runs one probe round.
  ///
  /// Blocks until a matching response, an error, or `timeout` with nothing
  /// received.
  ///
  /// @param groups The sockets and peers.
  /// @param poller A poller of the sockets' owner.
  /// @param clock The monotonic clock.
  /// @param request The encoded request.
  /// @param expected_request_id Its id.
  /// @param response_profile The fields answers are built from.
  /// @param stop_token Cancels the round; a deadline stop is a timeout.
  /// @param timeout The quiet time that ends the round.
  /// @param answered_peer_request Carried across rounds; may be null.
  /// @param discovery Derived peers carried across rounds; may be null.
  /// @param send_nat_probes Whether NAT probe sockets send this round.
  /// @return The match.
  /// @retval ProbeWaitError::protocol Bad groups, a malformed probe, or an
  ///     unexpected datagram from a listed peer.
  /// @retval ProbeWaitError::net Nothing could be sent or a socket failed.
  /// @retval ProbeWaitError::timeout The round ended quietly.
  /// @retval ProbeWaitError::cancelled The stop token fired.
  [[nodiscard]] static auto wait_for_response_from_groups(
      std::span<const ProbeSocketGroup> groups, platform::Poller& poller,
      const platform::ClockProvider& clock, std::span<const std::byte> request,
      const std::array<std::byte, holepunch_request_id_size>&
          expected_request_id,
      const CandidateProbe& response_profile,
      const platform::StopToken& stop_token, std::chrono::nanoseconds timeout,
      bool* answered_peer_request = nullptr,
      ProbeDiscoveryState* discovery = nullptr, bool send_nat_probes = true)
      -> Expected<ProbeSocketPeerMatch, ProbeWaitError>;

  /// @brief Runs probe rounds until one matches, up to the policy's count.
  ///
  /// Blocks for up to the rounds' quiet times, about 10.5 seconds by default
  /// when nothing arrives.
  ///
  /// @param groups The sockets and peers.
  /// @param poller A poller of the sockets' owner.
  /// @param clock The monotonic clock.
  /// @param request The encoded request.
  /// @param expected_request_id Its id.
  /// @param response_profile The fields answers are built from.
  /// @param stop_token Cancels the rounds.
  /// @param policy The rounds to run.
  /// @return The match.
  /// @retval ProbeWaitError Any round's failure other than a timeout, or
  ///     timeout after the last round.
  [[nodiscard]] static auto wait_for_response_from_groups_with_retries(
      std::span<const ProbeSocketGroup> groups, platform::Poller& poller,
      const platform::ClockProvider& clock, std::span<const std::byte> request,
      const std::array<std::byte, holepunch_request_id_size>&
          expected_request_id,
      const CandidateProbe& response_profile,
      const platform::StopToken& stop_token, ProbeRetryPolicy policy = {})
      -> Expected<ProbeSocketPeerMatch, ProbeWaitError>;

  /// @brief Takes the winning socket out of the sockets; the rest are closed.
  ///
  /// @param sockets The sockets; consumed.
  /// @param plan The plan the match came from.
  /// @param match The match.
  /// @return The socket, peer and candidate.
  /// @retval SelectSocketError The first problem.
  [[nodiscard]] static auto select_socket(BoundSockets sockets,
                                          const ProbeGroupPlan& plan,
                                          const ProbeSocketPeerMatch& match)
      -> Expected<SelectedCandidate, SelectSocketError>;

  /// @brief Hole-punches to the console: resolves its candidates, probes
  /// them,
  /// answers its probes, and returns the socket that works.
  ///
  /// Blocks for DNS, the probe rounds and the final answering phase.
  ///
  /// @param sockets The sockets to probe from; consumed.
  /// @param dns Resolves candidates.
  /// @param poller A poller of the sockets' owner.
  /// @param clock The monotonic clock.
  /// @param random Draws the request id.
  /// @param stop_token Cancels probing.
  /// @param input The candidates, identities and rounds.
  /// @return The socket and the console's endpoint.
  /// @retval PeerProbeError The first failure.
  [[nodiscard]] static auto probe_peer(BoundSockets sockets,
                                       platform::DnsProvider& dns,
                                       platform::Poller& poller,
                                       const platform::ClockProvider& clock,
                                       platform::SecureRandomProvider& random,
                                       const platform::StopToken& stop_token,
                                       const PeerProbeInput& input)
      -> Expected<SelectedCandidate, PeerProbeError>;

  /// @brief Builds the probing input from both connection requests.
  ///
  /// @param ours Our request.
  /// @param peer The console's.
  /// @return The console's candidates and both identities.
  /// @retval PeerProbeInputError The offer that cannot be probed.
  [[nodiscard]] static auto make_peer_probe_input(const ConnRequest& ours,
                                                  const ConnRequest& peer)
      -> Expected<PeerProbeInput, PeerProbeInputError>;

 private:
  /// @brief Names why a stopped token stopped.
  ///
  /// @param stop_token The stopped token.
  /// @return timeout for a deadline, otherwise cancelled.
  static auto stopped(const platform::StopToken& stop_token) noexcept
      -> ProbeWaitError;
  /// @brief Translates a wait failure.
  ///
  /// @param error The failure.
  /// @return Its PeerProbeError counterpart.
  static auto peer_probe_error(ProbeWaitError error) noexcept -> PeerProbeError;
};

}  // namespace libchiaking::net::nat::holepunch
