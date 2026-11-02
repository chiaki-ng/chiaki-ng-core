/// @file
/// @brief Declares Beacon, the UDP socket of the console discovery protocol:
/// it sends search and wake-up datagrams to PS4 and PS5 consoles and hands
/// back the replies it receives.
///
/// A search is an HTTP-like `SRCH * HTTP/1.1` datagram naming the discovery
/// protocol version, sent to UDP 987 for a PS4 and 9302 for a PS5. A wake-up
/// is a `WAKEUP * HTTP/1.1` datagram carrying the console's WakeCredential.
/// Replies come back to the beacon's own port and are screened into
/// SearchReply values.
#pragma once

#include "libchiaking/detail/consoles/firmware.hpp"
#include "libchiaking/detail/consoles/host.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/platform/net/interfaces.hpp"
#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/net/socket.hpp"
#include "libchiaking/platform/net/types.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/consoles/adapters/search/search_datagram.hpp"
#include "src/core/consoles/model/console/wake_credential.hpp"
#include "src/core/net/transport/http/sleep_waiter.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::consoles {

/// @brief One discovery socket and the datagrams sent and received on it.
///
/// start() binds the socket to the first free port from first_bind_port to
/// last_bind_port (or any port when none is free) with broadcast enabled
/// when the network can broadcast; stop() closes it. Sends and receives never
/// block; wait() blocks until the socket is readable. Replies, probe outcomes
/// and every datagram received are reported to the Watcher on the calling
/// thread.
///
/// Not safe to share between threads: one thread drives it; only wake() may
/// be called from another. The network provider and the watcher must
/// outlive it.
class Beacon {
 public:
  /// @brief The UDP port a PS4 listens on for searches and wake-ups.
  static constexpr std::uint16_t ps4_port = 987;
  /// @brief The UDP port a PS5 listens on for searches and wake-ups.
  static constexpr std::uint16_t ps5_port = 9302;
  /// @brief The discovery protocol version sent to a PS4.
  static constexpr std::string_view ps4_protocol_version =
      SearchDatagram::ps4_protocol_version;
  /// @brief The discovery protocol version sent to a PS5.
  static constexpr std::string_view ps5_protocol_version =
      SearchDatagram::ps5_protocol_version;
  /// @brief The first local port the beacon tries to bind.
  static constexpr std::uint16_t first_local_port = 9303;
  /// @brief The last local port the beacon tries to bind before taking any
  /// free port.
  static constexpr std::uint16_t last_local_port = 9319;
  /// @brief Longest host name accepted, in bytes.
  static constexpr std::size_t host_name_limit = 255;
  /// @brief Most explicit search targets (addresses and host names together).
  static constexpr std::size_t target_limit = 64;
  /// @brief Most datagrams a caller reads in one round before checking its
  /// deadlines again.
  static constexpr std::size_t datagrams_per_pump = 64;
  /// @brief 255.255.255.255, searched when no targets are given.
  static constexpr platform::IpAddress ipv4_limited_broadcast =
      platform::IpAddress::ipv4({255, 255, 255, 255});
  /// @brief ff02::1, searched when the beacon is bound to IPv6 and no targets
  /// are given.
  static constexpr platform::IpAddress ipv6_all_nodes =
      platform::IpAddress::ipv6(
          {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});

  /// @brief Why a beacon call failed.
  enum class Failure : std::uint8_t {
    /// @brief Too many targets, an unusable host name, or an unusable wake
    /// host.
    invalid_argument,
    /// @brief The beacon is not started, is already started, or its port
    /// range is reversed.
    invalid_state,
    /// @brief No port could be bound, a host did not resolve, interfaces
    /// could not be listed, or a send or receive failed.
    network,
    /// @brief The socket took only part of a datagram.
    short_send,
  };

  /// @brief Why one search datagram was not sent.
  enum class ProbeFailure : std::uint8_t {
    /// @brief The beacon is not started.
    not_started,
    /// @brief The destination is not of the bound address's family.
    address_family_mismatch,
    /// @brief The send failed.
    network,
    /// @brief The socket took only part of the datagram.
    short_send,
    /// @brief The operating system refused the send, as it does without
    /// local network permission.
    permission_denied,
  };

