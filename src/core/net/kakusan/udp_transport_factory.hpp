/// @file
/// @brief Declares UdpTransportFactory, which makes Kakusan's UDP spoke
/// transports (ICE plus DTLS over platform UDP sockets), the settings and
/// services they share, and the poller that waits on all their sockets.
#pragma once

#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/platform/byte_units.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/net/types.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/kakusan/datagram_security.hpp"
#include "src/core/net/kakusan/peer_link.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/net/nat/ice/agent.hpp"
#include "src/core/net/nat/ice/parameters.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace libchiaking::net::kakusan {

/// @brief Most local addresses a spoke binds sockets on.
inline constexpr std::size_t max_local_addresses = 8;
/// @brief Most datagrams read in one pump before yielding.
inline constexpr std::size_t max_datagrams_per_pump = 256;
/// @brief Bytes read per datagram.
inline constexpr std::size_t receive_buffer_size = 2048;
/// @brief Kernel receive buffer asked for each socket: 1 MiB.
inline constexpr std::size_t socket_receive_buffer_bytes = bytes_per_mib;

/// @brief Opens a spoke's DTLS association for a role.
using DatagramSecurityFactory =
    std::function<Result<std::unique_ptr<DatagramSecurity>>(SecurityRole)>;

/// @brief The platform services UDP spoke transports use; each must outlive the
/// factory and its transports.
struct UdpTransportDependencies {
  /// @brief Interfaces, sockets and pollers.
  platform::NetworkProvider& network;
  /// @brief STUN integrity.
  platform::CryptoProvider& crypto;
  /// @brief ICE randomness.
  platform::SecureRandomProvider& random;
  /// @brief Opens DTLS associations; must be set.
  DatagramSecurityFactory security;
};

/// @brief How UDP spoke transports gather.
struct UdpTransportConfig {
  /// @brief Resolved STUN servers; see resolve_stun_servers().
  std::vector<platform::SocketEndpoint> stun_servers;
  /// @brief Whether IPv6 addresses are used.
  bool enable_ipv6{false};
  /// @brief When to ask for port guesses.
  nat::ice::PortGuessing guessing{nat::ice::PortGuessing::automatic};
};

/// @brief Longest single wait, in microseconds: 60 seconds.
inline constexpr std::uint64_t max_wait_us = 60'000'000;

/// @brief How a wait ended.
enum class WaitResult : std::uint8_t {
  /// @brief A registered socket has a datagram waiting.
  readable,
  /// @brief wake() was called.
  woken,
  /// @brief The timeout passed.
  timed_out,
  /// @brief There is no poller, or the wait failed.
  unavailable,
};

/// @brief Makes UDP spoke transports that share one poller, and waits on all
/// their sockets at once.
///
/// The thread that pumps the transports calls wait(); wake() may come from any
/// thread. Not copyable or movable.
class UdpTransportFactory final : public SpokeTransportFactory {
 public:
  /// @brief Resolves user-configured STUN targets to endpoints.
  ///
  /// Blocks for DNS lookups. Invalid targets, bracketed non-IPv6 text and
  /// failed
  /// lookups are skipped; IPv4-mapped addresses are unmapped and duplicates
  /// dropped.
  ///
  /// @param dns Resolves host names.
  /// @param servers The targets, as StunTarget text.
  /// @return Up to 4 endpoints, in order.
  static auto resolve_stun_servers(platform::DnsProvider& dns,
                                   std::span<const std::string> servers)
      -> std::vector<platform::SocketEndpoint>;

  /// @brief Makes the factory and its shared poller.
  ///
  /// @param dependencies The platform services.
  /// @param config The gathering settings.
  UdpTransportFactory(UdpTransportDependencies dependencies,
                      UdpTransportConfig config);
  /// @brief Releases the poller once no transport holds it.
  ~UdpTransportFactory() override;

