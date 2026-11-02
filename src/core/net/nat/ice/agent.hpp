/// @file
/// @brief Declares Agent, the sans-I/O ICE agent that finds and keeps the peer
/// link's direct UDP path between host and guest, with its timers and limits.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "libchiaking/platform/net/types.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/nat/holepunch/limits.hpp"
#include "src/core/net/nat/ice/agent_event.hpp"
#include "src/core/net/nat/ice/agent_lifecycle.hpp"
#include "src/core/net/nat/ice/parameters.hpp"
#include "src/core/net/nat/stun/message.hpp"
#include "src/core/net/ports.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace libchiaking::net::nat::ice {

/// @brief Ta, the interval between new connectivity checks, in microseconds: 50
/// ms.
inline constexpr std::uint64_t pacing_interval_us = 50'000;
/// @brief Smallest initial retransmission timeout of a check, in microseconds:
/// 500 ms.
inline constexpr std::uint64_t check_rto_min_us = 500'000;
/// @brief Times a check request is sent before it fails.
inline constexpr std::size_t check_transmissions = 5;
/// @brief After the last send a check waits this many times its timeout.
inline constexpr std::uint64_t check_final_wait_factor = 4;
/// @brief Initial retransmission timeout of a STUN server query, in
/// microseconds: 500 ms.
inline constexpr std::uint64_t server_rto_us = 500'000;
/// @brief Times a STUN server query is sent: 7, as RFC 8489 Rc.
inline constexpr std::size_t server_transmissions = 7;
/// @brief After the last send a server query waits this many timeouts: 16, as
/// RFC 8489 Rm.
inline constexpr std::uint64_t server_final_wait_factor = 16;
/// @brief Most candidate pairs in the checklist.
inline constexpr std::size_t max_pairs = 100;
/// @brief Most local sockets the agent uses.
inline constexpr std::size_t max_bases = max_candidates;
/// @brief Most STUN servers queried while gathering.
inline constexpr std::size_t max_stun_servers = 4;
/// @brief Most peer-reflexive remote candidates learned from incoming checks.
inline constexpr std::size_t max_remote_reflexive = 16;
/// @brief Most authenticated checks held from before connect(); the oldest is
/// dropped.
inline constexpr std::size_t max_early_requests = 8;
/// @brief Most check transmissions over the agent's life.
inline constexpr std::size_t max_check_requests = 5000;
/// @brief Least time between any two check transmissions, in microseconds: 10
/// ms.
inline constexpr std::uint64_t check_floor_us = 10'000;
/// @brief Wait before a guest retries a checklist that failed, in microseconds:
/// 5 seconds.
inline constexpr std::uint64_t guest_rearm_us = 5'000'000;
/// @brief How long the controlling agent waits after its first valid pair for a
/// better one still being checked, in microseconds: 1 second.
inline constexpr std::uint64_t nomination_settle_us = 1'000'000;
/// @brief Times a nominating check may time out before its pair is failed.
inline constexpr std::size_t nomination_attempts = 3;
/// @brief Least interval between consent checks, in microseconds: 4 seconds.
inline constexpr std::uint64_t consent_interval_min_us = 4'000'000;
/// @brief Random span added to the consent interval, in microseconds: up to 2
/// seconds.
inline constexpr std::uint64_t consent_interval_span_us = 2'000'000;
/// @brief How long a consent check waits for its answer, in microseconds: 4
/// seconds.
inline constexpr std::uint64_t consent_response_window_us = 4'000'000;
/// @brief Consent expires this long after the last answer, in microseconds: 30
/// seconds (RFC 7675).
inline constexpr std::uint64_t consent_timeout_us = 30'000'000;
/// @brief Type preference of host candidates.
inline constexpr std::uint32_t host_type_preference = 126;
/// @brief Type preference of peer-reflexive candidates, sent as PRIORITY.
inline constexpr std::uint32_t reflexive_type_preference = 110;
/// @brief Type preference of server-reflexive candidates.
inline constexpr std::uint32_t server_reflexive_type_preference = 100;
/// @brief Bit position of the type preference in a candidate priority.
inline constexpr std::uint32_t type_preference_shift = 24;
/// @brief Bit position of the local preference in a candidate priority.
inline constexpr std::uint32_t local_preference_shift = 8;
/// @brief 256; the low byte of a priority is this less the component id.
inline constexpr std::uint32_t component_preference_range = 256;
/// @brief Local preference of the most preferred base; each next base gets one
/// less.
inline constexpr std::uint16_t max_local_preference = 65535;
/// @brief Shift of the smaller priority in a pair priority (RFC 8445 section
/// 6.1.2.3).
inline constexpr std::uint32_t pair_priority_minimum_shift = 32;
/// @brief Multiplier of the larger priority in a pair priority.
inline constexpr std::uint64_t pair_priority_maximum_factor = 2;
/// @brief Random bytes drawn for each consent interval.
inline constexpr std::size_t consent_draw_size = 4;