  /// @brief Where the beacon binds and whom it searches.
  struct Settings {
    /// @brief The local address bound; its family is the family searched.
    platform::IpAddress bind_address{platform::IpAddress::any_ipv4()};
    /// @brief The first local port tried; must not exceed #last_bind_port.
    std::uint16_t first_bind_port{first_local_port};
    /// @brief The last local port tried before taking any free port.
    std::uint16_t last_bind_port{last_local_port};
    /// @brief Addresses send_search() goes to; with #target_hosts, at most
    /// target_limit.
    std::vector<platform::IpAddress> targets;
    /// @brief Host names send_search() resolves and goes to; each
    /// non-empty, at most host_name_limit bytes, with no zero byte.
    std::vector<std::string> target_hosts;
    /// @brief Whether send_search() with no targets also goes to each IPv4
    /// interface's broadcast address.
    bool include_interface_broadcasts{true};
    /// @brief The port PS4 datagrams go to.
    std::uint16_t ps4_destination_port{ps4_port};
    /// @brief The port PS5 datagrams go to.
    std::uint16_t ps5_destination_port{ps5_port};
  };

  /// @brief Told what the beacon sends and receives, on the thread that
  /// drives it.
  class Watcher {
   public:
    /// @brief Allows deletion through the interface.
    virtual ~Watcher() = default;
    /// @brief A datagram was admitted as a console's reply.
    ///
    /// @param reply The reply.
    virtual void on_reply(SearchReply reply) = 0;
    /// @brief A search datagram was sent, or failed to be. Does nothing
    /// unless overridden.
    virtual void on_probe(platform::SocketEndpoint,
                          std::optional<ProbeFailure>) {}
    /// @brief A datagram was received, whether or not it was admitted. Does
    /// nothing unless overridden.
    virtual void on_datagram(bool) {}
  };

  /// @brief Whether settings name a usable set of targets.
  ///
  /// @param settings The settings.
  /// @return False when there are more than target_limit targets or a host
  ///     name is empty, too long or holds a zero byte.
  static auto accepts(const Settings& settings) noexcept -> bool;

  /// @brief Sends one wake-up to a console from a fresh beacon, then closes
  /// it.
  ///
  /// Blocks while `host` is resolved.
  ///
  /// @param network The network provider.
  /// @param threading Makes the sleeper the beacon waits on.
  /// @param clock The monotonic clock waits are measured on.
  /// @param host The console's address or host name; resolved to an address
  ///     of the same family as the text (IPv6 when it has a colon).
  /// @param port The destination port; zero means the generation's port.
  /// @param generation Which protocol version and default port to use.
  /// @param credential The console's wake credential.
  /// @return Nothing once sent.
  /// @retval Failure::invalid_argument The host is empty, too long or holds a
  ///     zero byte.
  /// @retval Failure::network The host did not resolve, no port could be
  ///     bound, or the send failed.
  /// @retval Failure::short_send The datagram was cut short.
  [[nodiscard]] static auto wake_host(platform::NetworkProvider& network,
                                      platform::ThreadingProvider& threading,
                                      const platform::ClockProvider& clock,
                                      std::string_view host, std::uint16_t port,
                                      Generation generation,
                                      const WakeCredential& credential)
      -> Expected<void, Failure>;

  /// @brief Makes a beacon with default settings: IPv4, ports 9303 to 9319,
  /// searching by broadcast.
  ///
  /// @param network The network provider; must outlive this object.
  /// @param threading Makes the sleeper the beacon waits on; must outlive
  ///     this object.
  /// @param clock The monotonic clock waits are measured on; must outlive
  ///     this object.
  /// @param watcher Told of replies; must outlive this object.
  Beacon(platform::NetworkProvider& network,
         platform::ThreadingProvider& threading,
         const platform::ClockProvider& clock, Watcher& watcher);
  /// @brief Makes a beacon with the given settings.
  ///
  /// @param network The network provider; must outlive this object.
  /// @param threading Makes the sleeper the beacon waits on; must outlive
  ///     this object.
  /// @param clock The monotonic clock waits are measured on; must outlive
  ///     this object.
  /// @param watcher Told of replies; must outlive this object.
  /// @param settings Where to bind and whom to search.
  Beacon(platform::NetworkProvider& network,
         platform::ThreadingProvider& threading,
         const platform::ClockProvider& clock, Watcher& watcher,
         Settings settings);
  /// @brief Closes the socket, if open.
  ~Beacon();

  /// @brief Not copyable: it owns a socket.
  Beacon(const Beacon&) = delete;
  /// @brief Not copyable.
  auto operator=(const Beacon&) -> Beacon& = delete;