  /// @brief Not copyable.
  UdpTransportFactory(const UdpTransportFactory&) = delete;
  /// @brief Not movable.
  UdpTransportFactory(UdpTransportFactory&&) = delete;
  /// @brief Not copyable.
  ///
  /// @return Never returns.
  auto operator=(const UdpTransportFactory&) -> UdpTransportFactory& = delete;
  /// @brief Not movable.
  ///
  /// @return Never returns.
  auto operator=(UdpTransportFactory&&) -> UdpTransportFactory& = delete;

  /// @brief Makes a transport sharing this factory's poller.
  ///
  /// @param role The side it serves.
  /// @return The transport, not yet gathering.
  /// @retval Error::internal No DTLS factory was given.
  [[nodiscard]] auto create(TransportRole role)
      -> Result<std::unique_ptr<SpokeTransport>> override;
  /// @brief Waits on every transport's sockets.
  ///
  /// Blocks for up to the smaller of `timeout_us` and 60 seconds.
  ///
  /// @param timeout_us Longest wait, in microseconds.
  /// @return How the wait ended.
  [[nodiscard]] auto wait(std::uint64_t timeout_us) -> WaitResult;
  /// @brief Ends the current or next wait. Safe from any thread.
  void wake() noexcept;

 private:
  /// @brief The set of UDP sockets one Kakusan peer waits on, and the poller
  /// that waits on them.
  ///
  /// Sockets are borrowed and must be detached before they are destroyed. One
  /// thread waits; wake() may be called from any thread.
  class SocketRegistry {
   public:
    /// @brief Takes the poller.
    ///
    /// @param poller The poller; null makes every wait unavailable.
    explicit SocketRegistry(std::unique_ptr<platform::Poller> poller) noexcept;

    /// @brief Adds a socket to wait on.
    ///
    /// @param socket The socket; must stay alive until detached.
    void attach(const platform::UdpSocket& socket);
    /// @brief Stops waiting on a socket.
    ///
    /// @param socket The socket.
    void detach(const platform::UdpSocket& socket) noexcept;
    /// @brief Waits until a socket is readable, wake() is called or the timeout
    /// passes.
    ///
    /// Blocks for up to the smaller of `timeout_us` and 60 seconds.
    ///
    /// @param timeout_us Longest wait, in microseconds.
    /// @return How the wait ended.
    auto wait(std::uint64_t timeout_us) -> WaitResult;
    /// @brief Ends the current or next wait early. Safe from any thread.
    void wake() noexcept;

   private:
    /// @brief The poller; may be null.
    std::unique_ptr<platform::Poller> poller_;
    /// @brief The borrowed sockets.
    std::vector<const platform::UdpSocket*> sockets_;
    /// @brief Scratch requests for one wait.
    std::vector<platform::PollRequest> requests_;
  };

  /// @brief A STUN server named by the user for Kakusan's ICE gathering, as
  /// `host`, `host:port`, `[ipv6]`, `[ipv6]:port` or a bare IPv6 literal.
  ///
  /// Holds a view into the parsed text, which must outlive it. Plain data.
  class StunTarget {
   public:
    /// @brief The STUN port used when a target names none.
    static constexpr std::uint16_t default_stun_port = 3478;
    /// @brief Longest STUN host name, in bytes.
    static constexpr std::size_t max_stun_host_size = 253;

    /// @brief Reads a target.
    ///
    /// @param text The text.
    /// @return The target; nothing when the text is too long, malformed, has an
    ///     empty or overlong host, or a port that is not 1 to 65535.
    [[nodiscard]] static auto parse(std::string_view text)
        -> std::optional<StunTarget>;

    /// @brief The host without brackets; for a bare IPv6 literal, the whole
    /// text.
    std::string_view host;
    /// @brief The port; 3478 when none was written.
    std::uint16_t port{default_stun_port};
    /// @brief Whether the host was written in brackets.
    bool bracketed{};
    /// @brief Whether the host is an IPv6 literal, bracketed or bare, and needs
    /// no DNS.
    bool literal{};