/// @brief A value or the agent's failure.
///
/// @tparam T The value type.
template <class T>
using Result = Expected<T, Error>;

/// @brief The agent's ICE role, which can change through role conflicts.
enum class Role : std::uint8_t {
  /// @brief Decides which pair is used, by nominating it with USE-CANDIDATE.
  controlling,
  /// @brief Accepts the pair the other side nominates.
  controlled,
};

/// @brief One local UDP socket the agent may use.
struct Base {
  /// @brief The caller's id for the socket; unique per agent.
  SocketId socket{};
  /// @brief The address and port it is bound to; becomes a host candidate.
  platform::SocketEndpoint local;
  /// @brief Interface preference; higher is preferred, ties go to the lower
  /// address and port.
  std::uint16_t preference{};
};

/// @brief What an agent is made with.
struct AgentConfig {
  /// @brief Which side of the session it serves.
  Policy role{Policy::host};
  /// @brief The local sockets; only the first 32 distinct usable ones are
  /// kept.
  std::vector<Base> bases;
  /// @brief STUN servers to gather reflexive candidates from; the first 4
  /// distinct are used.
  std::vector<platform::SocketEndpoint> stun_servers;
  /// @brief When to advertise port guesses.
  PortGuessing guessing{PortGuessing::automatic};
  /// @brief Fixed local credentials; nothing draws fresh ones.
  std::optional<Credentials> credentials;

  /// @brief Copies the configuration, credentials into new secrets.
  ///
  /// @return The copy.
  auto clone() const -> AgentConfig {
    return AgentConfig{role, bases, stun_servers, guessing,
                       credentials.transform(&Credentials::clone)};
  }
};

/// @brief A datagram the caller must send for the agent.
struct Outgoing {
  /// @brief The socket to send from.
  SocketId socket{};
  /// @brief Where to send it.
  platform::SocketEndpoint remote;
  /// @brief The STUN message.
  std::vector<std::byte> bytes;
};

/// @brief A full ICE agent (RFC 8445, one component, regular nomination) for
/// the peer link's direct UDP path, with STUN gathering, RFC 7675 consent
/// freshness and port guessing for NATs that move ports.
///
/// Sans-I/O: the caller owns the sockets, feeds every received datagram to
/// receive(), calls poll() by next_deadline_us(), sends each Outgoing, and
/// drains take_event(). Time is the caller's monotonic clock in microseconds.
/// Lifecycle: create() gathers host and server-reflexive candidates and reports
/// Gathered; connect() takes the peer's parameters and starts checks; the
/// controlling side nominates the best valid pair (after up to 1 second for
/// better pairs in flight) and both report Nominated; consent checks then run
/// every 4 to 6 seconds and failure is reported once. Credentials and
/// tie-breaker are wiped on close(). Not copyable or movable, made in a
/// unique_ptr; not thread-safe, one thread drives it.
class Agent {
 public:
  /// @brief Makes an agent and starts gathering.
  ///
  /// With no usable base the agent is returned already failed with no_path;
  /// with no STUN server of a base's family gathering finishes at once.
  ///
  /// @param crypto Computes STUN integrity; must outlive the agent.
  /// @param random Draws credentials, tie-breaker, transaction ids and
  ///     consent
  ///     intervals; must outlive the agent.
  /// @param config Sockets, servers, policy and credentials.
  /// @param now_us The current monotonic time, in microseconds.
  /// @return The agent.
  /// @retval Error::internal Two bases share a socket id.
  /// @retval Error::crypto_unavailable Randomness failed or drew all zeros.
  [[nodiscard]] static auto create(platform::CryptoProvider& crypto,
                                   platform::SecureRandomProvider& random,
                                   AgentConfig config, std::uint64_t now_us)
      -> Result<std::unique_ptr<Agent>>;
  /// @brief Closes the agent, wiping its secrets.
  ~Agent();

  /// @brief Not copyable: it holds secrets and pairs by address.
  Agent(const Agent&) = delete;
  /// @brief Not movable.
  Agent(Agent&&) = delete;
  /// @brief Not copyable.
  ///
  /// @return Never returns.
  auto operator=(const Agent&) -> Agent& = delete;
  /// @brief Not movable.
  ///
  /// @return Never returns.
  auto operator=(Agent&&) -> Agent& = delete;