  /// @brief Binds the socket and prepares the search datagrams.
  ///
  /// @return Nothing once bound.
  /// @retval Failure::invalid_state Already started, or the port range is
  ///     reversed.
  /// @retval Failure::invalid_argument accepts() refuses the settings.
  /// @retval Failure::network No socket could be made or bound.
  [[nodiscard]] auto start() -> Expected<void, Failure>;
  /// @brief Sends a PS4 and a PS5 search to every target.
  ///
  /// The targets are the configured addresses and resolved host names; with
  /// none configured, the limited broadcast (or ff02::1 for IPv6) and, when
  /// enabled, each IPv4 interface's broadcast address. A network that cannot
  /// broadcast gets no broadcast targets, so with none configured nothing is
  /// sent. Host names not yet resolved are resolved first, blocking for DNS. A
  /// failure for one destination is reported to the watcher and does not stop
  /// the others.
  ///
  /// @return Nothing when at least one datagram was sent, or there was
  ///     nothing to send.
  /// @retval Failure::invalid_state Not started.
  /// @retval Failure::network A host did not resolve to the bound family,
  ///     interfaces could not be listed, or every send failed.
  /// @retval Failure::short_send Every send failed and the last was cut
  ///     short.
  [[nodiscard]] auto send_search() -> Expected<void, Failure>;
  /// @brief Sends a PS4 and a PS5 search to one address.
  ///
  /// @param destination The address.
  /// @return Nothing when at least one of the two was sent.
  /// @retval ProbeFailure::not_started Not started.
  /// @retval ProbeFailure::address_family_mismatch The address is of another
  ///     family than the bound one.
  /// @retval ProbeFailure::permission_denied The operating system refused
  ///     either send.
  /// @retval ProbeFailure Why the PS5 send failed, when both failed.
  [[nodiscard]] auto send_search_to(platform::IpAddress destination)
      -> Expected<void, ProbeFailure>;
  /// @brief Sends a wake-up to a console's address.
  ///
  /// The datagram holds the credential and is wiped after sending.
  ///
  /// @param destination The console's address.
  /// @param generation Which protocol version and port to use.
  /// @param credential The console's wake credential.
  /// @return Nothing once sent.
  /// @retval Failure::invalid_state Not started.
  /// @retval Failure::network The send failed.
  /// @retval Failure::short_send The datagram was cut short.
  [[nodiscard]] auto send_wakeup(platform::IpAddress destination,
                                 Generation generation,
                                 const WakeCredential& credential)
      -> Expected<void, Failure>;
  /// @brief Resolves a host and sends a wake-up there.
  ///
  /// Blocks while the host is resolved.
  ///
  /// @param host The console's address or host name.
  /// @param generation Which protocol version and port to use.
  /// @param credential The console's wake credential.
  /// @return Nothing once sent.
  /// @retval Failure::invalid_state Not started.
  /// @retval Failure::invalid_argument The host is empty, too long or holds a
  ///     zero byte.
  /// @retval Failure::network The host did not resolve, or the send failed.
  /// @retval Failure::short_send The datagram was cut short.
  [[nodiscard]] auto send_wakeup(std::string_view host, Generation generation,
                                 const WakeCredential& credential)
      -> Expected<void, Failure>;
  /// @brief Receives at most one datagram; see receive_once().
  ///
  /// @return Nothing on success, whether or not a datagram was waiting.
  /// @retval Failure Any failure of receive_once().
  [[nodiscard]] auto pump_once() -> Expected<void, Failure>;
  /// @brief Receives at most one datagram without blocking and reports it.
  ///
  /// The watcher is told of every datagram and, when SearchDatagram::admit
  /// takes it, of the reply.
  ///
  /// @return True when a datagram was read, false when none was waiting.
  /// @retval Failure::invalid_state Not started.
  /// @retval Failure::network The receive failed.
  [[nodiscard]] auto receive_once() -> Expected<bool, Failure>;
  /// @brief Closes the socket; does nothing when not started.
  ///
  /// @return Nothing on success.
  /// @retval Failure::network The socket did not close cleanly; it is dropped
  ///     regardless.
  [[nodiscard]] auto stop() -> Expected<void, Failure>;
  /// @brief The address and port the socket is bound to.
  ///
  /// @return The endpoint.
  /// @retval Failure::invalid_state Not started.
  /// @retval Failure::network The socket could not say.
  [[nodiscard]] auto local_endpoint() const
      -> Expected<platform::SocketEndpoint, Failure>;
  /// @brief Blocks until a datagram is waiting, wake() is called or `timeout`
  /// passes.
  ///
  /// @param timeout The longest wait.
  void wait(std::chrono::milliseconds timeout);
  /// @brief Ends a wait() in progress early. Safe from any thread.
  void wake() noexcept;