   private:
    /// @brief Most digits a port may have.
    static constexpr std::size_t max_port_digits = 5;
  };

  /// @brief A SpokeTransport over platform UDP sockets: binds one socket per
  /// usable local address, runs the ICE agent over them, then DTLS on the
  /// nominated pair.
  ///
  /// Received datagrams are demultiplexed: STUN to the agent, RCPM on the
  /// nominated pair surfaced as Datagram events once DTLS is established,
  /// anything else from the pair to DTLS. Up to 256 datagrams are read per
  /// pump, round-robin over the sockets. Sends stop as soon as consent expires.
  /// Not copyable or movable; one thread drives it.
  class UdpSpokeTransport final : public SpokeTransport {
   public:
    /// @brief Prepares a transport; nothing is bound until gather().
    ///
    /// @param dependencies The platform services; must outlive the transport.
    /// @param config The gathering settings.
    /// @param registry The shared poller its sockets join.
    /// @param role The side it serves; the host is the DTLS client.
    UdpSpokeTransport(const UdpTransportDependencies& dependencies,
                      const UdpTransportConfig& config,
                      std::shared_ptr<SocketRegistry> registry,
                      TransportRole role);

    /// @brief Closes the transport.
    ~UdpSpokeTransport() override;

    /// @brief Not copyable.
    UdpSpokeTransport(const UdpSpokeTransport&) = delete;
    /// @brief Not movable.
    UdpSpokeTransport(UdpSpokeTransport&&) = delete;
    /// @brief Not copyable.
    ///
    /// @return Never returns.
    auto operator=(const UdpSpokeTransport&) -> UdpSpokeTransport& = delete;
    /// @brief Not movable.
    ///
    /// @return Never returns.
    auto operator=(UdpSpokeTransport&&) -> UdpSpokeTransport& = delete;

    /// @brief Binds the sockets, opens DTLS and starts the ICE agent gathering.
    ///
    /// With no socket bound the transport fails with no_path through pump().
    ///
    /// @param now_us The current monotonic time.
    /// @param credentials Derived ICE credentials, or nothing for fresh ones.
    /// @return Nothing on success.
    /// @retval Error::internal Closed, already gathering, or DTLS could not be
    ///     opened.
    /// @retval Error The agent could not start.
    auto gather(std::uint64_t now_us, std::optional<IceCredentials> credentials)
        -> Result<void> override;

    /// @brief Starts ICE checks to the peer and remembers its certificate
    /// digest.
    ///
    /// @param remote The peer's description.
    /// @param now_us The current monotonic time.
    /// @return Nothing on success.
    /// @retval Error::no_path The transport failed.
    /// @retval Error::internal Closed, not gathered, or already connected.
    /// @retval Error::invalid_block The peer's ICE parameters are invalid.
    auto connect(const TransportDescription& remote, std::uint64_t now_us)
        -> Result<void> override;

    /// @brief Reads the sockets, runs the ICE and DTLS timers and reports
    /// events.
    ///
    /// @param now_us The current monotonic time.
    /// @param events Receives the events; failures arrive as Failed.
    /// @return Always nothing.
    auto pump(std::uint64_t now_us, std::vector<TransportEvent>& events)
        -> Result<void> override;

    /// @brief When pump() must next run.
    ///
    /// @return Now when events or unread datagrams wait; otherwise the earlier
    ///     of
    ///     the agent's and DTLS's deadlines; nothing when failed or closed.
    auto next_deadline_us() const noexcept
        -> std::optional<std::uint64_t> override;

    /// @brief Encrypts and sends a record on the nominated pair.
    ///
    /// @param record The record.
    /// @return Nothing on success.
    /// @retval Error::no_path Not established or no consent.
    /// @retval Error::internal Empty or too large.
    /// @retval Error DTLS refused; the transport fails.
    auto send_record(std::span<const std::byte> record)
        -> Result<void> override;