  /// @brief Takes the peer's parameters and starts connectivity checks.
  ///
  /// Adds the peer's candidates and up to its port-guess count of guessed
  /// endpoints, forms the checklist, replays checks that arrived early, and
  /// for
  /// the host starts the 30-second nomination deadline.
  ///
  /// @param remote The peer's parameters.
  /// @param now_us The current monotonic time, in microseconds.
  /// @return Nothing on success.
  /// @retval Error::protocol The agent is closed, failed, already connected,
  ///     or
  ///     has not finished gathering.
  [[nodiscard]] auto connect(const Parameters& remote, std::uint64_t now_us)
      -> Result<void>;
  /// @brief Feeds a datagram received on one of the agent's sockets.
  ///
  /// Datagrams that are not STUN, come from another address family, or fail
  /// authentication are ignored. Authenticated checks are answered at once;
  /// checks before connect() are held.
  ///
  /// @param socket The socket it arrived on.
  /// @param source Who sent it.
  /// @param datagram The datagram.
  /// @param now_us The current monotonic time, in microseconds.
  /// @param outgoing Receives any responses to send.
  void receive(SocketId socket, const platform::SocketEndpoint& source,
               std::span<const std::byte> datagram, std::uint64_t now_us,
               std::vector<Outgoing>& outgoing);
  /// @brief Runs everything due: retransmissions, refreshes, the end of
  /// gathering, new checks, nomination, rearming and consent.
  ///
  /// @param now_us The current monotonic time, in microseconds.
  /// @param outgoing Receives the datagrams to send.
  void poll(std::uint64_t now_us, std::vector<Outgoing>& outgoing);
  /// @brief Takes the oldest unreported event.
  ///
  /// @return The event; nothing when none is waiting.
  [[nodiscard]] auto take_event() -> std::optional<Event>;
  /// @brief When poll() must next run.
  ///
  /// @return The monotonic instant, in microseconds; nothing when closed,
  ///     failed, or with nothing scheduled.
  auto next_deadline_us() const noexcept -> std::optional<std::uint64_t>;
  /// @brief The nominated pair.
  ///
  /// @return The path; nothing before nomination or after close().
  auto nominated() const noexcept -> std::optional<Path>;
  /// @brief Whether traffic may be sent on the nominated pair.
  ///
  /// @return True after nomination until consent expires or the agent closes.
  auto consent() const noexcept -> bool;
  /// @brief The agent's current ICE role.
  ///
  /// @return The role.
  auto role() const noexcept -> Role { return role_; }
  /// @brief Check transmissions so far.
  ///
  /// @return From 0 to max_check_requests.
  auto check_requests_sent() const noexcept -> std::size_t {
    return check_requests_sent_;
  }
  /// @brief Stops the agent and wipes its credentials, tie-breaker and state.
  /// Idempotent.
  void close() noexcept;

 private:
  /// @brief Where a candidate pair stands in the checklist.
  enum class PairState : std::uint8_t {
    /// @brief Not yet checked, or queued to be checked again.
    waiting,
    /// @brief A check is in flight.
    in_progress,
    /// @brief A check succeeded; the pair is valid.
    succeeded,
    /// @brief Checks failed or timed out.
    failed,
  };

  /// @brief What a STUN transaction is for.
  enum class TransactionKind : std::uint8_t {
    /// @brief A connectivity check on a pair.
    check,
    /// @brief A server query gathering a reflexive candidate.
    gather,
    /// @brief A server query keeping a reflexive mapping alive and checking
    /// it is unchanged.
    refresh,
    /// @brief A consent freshness check on the nominated pair.
    consent,
  };

  /// @brief A base and its ranking.
  struct BaseState {
    /// @brief The base as configured.
    Base base;
    /// @brief Its RFC 8445 local preference: 65535 less its rank.
    std::uint16_t local_preference{};
    /// @brief Index of its address among distinct local addresses, used in
    /// foundations.
    std::size_t address_index{};
  };

  /// @brief A local candidate.
  struct LocalCandidate {
    /// @brief Host or server reflexive.
    CandidateType type{CandidateType::host};
    /// @brief Its address and port.
    platform::SocketEndpoint endpoint;
    /// @brief Index of its base.
    std::size_t base{};
    /// @brief Index of the STUN server that found it.
    std::size_t server{};
    /// @brief Its priority.
    std::uint32_t priority{};
    /// @brief `h<address>` for hosts, `s<address>x<server>` for reflexive.
    std::string foundation;
    /// @brief When its mapping is next refreshed, monotonic microseconds.
    std::uint64_t refresh_at_us{};
    /// @brief Whether it is still refreshed; only reflexive candidates of
    /// the nominated base keep refreshing.
    bool refreshing{};
  };

  /// @brief The span of public ports the peer reported for one base and
  /// public address.
  struct MappedPorts {
    /// @brief The peer's base.
    platform::SocketEndpoint related;
    /// @brief The public address.
    platform::IpAddress address;
    /// @brief The lowest priority among them.
    std::uint32_t priority{};
    /// @brief The lowest port seen.
    std::int32_t low{};
    /// @brief The highest port seen.
    std::int32_t high{};
  };

  /// @brief A remote candidate.
  struct RemoteCandidate {
    /// @brief Its address and port.
    platform::SocketEndpoint endpoint;
    /// @brief Its priority; guesses get the base's priority lowered by step
    /// << 8.
    std::uint32_t priority{};
    /// @brief Whether it was learned from an incoming check.
    bool reflexive{};
  };