 private:
  /// @brief A watcher that ignores everything, for wake_host().
  class Unwatched final : public Watcher {
   public:
    /// @brief Ignores the reply.
    void on_reply(SearchReply) override {}
  };

  /// @brief Builds a search datagram.
  ///
  /// @param protocol_version The discovery protocol version to name.
  /// @return The datagram, ending with a zero byte.
  static auto search_datagram(std::string_view protocol_version)
      -> std::vector<std::byte>;
  /// @brief Builds a wake-up datagram in wiping storage.
  ///
  /// @param protocol_version The discovery protocol version to name.
  /// @param credential The wake credential, written in decimal.
  /// @return The datagram, ending with a zero byte.
  static auto wake_datagram(std::string_view protocol_version,
                            const WakeCredential& credential) -> SecretBytes;
  /// @brief Resolves the host a wake-up goes to.
  ///
  /// Blocks while the host is resolved.
  ///
  /// @param dns The resolver.
  /// @param host The address or host name.
  /// @return The first resolved address of the text's family.
  /// @retval Failure::invalid_argument The host is empty, too long or holds a
  ///     zero byte.
  /// @retval Failure::network It did not resolve to an address of that
  ///     family.
  [[nodiscard]] static auto wake_destination(platform::DnsProvider& dns,
                                             std::string_view host)
      -> Expected<platform::IpAddress, Failure>;
  /// @brief The beacon failure a probe failure is reported as.
  ///
  /// @param failure The probe failure.
  /// @return Failure::short_send for a short send, Failure::network
  ///     otherwise.
  static constexpr auto reported(ProbeFailure failure) noexcept -> Failure {
    return failure == ProbeFailure::short_send ? Failure::short_send
                                               : Failure::network;
  }

  /// @brief The addresses send_search() goes to, besides resolved hosts.
  ///
  /// @return The configured addresses; or, with no targets configured, the
  ///     broadcast addresses described at send_search(), which are none on a
  ///     network that cannot broadcast.
  /// @retval Failure::network Interfaces could not be listed.
  [[nodiscard]] auto search_targets()
      -> Expected<std::vector<platform::IpAddress>, Failure>;
  /// @brief Resolves the configured host names not yet resolved.
  ///
  /// Blocks for DNS. A host keeps the last resolved address of the bound
  /// family.
  ///
  /// @return Nothing once every host is resolved.
  /// @retval Failure::network A host did not resolve to the bound family; it
  ///     is tried again on the next send_search().
  [[nodiscard]] auto resolve_pending_hosts() -> Expected<void, Failure>;
  /// @brief Sends one datagram.
  ///
  /// A failed send to an IPv6 destination is not reported.
  ///
  /// @param datagram The bytes.
  /// @param destination Where to.
  /// @return Why it was not sent, or nothing when it was.
  [[nodiscard]] auto transmit(std::span<const std::byte> datagram,
                              const platform::SocketEndpoint& destination)
      -> std::optional<ProbeFailure>;
  /// @brief Sends one search datagram and tells the watcher how it went.
  ///
  /// @param datagram The bytes.
  /// @param destination Where to.
  /// @return Why it was not sent, or nothing when it was.
  [[nodiscard]] auto probe(std::span<const std::byte> datagram,
                           const platform::SocketEndpoint& destination)
      -> std::optional<ProbeFailure>;

  /// @brief Sockets, DNS and interfaces.
  platform::NetworkProvider& network_;
  /// @brief Told of what is sent and received.
  Watcher& watcher_;
  /// @brief Where to bind and whom to search.
  Settings settings_;
  /// @brief Whether the network can broadcast, as its
  /// platform::NetworkCapabilities report.
  bool broadcasts_;
  /// @brief The discovery socket; null when not started.
  std::unique_ptr<platform::UdpSocket> socket_;
  /// @brief Where a datagram is received; SearchDatagram::receive_limit bytes
  /// once started.
  std::vector<std::byte> receive_buffer_;
  /// @brief The PS4 search datagram.
  std::vector<std::byte> ps4_search_;
  /// @brief The PS5 search datagram.
  std::vector<std::byte> ps5_search_;
  /// @brief Configured host names not yet resolved.
  std::vector<std::string> pending_hosts_;
  /// @brief Addresses of the host names already resolved.
  std::vector<platform::IpAddress> resolved_hosts_;
  /// @brief Waits on the socket and can be woken from another thread.
  net::http::SleepWaiter waiter_;
};

}  // namespace libchiaking::consoles