    /// @brief Sends an RCPM datagram on the nominated pair; a socket error is
    /// ignored.
    ///
    /// @param datagram The datagram.
    /// @return Nothing on success.
    /// @retval Error::no_path Not established or no consent.
    /// @retval Error::internal Not RCPM or larger than 1200 bytes.
    auto send_datagram(std::span<const std::byte> datagram)
        -> Result<void> override;

    /// @brief Exports keying material from DTLS.
    ///
    /// @param label The exporter label.
    /// @param context The context value.
    /// @param output Receives the material.
    /// @return Nothing on success.
    /// @retval Error::no_path Not established.
    /// @retval Error The export failed.
    auto export_secret(std::string_view label,
                       std::span<const std::byte> context,
                       std::span<std::byte> output) -> Result<void> override;

    /// @brief Sends a DTLS close when possible and releases everything.
    /// Idempotent.
    void close() noexcept override;

   private:
    /// @brief A bound socket and the agent's id for it.
    struct Bound {
      /// @brief The agent's id, from 1.
      nat::ice::SocketId id{};
      /// @brief The socket.
      std::unique_ptr<platform::UdpSocket> socket;
    };

    /// @brief An event waiting to be reported.
    using Pending =
        std::variant<transport_event::Gathered, transport_event::Nominated,
                     transport_event::Established, transport_event::Record,
                     transport_event::Datagram, transport_event::Failed>;

    /// @brief Translates an ICE failure.
    ///
    /// @param error The failure.
    /// @return invalid_block for invalid parameters, otherwise its counterpart.
    static auto path_failure(nat::ice::Error error) noexcept -> Error;

    /// @brief Turns a held event into a transport event.
    ///
    /// @param pending The held event.
    /// @return The event.
    static auto surfaced(Pending pending) -> TransportEvent;

    /// @brief Whether a local address may be a base.
    ///
    /// @param address The address.
    /// @param ipv6 Whether IPv6 is enabled.
    /// @return True for IPv4, or IPv6 when enabled, that the candidate policy
    ///     permits.
    static auto permitted_local(const platform::IpAddress& address,
                                bool ipv6) noexcept -> bool;

    /// @brief Ranks an interface's medium.
    ///
    /// @param info The interface.
    /// @return 0 Ethernet, 1 wireless, 2 unclassified, 3 not a LAN adapter.
    static auto medium_rank(const platform::InterfaceInfo& info) noexcept
        -> unsigned;

    /// @brief Chooses the local addresses to bind, best first: by medium, IPv4
    /// before IPv6.
    ///
    /// @param interfaces Lists the interfaces.
    /// @param ipv6 Whether IPv6 is enabled.
    /// @return Up to 8 distinct addresses; empty when listing failed.
    static auto select_addresses(platform::InterfacesProvider& interfaces,
                                 bool ipv6) -> std::vector<platform::IpAddress>;

    /// @brief Unmaps an IPv4-mapped IPv6 source.
    ///
    /// @param endpoint The source.
    /// @return It with a plain IPv4 address where mapped.
    static auto normalise(platform::SocketEndpoint endpoint) noexcept
        -> platform::SocketEndpoint;

    /// @brief Advances the transport's clock.
    ///
    /// @param now_us The current time; never moves backwards.
    void observe(std::uint64_t now_us) noexcept;

    /// @brief Whether application traffic may be sent.
    ///
    /// @return True when established with consent, not failed or closed.
    auto usable() const noexcept -> bool;

    /// @brief Binds one socket per chosen address on an ephemeral port, with
    /// don't-fragment where supported and a 1 MiB receive buffer.
    ///
    /// @return The agent's bases, preference descending.
    auto bind_sockets() -> std::vector<nat::ice::Base>;

    /// @brief Reads up to 256 datagrams, round-robin, ignoring ICMP refusals.
    void read_sockets();