  /// @brief A candidate pair in the checklist.
  struct Pair {
    /// @brief Unique id, never reused.
    std::uint32_t id{};
    /// @brief Index of the local base.
    std::size_t base{};
    /// @brief The remote endpoint.
    platform::SocketEndpoint remote;
    /// @brief The base's host priority.
    std::uint32_t local_priority{};
    /// @brief The remote candidate's priority.
    std::uint32_t remote_priority{};
    /// @brief The pair priority for the current role.
    std::uint64_t priority{};
    /// @brief Where it stands.
    PairState state{PairState::waiting};
    /// @brief Whether it is in the triggered-check queue.
    bool queued{};
    /// @brief The local address the peer saw, from the successful check.
    std::optional<platform::SocketEndpoint> mapped;
    /// @brief The pair priority with the mapped local candidate, used to
    /// pick the nomination.
    std::uint64_t valid_priority{};
    /// @brief Whether the controlled agent nominates it once its own check
    /// succeeds.
    bool nominate_on_success{};
    /// @brief Timed-out nominating checks so far.
    std::size_t nomination_tries{};
  };

  /// @brief A STUN request awaiting its answer, with its retransmission
  /// schedule.
  struct Transaction {
    /// @brief The transaction id, unique among those in flight.
    stun::TransactionId id{};
    /// @brief What it is for.
    TransactionKind kind{TransactionKind::check};
    /// @brief The base it is sent from.
    std::size_t base{};
    /// @brief Where it is sent; answers from elsewhere are not symmetric.
    platform::SocketEndpoint remote;
    /// @brief The pair checked, for checks.
    std::uint32_t pair{};
    /// @brief The server queried, for gather and refresh.
    std::size_t server{};
    /// @brief The local candidate refreshed, for refresh.
    std::size_t candidate{};
    /// @brief The role claimed in the request, to judge a role-conflict
    /// answer.
    Role role_sent{Role::controlling};
    /// @brief Whether it nominates.
    bool use_candidate{};
    /// @brief Whether it is no longer retransmitted but still awaits an
    /// answer.
    bool cancelled{};
    /// @brief The encoded request.
    std::vector<std::byte> bytes;
    /// @brief Transmissions so far.
    std::size_t sent{};
    /// @brief Transmissions allowed.
    std::size_t limit{};
    /// @brief Initial retransmission timeout, doubled after each send, in
    /// microseconds.
    std::uint64_t rto_us{};
    /// @brief Wait after the last send, in microseconds.
    std::uint64_t final_wait_us{};
    /// @brief When it is next sent or expires, monotonic microseconds.
    std::uint64_t deadline_us{};
  };

  /// @brief An authenticated check that arrived before connect().
  struct EarlyRequest {
    /// @brief The base it arrived on.
    std::size_t base{};
    /// @brief Who sent it.
    platform::SocketEndpoint source;
    /// @brief The datagram.
    SecretBytes bytes;
  };

  /// @brief Random bytes behind a ufrag.
  static constexpr std::size_t ufrag_bytes = ufrag_text_size / 2;
  /// @brief Random bytes behind a password.
  static constexpr std::size_t password_bytes = password_text_size / 2;
  /// @brief Random bytes of the tie-breaker.
  static constexpr std::size_t tie_breaker_bytes = stun::tie_breaker_size;
  /// @brief The one ICE component.
  static constexpr std::uint32_t component_id = 1;
  /// @brief Most consent checks awaiting answers; the oldest is dropped.
  static constexpr std::size_t max_consent_outstanding = 8;
  /// @brief Draws tried for a transaction id not already in flight.
  static constexpr std::size_t transaction_draw_attempts = 4;

  /// @brief Lower-case hex of bytes, kept secret.
  ///
  /// @param bytes The bytes.
  /// @return The hex text.
  static auto hex_of(std::span<const std::byte> bytes) -> SecretString;
  /// @brief An RFC 8445 candidate priority.
  ///
  /// @param type_preference The type preference.
  /// @param local_preference The base's local preference.
  /// @return (type << 24) | (local << 8) | 255.
  static constexpr auto candidate_priority(
      std::uint32_t type_preference, std::uint16_t local_preference) noexcept
      -> std::uint32_t {
    return (type_preference << type_preference_shift) |
           (std::uint32_t{local_preference} << local_preference_shift) |
           (component_preference_range - component_id);
  }
  /// @brief Whether two endpoints share an address family.
  ///
  /// @param left One endpoint.
  /// @param right The other.
  /// @return True when they do.
  static auto same_family(const platform::SocketEndpoint& left,
                          const platform::SocketEndpoint& right) noexcept
      -> bool;
  /// @brief Whether an endpoint would be a valid host candidate.
  ///
  /// @param endpoint The endpoint.
  /// @return True when it passes candidate validation.
  static auto usable_host(const platform::SocketEndpoint& endpoint) -> bool;
  /// @brief Whether a mapping would be a valid reflexive candidate of a base.
  ///
  /// @param base The base.
  /// @param mapped The mapping.
  /// @return True when it passes candidate validation.
  static auto usable_reflexive(const platform::SocketEndpoint& base,
                               const platform::SocketEndpoint& mapped) -> bool;
  /// @brief Orders bases by preference.
  ///
  /// @param left One base.
  /// @param right The other.
  /// @return True when `left` ranks first: higher preference, then lower
  ///     address
  ///     text, then lower port.
  static auto preferred(const Base& left, const Base& right) -> bool;
  /// @brief The integrity key of a password: its bytes.
  ///
  /// @param password The password.
  /// @return A view of it.
  static auto key_of(const SecretString& password) noexcept
      -> std::span<const std::byte>;
  /// @brief Fills bytes with randomness.
  ///
  /// @param output The bytes.
  /// @return Nothing on success.
  /// @retval Error::crypto_unavailable The source failed or threw.
  [[nodiscard]] auto draw(std::span<std::byte> output) noexcept -> Result<void>;

  /// @brief Sets up an empty agent.
  ///
  /// @param crypto The crypto provider.
  /// @param random The random source.
  /// @param policy The side it serves; host starts controlling.
  /// @param now_us The current time; gathering ends 15 seconds later.
  Agent(platform::CryptoProvider& crypto,
        platform::SecureRandomProvider& random, Policy policy,
        std::uint64_t now_us);

  /// @brief Draws credentials and tie-breaker, ranks the bases and makes the
  /// host candidates.
  ///
  /// @param config The configuration.
  /// @return Nothing on success.
  /// @retval Error::internal Duplicate socket ids.
  /// @retval Error::crypto_unavailable Randomness failed.
  [[nodiscard]] auto initialise(AgentConfig config) -> Result<void>;
  /// @brief Queues a query to every server for every base of its family.
  ///
  /// @param now_us The current time.
  void start_gathering(std::uint64_t now_us);
  /// @brief Ends gathering and queues the Gathered event with up to 32
  /// candidates, best first; starts refreshing reflexive mappings.
  ///
  /// @param now_us The current time.
  void finish_gathering(std::uint64_t now_us);
  /// @brief Whether to ask the peer for port guesses.
  ///
  /// @return True when forced and any reflexive candidate exists, or when one
  ///     base mapped to one public address with different ports.
  auto wants_guesses() const noexcept -> bool;
  /// @brief Groups the peer's reflexive candidates by base and public
  /// address.
  ///
  /// @param candidates The peer's candidates.
  /// @return The port span of each group.
  static auto mapped_ports(std::span<const Candidate> candidates)
      -> std::vector<MappedPorts>;
  /// @brief Adds guessed remote endpoints one step further above and below
  /// each observed port span, up to the peer's port-guess count.
  ///
  /// @param remote The peer's parameters.
  void guess_remote_mappings(const Parameters& remote);
  /// @brief Wraps a guessed port back into the ephemeral range.
  ///
  /// @param port The guessed port, possibly out of range.
  /// @return Above 65535 wraps from the ephemeral start; below the port floor
  ///     wraps down from 65535.
  static constexpr auto wrapped_port(std::int32_t port) noexcept
      -> std::uint16_t {
    if (port > max_port)
      port = holepunch_ephemeral_start + (port - max_port - 1);
    else if (port < holepunch_port_floor)
      port = max_port - (holepunch_port_floor - port);
    return static_cast<std::uint16_t>(port);
  }
  /// @brief Fails the agent: drops its transactions and queues Failed once.
  ///
  /// @param error Why.
  void stop(Error error);