    /// @brief Routes one datagram.
    ///
    /// @param socket The socket it arrived on.
    /// @param source Who sent it.
    /// @param bytes The datagram.
    void on_datagram(nat::ice::SocketId socket,
                     const platform::SocketEndpoint& source,
                     std::span<const std::byte> bytes);

    /// @brief Polls the agent and DTLS when due.
    void drive_timers();

    /// @brief Turns the agent's events into transport events.
    void drain_agent();

    /// @brief Reports nomination and starts the DTLS handshake.
    void on_nominated();

    /// @brief Sends DTLS output and reports establishment and records.
    ///
    /// @param result The DTLS step's result; a failure fails the transport.
    void after_security(const Result<void>& result);

    /// @brief Sends every datagram DTLS produced.
    void flush_security();

    /// @brief Sends on the nominated pair while consent holds; errors are
    /// ignored.
    ///
    /// @param bytes The datagram.
    void send_on_path(std::span<const std::byte> bytes);

    /// @brief Sends the agent's outgoing STUN messages.
    void transmit();

    /// @brief Finds a socket by id.
    ///
    /// @param id The id.
    /// @return The socket; null when unknown.
    auto find_socket(nat::ice::SocketId id) noexcept -> platform::UdpSocket*;

    /// @brief Fails the transport once, reporting Failed and releasing it.
    ///
    /// @param error Why.
    void stop(Error error);

    /// @brief Fails the transport during gather() without an event.
    ///
    /// @param error Why.
    /// @return The failure.
    auto abandon(Error error) -> Result<void>;

    /// @brief Closes DTLS, the agent and the sockets and wipes the buffer.
    void release() noexcept;

    /// @brief Interfaces and sockets.
    platform::NetworkProvider& network_;
    /// @brief STUN integrity.
    platform::CryptoProvider& crypto_;
    /// @brief ICE randomness.
    platform::SecureRandomProvider& random_;
    /// @brief Opens the DTLS association.
    DatagramSecurityFactory security_factory_;
    /// @brief The STUN servers.
    std::vector<platform::SocketEndpoint> stun_servers_;
    /// @brief Whether IPv6 is used.
    bool enable_ipv6_{};
    /// @brief When to ask for port guesses.
    nat::ice::PortGuessing guessing_{nat::ice::PortGuessing::automatic};
    /// @brief The shared poller.
    std::shared_ptr<SocketRegistry> registry_;
    /// @brief The side served.
    TransportRole role_;
    /// @brief The bound sockets.
    std::vector<Bound> sockets_;
    /// @brief The DTLS association.
    std::unique_ptr<DatagramSecurity> security_;
    /// @brief The ICE agent.
    std::unique_ptr<nat::ice::Agent> agent_;
    /// @brief Events not yet reported.
    std::deque<Pending> pending_;
    /// @brief The agent's outgoing messages.
    std::vector<nat::ice::Outgoing> outgoing_;
    /// @brief The receive buffer, wiped on release.
    SecretArray<receive_buffer_size> buffer_;
    /// @brief The peer's certificate digest.
    Digest256 remote_digest_{};
    /// @brief The latest time seen.
    std::uint64_t now_us_{};
    /// @brief The socket the next read starts at.
    std::size_t next_socket_{};
    /// @brief Whether gather() ran.
    bool started_{};
    /// @brief Whether gathering finished.
    bool gathered_{};
    /// @brief Whether connect() succeeded.
    bool connected_{};
    /// @brief Whether the DTLS handshake started.
    bool securing_{};
    /// @brief Whether DTLS is established.
    bool established_{};
    /// @brief Whether datagrams were left unread.
    bool backlog_{};
    /// @brief Whether the transport failed.
    bool failed_{};
    /// @brief Whether close() ran.
    bool closed_{};
  };

  /// @brief The platform services.
  UdpTransportDependencies dependencies_;
  /// @brief The gathering settings.
  UdpTransportConfig config_;
  /// @brief The poller shared with every transport.
  std::shared_ptr<SocketRegistry> registry_;
};

}  // namespace libchiaking::net::kakusan