  /// @brief The key that authenticates checks sent to this agent.
  ///
  /// @return The local password's bytes.
  auto local_key() const noexcept -> std::span<const std::byte>;
  /// @brief The key that authenticates checks this agent sends.
  ///
  /// @return The remote password's bytes.
  auto remote_key() const noexcept -> std::span<const std::byte>;
  /// @brief Finds a base by socket id.
  ///
  /// @param socket The socket id.
  /// @return Its index; nothing when unknown.
  [[nodiscard]] auto find_base(SocketId socket) const noexcept
      -> std::optional<std::size_t>;
  /// @brief Finds the pair of a base and remote endpoint.
  ///
  /// @param base The base index.
  /// @param remote The remote endpoint.
  /// @return The pair; null when none.
  auto find_pair(std::size_t base,
                 const platform::SocketEndpoint& remote) noexcept -> Pair*;
  /// @brief Finds a pair by id.
  ///
  /// @param id The id.
  /// @return The pair; null when gone.
  auto pair_by_id(std::uint32_t id) noexcept -> Pair*;
  /// @brief Finds a transaction in flight.
  ///
  /// @param id The transaction id.
  /// @return Its position; end when none.
  auto find_transaction(const stun::TransactionId& id) noexcept
      -> std::vector<Transaction>::iterator;
  /// @brief Draws a transaction id not already in flight.
  ///
  /// @return The id.
  /// @retval Error::crypto_unavailable Randomness failed.
  /// @retval Error::internal Four draws all collided.
  [[nodiscard]] auto fresh_transaction_id() -> Result<stun::TransactionId>;
  /// @brief Translates a STUN encoding failure.
  ///
  /// @param error The failure.
  /// @return protocol, crypto_unavailable or internal.
  static auto encoding_failure(stun::MessageError error) noexcept -> Error;
  /// @brief The host candidate priority of a base.
  ///
  /// @param base The base index.
  /// @return The priority.
  auto host_priority(std::size_t base) const noexcept -> std::uint32_t;
  /// @brief The peer-reflexive priority of a base, sent as PRIORITY.
  ///
  /// @param base The base index.
  /// @return The priority.
  auto reflexive_priority(std::size_t base) const noexcept -> std::uint32_t;
  /// @brief The RFC 8445 pair priority for the current role.
  ///
  /// @param local The local priority.
  /// @param remote The remote priority.
  /// @return 2^32 * min + 2 * max + (controlling > controlled).
  auto pair_priority(std::uint32_t local, std::uint32_t remote) const noexcept
      -> std::uint64_t;
  /// @brief The priority of the local candidate a check's mapping matches.
  ///
  /// @param base The base index.
  /// @param mapped The mapping.
  /// @return The matching local candidate's priority, or the peer-reflexive
  ///     priority when none matches.
  auto mapped_priority(std::size_t base,
                       const platform::SocketEndpoint& mapped) const noexcept
      -> std::uint32_t;
  /// @brief Adds a pair for a newly learned remote, evicting the lowest idle
  /// pair when the checklist is full.
  ///
  /// @param base The base index.
  /// @param remote The remote candidate.
  /// @return The pair; null when full of pairs at least as good or busy.
  auto add_pair(std::size_t base, const RemoteCandidate& remote) -> Pair*;
  /// @brief Builds the checklist: every same-family base and remote, best 100
  /// first.
  void form_pairs();
  /// @brief Recomputes every pair priority after a role change.
  void reprioritise() noexcept;
  /// @brief Changes role; a controlled agent abandons any nomination.
  ///
  /// @param role The new role.
  void switch_role(Role role) noexcept;

  [[nodiscard]] auto
  /// @brief Encodes a check: USERNAME `remote:local`, PRIORITY, the role
  /// attribute with the tie-breaker, optional USE-CANDIDATE, integrity with
  /// the
  /// remote password and a fingerprint.
  ///
  /// @param base The base index.
  /// @param use_candidate Whether it nominates.
  /// @param id The transaction id.
  /// @return The request.
  /// @retval Error Encoding failed.
  build_request(std::size_t base, bool use_candidate,
                const stun::TransactionId& id)
      -> Result<std::vector<std::byte>>;
  /// @brief Answers a check, with XOR-MAPPED-ADDRESS or an error, integrity
  /// with the local password and a fingerprint.
  ///
  /// @param base The base it arrived on.
  /// @param source Who sent it.
  /// @param request The request.
  /// @param error The error to send; nothing for success.
  /// @param unknown The unknown required attributes, for a 420.
  /// @param outgoing Receives the response.
  void respond(std::size_t base, const platform::SocketEndpoint& source,
               const stun::Message& request,
               std::optional<stun::ErrorResponse> error,
               std::vector<std::uint16_t> unknown,
               std::vector<Outgoing>& outgoing);
  /// @brief Sends a transaction and schedules its next send or expiry.
  ///
  /// @param transaction The transaction.
  /// @param now_us The current time.
  /// @param outgoing Receives the datagram.
  void transmit(Transaction& transaction, std::uint64_t now_us,
                std::vector<Outgoing>& outgoing);
  /// @brief Queues a Binding request to a STUN server, due now.
  ///
  /// @param kind gather or refresh.
  /// @param base The base index.
  /// @param server The server index.
  /// @param candidate The candidate refreshed, for refresh.
  /// @param now_us The current time.
  void start_server_query(TransactionKind kind, std::size_t base,
                          std::size_t server, std::size_t candidate,
                          std::uint64_t now_us);
  /// @brief Sends the first transmission of a check on a pair.
  ///
  /// Its timeout is the larger of 500 ms and 50 ms per active pair.
  ///
  /// @param pair The pair.
  /// @param use_candidate Whether it nominates.
  /// @param now_us The current time.
  /// @param outgoing Receives the datagram.
  /// @return False when failed, out of budget, inside the 10 ms floor, or the
  ///     request could not be built.
  [[nodiscard]] auto start_check(Pair& pair, bool use_candidate,
                                 std::uint64_t now_us,
                                 std::vector<Outgoing>& outgoing) -> bool;
  /// @brief Stops retransmitting a pair's checks, keeping them open for one
  /// more timeout.
  ///
  /// @param pair The pair id.
  /// @param now_us The current time.
  void cancel_pair_transactions(std::uint32_t pair, std::uint64_t now_us);
  /// @brief Whether a pair has a check in flight.
  ///
  /// @param pair The pair id.
  /// @return True when it does.
  auto pair_busy(std::uint32_t pair) const noexcept -> bool;
  /// @brief Whether a pair has a nominating check in flight.
  ///
  /// @param pair The pair id.
  /// @return True when it does.
  auto nomination_in_flight(std::uint32_t pair) const noexcept -> bool;
  /// @brief Whether any check is in flight.
  ///
  /// @return True when one is.
  auto pair_checks_active() const noexcept -> bool;
  /// @brief Queues a triggered check on a pair, at most once.
  ///
  /// @param pair The pair.
  void enqueue_triggered(Pair& pair);

  /// @brief Authenticates an incoming check and processes it, or holds it
  /// until connect().
  ///
  /// Requests without fingerprint, integrity, username, a non-zero priority
  /// or
  /// exactly one role attribute, or for another ufrag, are ignored.
  ///
  /// @param base The base it arrived on.
  /// @param source Who sent it.
  /// @param datagram The datagram.
  /// @param decoded Its decoding.
  /// @param now_us The current time.
  /// @param outgoing Receives the response.
  void handle_request(std::size_t base, const platform::SocketEndpoint& source,
                      std::span<const std::byte> datagram,
                      const stun::ReceivedMessage& decoded,
                      std::uint64_t now_us, std::vector<Outgoing>& outgoing);
  /// @brief Answers an authenticated check, resolves role conflicts, learns
  /// peer-reflexive candidates and triggers a check back.
  ///
  /// @param base The base it arrived on.
  /// @param source Who sent it.
  /// @param decoded Its decoding.
  /// @param now_us The current time.
  /// @param outgoing Receives the response.
  void process_request(std::size_t base, const platform::SocketEndpoint& source,
                       const stun::ReceivedMessage& decoded,
                       std::uint64_t now_us, std::vector<Outgoing>& outgoing);
  /// @brief Matches a response to its transaction and dispatches it.
  ///
  /// @param base The base it arrived on.
  /// @param source Who sent it.
  /// @param datagram The datagram.
  /// @param decoded Its decoding.
  /// @param now_us The current time.
  void handle_response(std::size_t base, const platform::SocketEndpoint& source,
                       std::span<const std::byte> datagram,
                       const stun::ReceivedMessage& decoded,
                       std::uint64_t now_us);
  /// @brief Handles a STUN server's answer: learns a candidate while
  /// gathering;
  /// on refresh, fails with no_path when the mapping changed before
  /// nomination.
  ///
  /// @param transaction The finished transaction.
  /// @param mapped The mapping, if the answer was a success.
  /// @param now_us The current time.
  void handle_server_response(Transaction transaction,
                              std::optional<platform::SocketEndpoint> mapped,
                              std::uint64_t now_us);
  /// @brief Handles a check's answer: marks the pair valid or failed,
  /// switches
  /// role on a 487, and nominates when due.
  ///
  /// @param transaction The finished transaction.
  /// @param symmetric Whether it came from where it was sent.
  /// @param decoded Its decoding.
  /// @param now_us The current time.
  void handle_check_response(Transaction transaction, bool symmetric,
                             const stun::ReceivedMessage& decoded,
                             std::uint64_t now_us);
  /// @brief Adds a server-reflexive candidate while gathering, unless known
  /// or
  /// invalid.
  ///
  /// @param base The base index.
  /// @param server The server index.
  /// @param mapped The mapping.
  void learn_reflexive(std::size_t base, std::size_t server,
                       const platform::SocketEndpoint& mapped);
  /// @brief Fixes the nominated path, stops checks, starts consent and queues
  /// Nominated.
  ///
  /// @param pair The valid pair.
  /// @param now_us The current time.
  void nominate(Pair& pair, std::uint64_t now_us);

  /// @brief Retransmits due transactions and expires finished ones.
  ///
  /// @param now_us The current time.
  /// @param outgoing Receives the datagrams.
  void run_transactions(std::uint64_t now_us, std::vector<Outgoing>& outgoing);
  /// @brief Fails the pair of an expired check, allowing a nominating check
  /// three tries.
  ///
  /// @param transaction The expired transaction.
  void expire(const Transaction& transaction);
  /// @brief Starts due mapping refreshes.
  ///
  /// @param now_us The current time.
  void run_refreshes(std::uint64_t now_us);
  /// @brief Processes checks held from before connect() that name the peer's
  /// ufrag.
  ///
  /// @param now_us The current time.
  /// @param outgoing Receives the responses.
  void run_early_requests(std::uint64_t now_us,
                          std::vector<Outgoing>& outgoing);
  /// @brief Picks the pair to nominate once the settle time has passed.
  ///
  /// @param now_us The current time.
  void run_nomination(std::uint64_t now_us);
  /// @brief Sends the next check: the nomination, a triggered check, or the
  /// best waiting pair; at most one per 50 ms.
  ///
  /// @param now_us The current time.
  /// @param outgoing Receives the datagram.
  void run_checks(std::uint64_t now_us, std::vector<Outgoing>& outgoing);
  /// @brief For a guest, rearms failed pairs 5 seconds after a round failed.
  ///
  /// @param now_us The current time.
  void run_rearm(std::uint64_t now_us);
  /// @brief Sends a due consent check, or fails with no_path when consent
  /// expired.
  ///
  /// @param now_us The current time.
  /// @param outgoing Receives the datagram.
  void run_consent(std::uint64_t now_us, std::vector<Outgoing>& outgoing);
  /// @brief The valid pair with the highest valid priority.
  ///
  /// @return Its index; nothing when none is valid.
  auto best_valid_index() const noexcept -> std::optional<std::size_t>;
  /// @brief Whether an unchecked pair might beat a priority.
  ///
  /// @param priority The priority to beat.
  /// @return True when a waiting or in-progress pair has a higher one.
  auto higher_pending(std::uint64_t priority) const noexcept -> bool;
  /// @brief When the controlling agent may nominate.
  ///
  /// @return The first valid time, plus 1 second when better pairs are
  ///     pending;
  ///     nothing when not controlling, already nominating, or no pair is
  ///     valid.
  auto settle_deadline_us() const noexcept -> std::optional<std::uint64_t>;
  /// @brief Whether a checklist round ended with failures and nothing left.
  ///
  /// @return True when no check is queued or in flight and some pair failed.
  auto round_finished() const noexcept -> bool;
  /// @brief Whether a check is waiting to be sent.
  ///
  /// @return True for an unsent nomination or a waiting pair.
  auto check_work_pending() const noexcept -> bool;
  /// @brief Whether more checks may be sent.
  ///
  /// @return True below max_check_requests.
  auto check_budget() const noexcept -> bool;
  /// @brief Draws the next consent interval.
  ///
  /// @return 4 to 6 seconds, in microseconds.
  /// @retval Error::crypto_unavailable Randomness failed.
  [[nodiscard]] auto consent_interval_us() -> Result<std::uint64_t>;

  /// @brief Computes STUN integrity.
  platform::CryptoProvider& crypto_;
  /// @brief Draws randomness.
  platform::SecureRandomProvider& random_;
  /// @brief The side the agent serves.
  Policy policy_;
  /// @brief When to ask for port guesses.
  PortGuessing guessing_{PortGuessing::automatic};
  /// @brief The current role.
  Role role_;
  /// @brief The random tie-breaker for role conflicts; zeroed on close.
  std::uint64_t tie_breaker_{};
  /// @brief The local ufrag.
  SecretString local_ufrag_;
  /// @brief The local password.
  SecretString local_password_;
  /// @brief The peer's ufrag, from connect().
  SecretString remote_ufrag_;
  /// @brief The peer's password, from connect().
  SecretString remote_password_;
  /// @brief The ranked bases.
  std::vector<BaseState> bases_;
  /// @brief The STUN servers.
  std::vector<platform::SocketEndpoint> servers_;
  /// @brief The local candidates.
  std::vector<LocalCandidate> locals_;
  /// @brief The remote candidates, given, guessed and learned.
  std::vector<RemoteCandidate> remotes_;
  /// @brief The checklist.
  std::vector<Pair> pairs_;
  /// @brief Pair ids queued for triggered checks.
  std::deque<std::uint32_t> triggered_;
  /// @brief The transactions in flight.
  std::vector<Transaction> transactions_;
  /// @brief Checks held from before connect().
  std::deque<EarlyRequest> early_;
  /// @brief The next pair id.
  std::uint32_t next_pair_id_{1};
  /// @brief When gathering ends regardless, monotonic microseconds.
  std::uint64_t gathering_deadline_us_{};
  /// @brief Progress, failure, closure, held checks and consent, with the
  /// events not yet taken.
  AgentLifecycle lifecycle_;
  /// @brief The nominated path.
  std::optional<Path> nominated_;
  /// @brief The host's nomination deadline.
  std::optional<std::uint64_t> connectivity_deadline_us_;
  /// @brief When the first pair became valid.
  std::optional<std::uint64_t> first_valid_us_;
  /// @brief The pair being nominated, by id.
  std::optional<std::uint32_t> nominating_;
  /// @brief When a guest rearms its failed pairs.
  std::optional<std::uint64_t> rearm_at_us_;
  /// @brief When the next ordinary check may be sent.
  std::uint64_t next_check_us_{};
  /// @brief When any check transmission may next happen, after the 10 ms
  /// floor.
  std::uint64_t next_check_transmit_us_{};
  /// @brief Check transmissions so far.
  std::size_t check_requests_sent_{};
  /// @brief When connect() succeeded.
  std::uint64_t connect_us_{};
  /// @brief When consent was last confirmed.
  std::uint64_t last_consent_us_{};
  /// @brief When the next consent check is sent.
  std::uint64_t next_consent_us_{};
};

}  // namespace libchiaking::net::nat::ice
