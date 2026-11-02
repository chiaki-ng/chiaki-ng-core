/// @file
/// @brief Declares InMemoryNetwork, a whole network held inside one process.
#pragma once

#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::testing {

/// @brief Stands in for platform::NetworkProvider with sockets that only reach
/// each other.
///
/// Every UDP socket, TCP socket, listener and poller it hands out belongs to
/// one private network: a datagram or connection goes to another socket of the
/// same InMemoryNetwork or nowhere. Nothing touches the operating system, so
/// two halves of a protocol (the library and a FakeConsole, or two library
/// instances) can talk in one process on any port, including privileged ones.
///
/// To use it, construct one, adjust interface_list and add_host() if the test
/// needs particular addresses or names, and pass it wherever the library takes
/// a platform::NetworkProvider. Script faults with the fail, limit and stall
/// functions, undo them with clear_failures() and stall_connects(), and read
/// sent() to see where UDP traffic was addressed.
///
/// It behaves like a perfect local network and differs from a real one in these
/// ways:
///
/// Delivery is immediate and lossless, with no delay, reordering or
/// duplication. A UDP datagram reaches every socket bound to the destination
/// port whose address is the destination address or the wildcard. Broadcast and
/// multicast addresses get no special treatment.
///
/// A socket may bind only to the wildcard, a loopback address or an address
/// listed in interface_list. Port 0 is given the next free port from 49152
/// upwards.
///
/// The source address reported for a received datagram is the sender's bound
/// address exactly as bound, so a sender bound to the wildcard is reported with
/// the wildcard address.
///
/// A TCP connect always reports platform::NetError::would_block first and
/// settles afterwards, as a non-blocking socket would. Unless a fault is
/// scripted it fails with platform::NetError::host_unreachable when the target
/// address is not one of this network's own, and with
/// platform::NetError::connection_refused when nothing is listening there.
///
/// Socket options and receive buffer sizes are accepted and ignored.
///
/// Pollers measure their timeouts on the monotonic clock given at
/// construction. Their waits end in real time, so a ManualClock does not
/// stretch or shorten them.
///
/// @note Sockets, pollers and the scripting functions share one lock and may be
///     used from several threads. interface_list and add_host() are not covered
///     by it; set them up before the code under test starts.
class InMemoryNetwork final : public platform::NetworkProvider {
 public:
  /// @brief Creates a network with one wired interface and a `localhost`
  /// name.
  ///
  /// The interface is `eth0`, index 1, address 192.0.2.10 with a 24 bit
  /// prefix and broadcast address 192.0.2.255, reported as operational. The
  /// name `localhost` resolves to the IPv4 loopback address.
  ///
  /// @param threading Makes the network's lock and the condition its pollers
  ///     wait on; must outlive the network.
  /// @param clock The monotonic clock poller timeouts are measured on; must
  ///     outlive the network.
  InMemoryNetwork(platform::ThreadingProvider& threading,
                  const platform::ClockProvider& clock)
      : mutex_(threading), activity_(threading, clock), clock_(clock) {
    platform::InterfaceInfo wired;
    wired.name = "eth0";
    wired.index = 1;
    wired.address = platform::IpAddress::ipv4({192, 0, 2, 10});
    wired.broadcast = platform::IpAddress::ipv4({192, 0, 2, 255});
    wired.has_broadcast = true;
    wired.operational = true;
    wired.ipv4_prefix_length = 24;
    interface_list.push_back(std::move(wired));
    hosts_["localhost"] = {platform::IpAddress::loopback_ipv4()};
  }

  /// @brief Not copyable; its sockets hold a reference to it.
  InMemoryNetwork(const InMemoryNetwork&) = delete;
  /// @brief Not assignable; its sockets hold a reference to it.
  auto operator=(const InMemoryNetwork&) -> InMemoryNetwork& = delete;

  /// @brief The interfaces this machine appears to have.
  ///
  /// The interfaces provider reports exactly this list, and its addresses are
  /// the non-loopback addresses a socket is allowed to bind to. A test edits
  /// it to play a machine with no network, several networks or an IPv6
  /// interface.
  std::vector<platform::InterfaceInfo> interface_list;

  /// @brief Makes a host name resolvable.
  ///
  /// @param name The name to resolve, matched exactly.
  /// @param addresses The addresses to return for it, replacing any earlier
  ///     entry for the same name.
  /// @note Text that parses as an IP address resolves to itself without an
  ///     entry. Any other unknown name fails with
  ///     platform::NetError::host_unreachable.
  void add_host(std::string name, std::vector<platform::IpAddress> addresses) {
    hosts_[std::move(name)] = std::move(addresses);
  }

  /// @brief Makes every send fail from now on.
  ///
  /// Applies to UDP and TCP sockets alike until clear_failures() is called. A
  /// failed UDP send is still listed by sent().
  ///
  /// @param error The error each send returns.
  void fail_sends_with(platform::NetError error) {
    const platform::Lock lock{mutex_};
    send_error_ = error;
  }

  /// @brief Makes every receive fail from now on.
  ///
  /// Applies to UDP and TCP sockets alike until clear_failures() is called.
  /// Data already queued stays queued and is delivered once receives work
  /// again.
  ///
  /// @param error The error each receive returns.
  void fail_receives_with(platform::NetError error) {
    const platform::Lock lock{mutex_};
    receive_error_ = error;
  }

  /// @brief Makes TCP connects fail.
  ///
  /// Connects still waiting to settle fail at once and later ones fail as
  /// they are made, until clear_failures() is called. The socket's owner sees
  /// the error when it completes the connect. Connections already established
  /// are not affected, and while stall_connects() is in force connects stay
  /// pending instead of failing.
  ///
  /// @param error The error each connect completes with.
  void fail_connects_with(platform::NetError error) {
    const platform::Lock lock{mutex_};
    connect_error_ = error;
    settle_connects_locked();
    activity_.notify_all();
  }

  /// @brief Removes the scripted send, receive and connect failures and the
  /// stream send limit.
  ///
  /// It does not release connects held by stall_connects().
  void clear_failures() {
    const platform::Lock lock{mutex_};
    send_error_.reset();
    receive_error_.reset();
    connect_error_.reset();
    stream_send_limit_.reset();
    settle_connects_locked();
    activity_.notify_all();
  }

  /// @brief Makes TCP sends accept only part of what they are given.
  ///
  /// Each send on a TCP socket takes at most this many bytes and reports the
  /// number taken, which plays a full send buffer and exercises the caller's
  /// handling of short writes. UDP is not affected. clear_failures() removes
  /// the limit.
  ///
  /// @param bytes The most a single send accepts. Zero makes every send
  ///     accept nothing.
  void limit_stream_sends(std::size_t bytes) {
    const platform::Lock lock{mutex_};
    stream_send_limit_ = bytes;
  }

  /// @brief Holds TCP connects pending, or lets them go.
  ///
  /// While stalled, a connect neither succeeds nor fails, which plays a host
  /// that never answers so the caller's own timeout can be tested.
  ///
  /// @param stalled True to hold connects; false to let every pending connect
  ///     settle normally.
  void stall_connects(bool stalled = true) {
    const platform::Lock lock{mutex_};
    connects_stalled_ = stalled;
    settle_connects_locked();
    activity_.notify_all();
  }

  /// @brief Lists where UDP datagrams were addressed.
  ///
  /// @return The destination of every UDP send attempted so far, in order,
  ///     across all sockets of this network. Sends that were scripted to fail
  ///     are included; TCP traffic is not.
  auto sent() const -> std::vector<platform::SocketEndpoint> {
    const platform::Lock lock{mutex_};
    return sent_;
  }

  /// @brief Returns the provider that creates this network's sockets.
  ///
  /// @return This network's socket provider. It lives as long as the network
  ///     and may be used from any thread.
  auto sockets() noexcept -> platform::SocketProvider& override {
    return sockets_;
  }
  /// @brief Returns the provider that creates pollers for this network's
  /// sockets.
  ///
  /// A poller refuses, with platform::NetError::wrong_provider, to wait on a
  /// socket that belongs to any other network provider.
  ///
  /// @return This network's poller provider. It lives as long as the network
  ///     and may be used from any thread.
  auto pollers() noexcept -> platform::PollerProvider& override {
    return pollers_;
  }
  /// @brief Returns the resolver that answers from add_host() entries.
  ///
  /// @return This network's resolver. It lives as long as the network. It
  ///     reads add_host() entries without the lock.
  auto dns() noexcept -> platform::DnsProvider& override { return dns_; }
  /// @brief Returns the provider that reports interface_list.
  ///
  /// @return This network's interfaces provider. It lives as long as the
  ///     network. It copies interface_list without the lock.
  auto interfaces() noexcept -> platform::InterfacesProvider& override {
    return interfaces_;
  }
  /// @brief Reports that both "do not fragment" options are supported.
  ///
  /// The options themselves are accepted and have no effect.
  ///
  /// @return Capabilities with both options marked supported.
  auto capabilities() const noexcept -> platform::NetworkCapabilities override {
    return platform::NetworkCapabilities{true, true};
  }

 private:
  struct Datagram {
    platform::SocketEndpoint source;
    std::vector<std::byte> bytes;
  };

  struct Port {
    platform::SocketEndpoint endpoint;
    std::deque<Datagram> queue;
    std::optional<platform::SocketEndpoint> peer;
    bool refused{};
  };

  struct Pipe {
    std::deque<std::byte> bytes;
    bool writer_closed{};
    bool reader_closed{};
  };

  struct Connection {
    std::array<Pipe, 2> pipes;
    std::array<platform::SocketEndpoint, 2> endpoints;
  };

  struct Binding {
    platform::SocketEndpoint endpoint;
    bool listening{};
    std::size_t backlog{1};
    std::deque<std::shared_ptr<Connection>> pending;
  };

  struct StreamState {
    enum class Phase : std::uint8_t { idle, connecting, connected, failed };

    Phase phase{Phase::idle};
    std::shared_ptr<Binding> binding;
    platform::SocketEndpoint target;
    std::shared_ptr<Connection> connection;
    std::size_t side{};
    platform::NetError error{platform::NetError::system_failure};
  };

  /// @brief Something a Poller of this network can wait on.
  ///
  /// Implemented by the network's UDP sockets, TCP sockets and listeners.
  class Waitable {
   public:
    virtual ~Waitable() = default;
    /// @brief Reports which events would not block right now.
    ///
    /// The caller must hold the network's lock; Poller::wait does.
    ///
    /// @return The ready events, masked by nothing.
    virtual auto ready_locked() const noexcept -> platform::PollEvent = 0;
  };

  /// @brief A UDP socket of this network.
  ///
  /// Every call takes the network's lock, so a socket may be used from any
  /// thread. Nothing blocks.
  class Socket final : public platform::UdpSocket, public Waitable {
   public:
    /// @brief Creates an unbound socket.
    ///
    /// @param network The network it belongs to; it must outlive the
    ///     socket.
    /// @param family The only address family it accepts.
    Socket(InMemoryNetwork& network, platform::IpFamily family) noexcept
        : network_(network), family_(family) {}

    ~Socket() override { static_cast<void>(close()); }

    /// @brief Names the network that made this object.
    ///
    /// A poller uses it to refuse objects from another provider.
    ///
    /// @return This InMemoryNetwork.
    auto owner() const noexcept -> const platform::NetworkProvider& override {
      return network_;
    }

    /// @brief Claims a local endpoint. See platform::UdpSocket::bind.
    ///
    /// @param endpoint Where to bind. Port 0 takes the next free port.
    /// @return Nothing once the endpoint is claimed.
    /// @retval platform::NetError::closed The socket was closed.
    /// @retval platform::NetError::invalid_argument The socket is already
    ///     bound, or the address family is not the socket's.
    /// @retval platform::NetError::address_not_available The address is not
    ///     the wildcard, a loopback address or in interface_list.
    /// @retval platform::NetError::address_in_use Another socket holds it.
    [[nodiscard]] auto bind(const platform::SocketEndpoint& endpoint)
        -> libchiaking::Expected<void, platform::NetError> override {
      const platform::Lock lock{network_.mutex_};
      if (closed_) return libchiaking::unexpected(platform::NetError::closed);
      if (port_ || endpoint.address.family() != family_)
        return libchiaking::unexpected(platform::NetError::invalid_argument);
      if (!network_.owns(endpoint.address))
        return libchiaking::unexpected(
            platform::NetError::address_not_available);
      auto local = endpoint;
      if (local.port == 0) local.port = network_.free_port(network_.ports_);
      if (network_.conflicts(network_.ports_, local))
        return libchiaking::unexpected(platform::NetError::address_in_use);
      port_ = std::make_shared<Port>();
      port_->endpoint = local;
      network_.ports_.push_back(port_);
      return {};
    }

    /// @brief Delivers a datagram at once. See
    /// platform::UdpSocket::send_to.
    ///
    /// The destination is recorded for sent() even when the send fails.
    ///
    /// @param datagram The bytes to deliver.
    /// @param destination Where to deliver them; with no socket there the
    ///     datagram is dropped silently.
    /// @return The whole size of @p datagram.
    /// @retval platform::NetError::closed The socket was closed.
    /// @retval platform::NetError::invalid_argument The socket is not
    ///     bound.
    ///
    /// Any other failure is the error scripted with fail_sends_with().
    [[nodiscard]] auto send_to(std::span<const std::byte> datagram,
                               const platform::SocketEndpoint& destination)
        -> libchiaking::Expected<std::size_t, platform::NetError> override {
      const platform::Lock lock{network_.mutex_};
      if (closed_) return libchiaking::unexpected(platform::NetError::closed);
      if (!port_)
        return libchiaking::unexpected(platform::NetError::invalid_argument);
      network_.sent_.push_back(destination);
      if (network_.send_error_)
        return libchiaking::unexpected(*network_.send_error_);
      network_.deliver(port_->endpoint, destination, datagram);
      return datagram.size();
    }

    /// @brief Takes the oldest queued datagram. See
    /// platform::UdpSocket::receive_from.
    ///
    /// @param buffer Where to copy it; a longer datagram is cut to fit.
    /// @return The bytes copied and the sender's endpoint as it was bound.
    /// @retval platform::NetError::closed The socket was closed.
    /// @retval platform::NetError::connection_refused An earlier send() on
    ///     this connected socket reached nobody; reported once.
    /// @retval platform::NetError::would_block Nothing is queued, or the
    ///     socket is not bound.
    ///
    /// Any other failure is the error scripted with fail_receives_with().
    [[nodiscard]] auto receive_from(std::span<std::byte> buffer)
        -> libchiaking::Expected<platform::UdpReceiveResult,
                                 platform::NetError> override {
      const platform::Lock lock{network_.mutex_};
      if (closed_) return libchiaking::unexpected(platform::NetError::closed);
      if (network_.receive_error_)
        return libchiaking::unexpected(*network_.receive_error_);
      if (port_ && std::exchange(port_->refused, false))
        return libchiaking::unexpected(platform::NetError::connection_refused);
      if (!port_ || port_->queue.empty())
        return libchiaking::unexpected(platform::NetError::would_block);
      auto datagram = std::move(port_->queue.front());
      port_->queue.pop_front();
      const auto size = std::min(buffer.size(), datagram.bytes.size());
      std::copy_n(datagram.bytes.begin(), size, buffer.begin());
      return platform::UdpReceiveResult{size, datagram.source, std::nullopt};
    }

    /// @brief Fixes the peer for send(). See
    /// platform::UdpSocket::connect.
    ///
    /// An unbound socket is first bound to a free port on the address this
    /// network would send from.
    ///
    /// @param endpoint The peer.
    /// @return Nothing once the peer is set.
    /// @retval platform::NetError::closed The socket was closed.
    /// @retval platform::NetError::invalid_argument The address family is
    ///     not the socket's.
    /// @retval platform::NetError::address_not_available No address of this
    ///     network can reach @p endpoint.
    [[nodiscard]] auto connect(const platform::SocketEndpoint& endpoint)
        -> libchiaking::Expected<void, platform::NetError> override {
      const platform::Lock lock{network_.mutex_};
      if (closed_) return libchiaking::unexpected(platform::NetError::closed);
      if (endpoint.address.family() != family_)
        return libchiaking::unexpected(platform::NetError::invalid_argument);
      if (!network_.routes(endpoint.address))
        return libchiaking::unexpected(
            platform::NetError::address_not_available);
      if (!port_) {
        port_ = std::make_shared<Port>();
        port_->endpoint = platform::SocketEndpoint{
            network_.source_address_for(
                platform::SocketEndpoint{wildcard(family_), 0},
                endpoint.address),
            network_.free_port(network_.ports_)};
        network_.ports_.push_back(port_);
      }
      port_->peer = endpoint;
      return {};
    }

    /// @brief Delivers a datagram to the connected peer. See
    /// platform::UdpSocket::send.
    ///
    /// When nothing is bound at the peer the next receive reports
    /// platform::NetError::connection_refused, as a real socket would.
    ///
    /// @param datagram The bytes to deliver.
    /// @return The whole size of @p datagram.
    /// @retval platform::NetError::closed The socket was closed.
    /// @retval platform::NetError::not_connected connect() was not called.
    ///
    /// Any other failure is the error scripted with fail_sends_with().
    [[nodiscard]] auto send(std::span<const std::byte> datagram)
        -> libchiaking::Expected<std::size_t, platform::NetError> override {
      const platform::Lock lock{network_.mutex_};
      if (closed_) return libchiaking::unexpected(platform::NetError::closed);
      if (!port_ || !port_->peer)
        return libchiaking::unexpected(platform::NetError::not_connected);
      network_.sent_.push_back(*port_->peer);
      if (network_.send_error_)
        return libchiaking::unexpected(*network_.send_error_);
      if (network_.deliver(port_->endpoint, *port_->peer, datagram) == 0)
        port_->refused = true;
      return datagram.size();
    }

    /// @brief Accepts any option and ignores it.
    ///
    /// @return Always nothing.
    [[nodiscard]] auto set_boolean_option(platform::SocketOption, bool)
        -> libchiaking::Expected<void, platform::NetError> override {
      return {};
    }

    /// @brief Accepts any size and ignores it.
    ///
    /// @return Always nothing.
    [[nodiscard]] auto set_receive_buffer_size(std::size_t)
        -> libchiaking::Expected<void, platform::NetError> override {
      return {};
    }

    /// @brief Reports where the socket is bound.
    ///
    /// @return The bound endpoint, with the port bind() chose.
    /// @retval platform::NetError::not_connected The socket is not bound,
    ///     or was closed.
    [[nodiscard]] auto local_endpoint()
        -> libchiaking::Expected<platform::SocketEndpoint,
                                 platform::NetError> override {
      const platform::Lock lock{network_.mutex_};
      if (!port_)
        return libchiaking::unexpected(platform::NetError::not_connected);
      return port_->endpoint;
    }

    /// @brief Releases the port and drops queued datagrams. Safe to repeat.
    ///
    /// @return Always nothing.
    [[nodiscard]] auto close()
        -> libchiaking::Expected<void, platform::NetError> override {
      const platform::Lock lock{network_.mutex_};
      if (closed_) return {};
      closed_ = true;
      if (port_) std::erase(network_.ports_, port_);
      port_.reset();
      return {};
    }

    /// @brief Reports which events would not block right now.
    ///
    /// The caller must hold the network's lock; Poller::wait does.
    ///
    /// @return Readable while a datagram is queued, and always writable;
    ///     nothing once closed.
    auto ready_locked() const noexcept -> platform::PollEvent override {
      if (closed_) return platform::PollEvent::none;
      const auto readable = port_ && !port_->queue.empty()
                                ? platform::PollEvent::readable
                                : platform::PollEvent::none;
      return readable | platform::PollEvent::writable;
    }

   private:
    InMemoryNetwork& network_;
    platform::IpFamily family_;
    std::shared_ptr<Port> port_;
    bool closed_{};
  };

  /// @brief A TCP socket of this network.
  ///
  /// Every call takes the network's lock, so a socket may be used from any
  /// thread. Nothing blocks; a connect settles on the network's own schedule.
  class Stream final : public platform::TcpSocket, public Waitable {
   public:
    /// @brief Creates an unbound, unconnected socket.
    ///
    /// @param network The network it belongs to; it must outlive the
    ///     socket.
    /// @param family The only address family it accepts.
    Stream(InMemoryNetwork& network, platform::IpFamily family)
        : network_(network),
          family_(family),
          state_(std::make_shared<StreamState>()) {}

    /// @brief Creates the accepted end of a connection.
    ///
    /// @param network The network it belongs to; it must outlive the
    ///     socket.
    /// @param family The connection's address family.
    /// @param connection The connection to join.
    /// @param side Which end it is: 0 for the connecting end, 1 for the
    ///     accepted end.
    Stream(InMemoryNetwork& network, platform::IpFamily family,
           std::shared_ptr<Connection> connection, std::size_t side)
        : Stream(network, family) {
      state_->phase = StreamState::Phase::connected;
      state_->connection = std::move(connection);
      state_->side = side;
    }

    ~Stream() override { static_cast<void>(close()); }

    /// @brief Names the network that made this object.
    ///
    /// A poller uses it to refuse objects from another provider.
    ///
    /// @return This InMemoryNetwork.
    auto owner() const noexcept -> const platform::NetworkProvider& override {
      return network_;
    }

    /// @brief Claims a local endpoint before connecting. See
    /// platform::TcpSocket::bind.
    ///
    /// @param endpoint Where to bind. Port 0 takes the next free port.
    /// @return Nothing once the endpoint is claimed.
    /// @retval platform::NetError::closed The socket was closed.
    /// @retval platform::NetError::invalid_argument The socket is already
    ///     bound or connecting, or the family is not the socket's.
    /// @retval platform::NetError::address_not_available The address is not
    ///     this network's own.
    /// @retval platform::NetError::address_in_use Another socket holds it.
    [[nodiscard]] auto bind(const platform::SocketEndpoint& endpoint)
        -> libchiaking::Expected<void, platform::NetError> override {
      const platform::Lock lock{network_.mutex_};
      if (closed_) return libchiaking::unexpected(platform::NetError::closed);
      if (state_->binding || state_->phase != StreamState::Phase::idle)
        return libchiaking::unexpected(platform::NetError::invalid_argument);
      auto binding = network_.bind_tcp_locked(endpoint, family_);
      if (!binding) return libchiaking::unexpected(binding.error());
      state_->binding = std::move(binding.value());
      return {};
    }

    /// @brief Starts a connect. See platform::TcpSocket::connect.
    ///
    /// @param endpoint The listener to reach.
    /// @return Nothing when already connected.
    /// @retval platform::NetError::would_block The connect started, or is
    ///     still pending; finish it with complete_connect().
    /// @retval platform::NetError::closed The socket was closed.
    /// @retval platform::NetError::invalid_argument The family is not the
    ///     socket's.
    ///
    /// Any other failure comes from binding a free local port.
    [[nodiscard]] auto connect(const platform::SocketEndpoint& endpoint)
        -> libchiaking::Expected<void, platform::NetError> override {
      const platform::Lock lock{network_.mutex_};
      if (closed_) return libchiaking::unexpected(platform::NetError::closed);
      if (endpoint.address.family() != family_)
        return libchiaking::unexpected(platform::NetError::invalid_argument);
      if (state_->phase == StreamState::Phase::connected) return {};
      if (state_->phase == StreamState::Phase::connecting)
        return libchiaking::unexpected(platform::NetError::would_block);
      if (!state_->binding) {
        auto binding = network_.bind_tcp_locked(
            platform::SocketEndpoint{wildcard(family_), 0}, family_);
        if (!binding) return libchiaking::unexpected(binding.error());
        state_->binding = std::move(binding.value());
      }
      state_->target = endpoint;
      state_->phase = StreamState::Phase::connecting;
      network_.connecting_.push_back(state_);
      network_.settle_connects_locked();
      network_.activity_.notify_all();
      return libchiaking::unexpected(platform::NetError::would_block);
    }

    /// @brief Reports how a pending connect settled. See
    /// platform::TcpSocket::complete_connect.
    ///
    /// @return Nothing once connected.
    /// @retval platform::NetError::would_block The connect is still
    ///     pending.
    /// @retval platform::NetError::closed The socket was closed.
    /// @retval platform::NetError::host_unreachable The target is not one
    ///     of this network's addresses.
    /// @retval platform::NetError::connection_refused Nothing listens
    ///     there.
    ///
    /// Any other failure is the error scripted with fail_connects_with().
    [[nodiscard]] auto complete_connect()
        -> libchiaking::Expected<void, platform::NetError> override {
      const platform::Lock lock{network_.mutex_};
      if (closed_) return libchiaking::unexpected(platform::NetError::closed);
      if (state_->phase == StreamState::Phase::connecting)
        return libchiaking::unexpected(platform::NetError::would_block);
      if (state_->phase == StreamState::Phase::failed) {
        state_->phase = StreamState::Phase::idle;
        return libchiaking::unexpected(state_->error);
      }
      return {};
    }

    /// @brief Queues bytes for the peer. See
    /// platform::TcpSocket::send.
    ///
    /// @param data The bytes to send.
    /// @return How many were taken: all of them, or at most the limit set
    ///     with limit_stream_sends().
    /// @retval platform::NetError::closed The socket was closed, or the
    ///     peer stopped reading.
    /// @retval platform::NetError::not_connected The socket is not
    ///     connected.
    ///
    /// Any other failure is the error scripted with fail_sends_with().
    [[nodiscard]] auto send(std::span<const std::byte> data)
        -> libchiaking::Expected<std::size_t, platform::NetError> override {
      const platform::Lock lock{network_.mutex_};
      if (closed_) return libchiaking::unexpected(platform::NetError::closed);
      if (state_->phase != StreamState::Phase::connected)
        return libchiaking::unexpected(platform::NetError::not_connected);
      if (network_.send_error_)
        return libchiaking::unexpected(*network_.send_error_);
      auto& outbound = state_->connection->pipes[state_->side];
      if (outbound.reader_closed)
        return libchiaking::unexpected(platform::NetError::closed);
      const auto accepted = data.first(std::min(
          data.size(), network_.stream_send_limit_.value_or(data.size())));
      outbound.bytes.insert(outbound.bytes.end(), accepted.begin(),
                            accepted.end());
      network_.activity_.notify_all();
      return accepted.size();
    }

    /// @brief Takes bytes the peer sent. See
    /// platform::TcpSocket::receive.
    ///
    /// @param buffer Where to copy them.
    /// @return How many bytes were copied, at most the size of @p buffer.
    /// @retval platform::NetError::closed The socket was closed, or the
    ///     peer closed and everything it sent has been read.
    /// @retval platform::NetError::not_connected The socket is not
    ///     connected.
    /// @retval platform::NetError::would_block Nothing has arrived yet.
    ///
    /// Any other failure is the error scripted with fail_receives_with().
    [[nodiscard]] auto receive(std::span<std::byte> buffer)
        -> libchiaking::Expected<std::size_t, platform::NetError> override {
      const platform::Lock lock{network_.mutex_};
      if (closed_) return libchiaking::unexpected(platform::NetError::closed);
      if (state_->phase != StreamState::Phase::connected)
        return libchiaking::unexpected(platform::NetError::not_connected);
      if (network_.receive_error_)
        return libchiaking::unexpected(*network_.receive_error_);
      auto& inbound = state_->connection->pipes[1 - state_->side];
      if (!inbound.bytes.empty()) {
        const auto size = std::min(buffer.size(), inbound.bytes.size());
        std::copy_n(inbound.bytes.begin(), size, buffer.begin());
        inbound.bytes.erase(
            inbound.bytes.begin(),
            inbound.bytes.begin() + static_cast<std::ptrdiff_t>(size));
        return size;
      }
      if (inbound.writer_closed)
        return libchiaking::unexpected(platform::NetError::closed);
      return libchiaking::unexpected(platform::NetError::would_block);
    }

    /// @brief Reports this end's endpoint.
    ///
    /// @return The connection's local endpoint, or the bound endpoint
    ///     before connecting.
    /// @retval platform::NetError::closed The socket was closed.
    /// @retval platform::NetError::not_connected It is neither bound nor
    ///     connected.
    [[nodiscard]] auto local_endpoint()
        -> libchiaking::Expected<platform::SocketEndpoint,
                                 platform::NetError> override {
      const platform::Lock lock{network_.mutex_};
      if (closed_) return libchiaking::unexpected(platform::NetError::closed);
      if (state_->phase == StreamState::Phase::connected)
        return state_->connection->endpoints[state_->side];
      if (!state_->binding)
        return libchiaking::unexpected(platform::NetError::not_connected);
      return state_->binding->endpoint;
    }

    /// @brief Reports the other end's endpoint.
    ///
    /// @return The peer's endpoint.
    /// @retval platform::NetError::closed The socket was closed.
    /// @retval platform::NetError::not_connected The socket is not
    ///     connected.
    [[nodiscard]] auto peer_endpoint()
        -> libchiaking::Expected<platform::SocketEndpoint,
                                 platform::NetError> override {
      const platform::Lock lock{network_.mutex_};
      if (closed_) return libchiaking::unexpected(platform::NetError::closed);
      if (state_->phase != StreamState::Phase::connected)
        return libchiaking::unexpected(platform::NetError::not_connected);
      return state_->connection->endpoints[1 - state_->side];
    }

    /// @brief Ends the connection and releases the port.
    ///
    /// The peer reads what was already sent and then sees the end of the
    /// stream.
    ///
    /// @return Nothing on the first call.
    /// @retval platform::NetError::closed The socket was already closed.
    [[nodiscard]] auto close()
        -> libchiaking::Expected<void, platform::NetError> override {
      const platform::Lock lock{network_.mutex_};
      if (closed_) return libchiaking::unexpected(platform::NetError::closed);
      closed_ = true;
      if (state_->phase == StreamState::Phase::connected) {
        state_->connection->pipes[state_->side].writer_closed = true;
        state_->connection->pipes[1 - state_->side].reader_closed = true;
      }
      std::erase(network_.connecting_, state_);
      if (state_->binding) std::erase(network_.tcp_bindings_, state_->binding);
      state_->binding.reset();
      state_->phase = StreamState::Phase::idle;
      network_.activity_.notify_all();
      return {};
    }

    /// @brief Reports which events would not block right now.
    ///
    /// The caller must hold the network's lock; Poller::wait does.
    ///
    /// @return Readable while bytes or the end of the stream wait, and
    ///     writable while connected; writable alone once a connect failed;
    ///     nothing otherwise.
    auto ready_locked() const noexcept -> platform::PollEvent override {
      if (closed_) return platform::PollEvent::none;
      if (state_->phase == StreamState::Phase::failed)
        return platform::PollEvent::writable;
      if (state_->phase != StreamState::Phase::connected)
        return platform::PollEvent::none;
      const auto& inbound = state_->connection->pipes[1 - state_->side];
      const auto readable = !inbound.bytes.empty() || inbound.writer_closed
                                ? platform::PollEvent::readable
                                : platform::PollEvent::none;
      return readable | platform::PollEvent::writable;
    }

   private:
    InMemoryNetwork& network_;
    platform::IpFamily family_;
    std::shared_ptr<StreamState> state_;
    bool closed_{};
  };

  /// @brief A TCP listener of this network.
  ///
  /// Every call takes the network's lock, so a listener may be used from any
  /// thread. Nothing blocks.
  class Listener final : public platform::TcpListener, public Waitable {
   public:
    /// @brief Creates an unbound listener.
    ///
    /// @param network The network it belongs to; it must outlive the
    ///     listener.
    /// @param family The only address family it accepts.
    Listener(InMemoryNetwork& network, platform::IpFamily family) noexcept
        : network_(network), family_(family) {}

    ~Listener() override { static_cast<void>(close()); }

    /// @brief Names the network that made this object.
    ///
    /// A poller uses it to refuse objects from another provider.
    ///
    /// @return This InMemoryNetwork.
    auto owner() const noexcept -> const platform::NetworkProvider& override {
      return network_;
    }

    /// @brief Claims the endpoint to listen on. See
    /// platform::TcpListener::bind.
    ///
    /// @param endpoint Where to bind. Port 0 takes the next free port.
    /// @return Nothing once the endpoint is claimed.
    /// @retval platform::NetError::closed The listener was closed.
    /// @retval platform::NetError::invalid_argument It is already bound, or
    ///     the family is not the listener's.
    /// @retval platform::NetError::address_not_available The address is not
    ///     this network's own.
    /// @retval platform::NetError::address_in_use Another socket holds it.
    [[nodiscard]] auto bind(const platform::SocketEndpoint& endpoint)
        -> libchiaking::Expected<void, platform::NetError> override {
      const platform::Lock lock{network_.mutex_};
      if (closed_) return libchiaking::unexpected(platform::NetError::closed);
      if (binding_)
        return libchiaking::unexpected(platform::NetError::invalid_argument);
      auto binding = network_.bind_tcp_locked(endpoint, family_);
      if (!binding) return libchiaking::unexpected(binding.error());
      binding_ = std::move(binding.value());
      return {};
    }

    /// @brief Starts accepting connects. See
    /// platform::TcpListener::listen.
    ///
    /// An unbound listener is first bound to a free port on the wildcard.
    ///
    /// @param backlog How many connections may wait for accept(); values
    ///     below one count as one. Connects beyond it stay pending.
    /// @return Nothing once listening.
    /// @retval platform::NetError::closed The listener was closed.
    ///
    /// Any other failure comes from binding a free port.
    [[nodiscard]] auto listen(int backlog)
        -> libchiaking::Expected<void, platform::NetError> override {
      const platform::Lock lock{network_.mutex_};
      if (closed_) return libchiaking::unexpected(platform::NetError::closed);
      if (!binding_) {
        auto binding = network_.bind_tcp_locked(
            platform::SocketEndpoint{wildcard(family_), 0}, family_);
        if (!binding) return libchiaking::unexpected(binding.error());
        binding_ = std::move(binding.value());
      }
      binding_->listening = true;
      binding_->backlog = static_cast<std::size_t>(std::max(backlog, 1));
      network_.settle_connects_locked();
      network_.activity_.notify_all();
      return {};
    }

    /// @brief Takes the oldest waiting connection. See
    /// platform::TcpListener::accept.
    ///
    /// @return The accepted end, already connected.
    /// @retval platform::NetError::closed The listener was closed.
    /// @retval platform::NetError::invalid_argument listen() was not
    ///     called.
    /// @retval platform::NetError::would_block No connection is waiting.
    [[nodiscard]] auto accept()
        -> libchiaking::Expected<std::unique_ptr<platform::TcpSocket>,
                                 platform::NetError> override {
      const platform::Lock lock{network_.mutex_};
      if (closed_) return libchiaking::unexpected(platform::NetError::closed);
      if (!binding_ || !binding_->listening)
        return libchiaking::unexpected(platform::NetError::invalid_argument);
      if (binding_->pending.empty())
        return libchiaking::unexpected(platform::NetError::would_block);
      auto connection = std::move(binding_->pending.front());
      binding_->pending.pop_front();
      network_.settle_connects_locked();
      network_.activity_.notify_all();
      std::unique_ptr<platform::TcpSocket> stream =
          std::make_unique<Stream>(network_, family_, std::move(connection), 1);
      return stream;
    }

    /// @brief Reports where the listener is bound.
    ///
    /// @return The bound endpoint.
    /// @retval platform::NetError::closed The listener was closed.
    /// @retval platform::NetError::not_connected It is not bound.
    [[nodiscard]] auto local_endpoint()
        -> libchiaking::Expected<platform::SocketEndpoint,
                                 platform::NetError> override {
      const platform::Lock lock{network_.mutex_};
      if (closed_) return libchiaking::unexpected(platform::NetError::closed);
      if (!binding_)
        return libchiaking::unexpected(platform::NetError::not_connected);
      return binding_->endpoint;
    }

    /// @brief Stops listening; waiting connections see the end of the
    /// stream.
    ///
    /// @return Nothing on the first call.
    /// @retval platform::NetError::closed The listener was already closed.
    [[nodiscard]] auto close()
        -> libchiaking::Expected<void, platform::NetError> override {
      const platform::Lock lock{network_.mutex_};
      if (closed_) return libchiaking::unexpected(platform::NetError::closed);
      closed_ = true;
      if (binding_) {
        for (const auto& connection : binding_->pending) {
          connection->pipes[1].writer_closed = true;
          connection->pipes[0].reader_closed = true;
        }
        std::erase(network_.tcp_bindings_, binding_);
      }
      binding_.reset();
      network_.activity_.notify_all();
      return {};
    }

    /// @brief Reports which events would not block right now.
    ///
    /// The caller must hold the network's lock; Poller::wait does.
    ///
    /// @return Readable while a connection waits for accept(); nothing
    ///     otherwise.
    auto ready_locked() const noexcept -> platform::PollEvent override {
      if (closed_ || !binding_ || binding_->pending.empty())
        return platform::PollEvent::none;
      return platform::PollEvent::readable;
    }

   private:
    InMemoryNetwork& network_;
    platform::IpFamily family_;
    std::shared_ptr<Binding> binding_;
    bool closed_{};
  };

  /// @brief Waits on this network's sockets and listeners.
  ///
  /// It shares the network's lock and is woken by any activity on the
  /// network, so it may be used from any thread.
  class Poller final : public platform::Poller {
   public:
    /// @brief Creates a poller.
    ///
    /// @param network The network whose objects it waits on; it must
    ///     outlive the poller.
    explicit Poller(InMemoryNetwork& network) noexcept : network_(network) {}

    /// @brief Names the network that made this object.
    ///
    /// A poller uses it to refuse objects from another provider.
    ///
    /// @return This InMemoryNetwork.
    auto owner() const noexcept -> const platform::NetworkProvider& override {
      return network_;
    }

    /// @brief Ends the current or next wait early. See
    /// platform::Poller::wake.
    ///
    /// @return Always nothing.
    [[nodiscard]] auto wake()
        -> libchiaking::Expected<void, platform::NetError> override {
      const platform::Lock lock{network_.mutex_};
      woken_ = true;
      network_.activity_.notify_all();
      return {};
    }

    /// @brief Blocks until a request is ready, the poller is woken, the
    /// stop token fires or the timeout passes. See
    /// platform::Poller::wait.
    ///
    /// The timeout is measured on the network's clock. A stop request ends
    /// the wait at once.
    ///
    /// @param requests The objects to watch and the events of interest.
    /// @param timeout The longest time to wait.
    /// @param stop Ends the wait when triggered, and is reported in the
    ///     result.
    /// @return The ready requests, whether it was woken and whether a stop
    ///     was requested; empty when the timeout passed.
    /// @retval platform::NetError::wrong_provider A request has a null
    ///     pollable or one that belongs to another provider.
    [[nodiscard]] auto wait(std::span<const platform::PollRequest> requests,
                            std::chrono::nanoseconds timeout,
                            const platform::StopToken& stop)
        -> libchiaking::Expected<platform::PollWaitResult,
                                 platform::NetError> override {
      platform::Lock lock{network_.mutex_};
      const auto started = network_.clock_.now_microseconds();
      const auto budget =
          std::chrono::duration_cast<std::chrono::microseconds>(timeout);
      for (;;) {
        platform::PollWaitResult result;
        for (const auto& request : requests) {
          if (request.pollable == nullptr ||
              &request.pollable->owner() != &network_)
            return libchiaking::unexpected(platform::NetError::wrong_provider);
          const auto* waitable =
              dynamic_cast<const Waitable*>(request.pollable);
          if (waitable == nullptr)
            return libchiaking::unexpected(platform::NetError::wrong_provider);
          const auto ready = waitable->ready_locked() & request.interest;
          if (ready != platform::PollEvent::none)
            result.ready.push_back(platform::PollReadiness{request.tag, ready});
        }
        result.woke = std::exchange(woken_, false);
        result.stop_requested = stop.stop_requested();
        if (!result.ready.empty() || result.woke || result.stop_requested)
          return result;
        const auto spent = std::chrono::microseconds{static_cast<std::int64_t>(
            network_.clock_.now_microseconds() - started)};
        if (spent >= budget ||
            network_.activity_.wait_for(lock, budget - spent, stop) ==
                platform::ConditionWaitStatus::timed_out)
          return result;
      }
    }

   private:
    InMemoryNetwork& network_;
    bool woken_{};
  };

  /// @brief Creates this network's sockets and listeners.
  ///
  /// Creation never fails and may happen on any thread.
  class Sockets final : public platform::SocketProvider {
   public:
    /// @brief Creates the provider.
    ///
    /// @param network The network the sockets will belong to.
    explicit Sockets(InMemoryNetwork& network) noexcept : network_(network) {}

    /// @brief Creates an unbound UDP socket.
    ///
    /// @param family_of An address whose family the socket will use.
    /// @return The socket. Never fails.
    [[nodiscard]] auto create_udp_socket(const platform::IpAddress& family_of)
        -> libchiaking::Expected<std::unique_ptr<platform::UdpSocket>,
                                 platform::NetError> override {
      std::unique_ptr<platform::UdpSocket> socket =
          std::make_unique<Socket>(network_, family_of.family());
      return socket;
    }

    /// @brief Creates an unconnected TCP socket.
    ///
    /// @param family_of An address whose family the socket will use.
    /// @return The socket. Never fails.
    [[nodiscard]] auto create_tcp_socket(const platform::IpAddress& family_of)
        -> libchiaking::Expected<std::unique_ptr<platform::TcpSocket>,
                                 platform::NetError> override {
      std::unique_ptr<platform::TcpSocket> socket =
          std::make_unique<Stream>(network_, family_of.family());
      return socket;
    }

    /// @brief Creates an unbound TCP listener.
    ///
    /// @param family_of An address whose family the listener will use.
    /// @return The listener. Never fails.
    [[nodiscard]] auto create_tcp_listener(const platform::IpAddress& family_of)
        -> libchiaking::Expected<std::unique_ptr<platform::TcpListener>,
                                 platform::NetError> override {
      std::unique_ptr<platform::TcpListener> listener =
          std::make_unique<Listener>(network_, family_of.family());
      return listener;
    }

   private:
    InMemoryNetwork& network_;
  };

  /// @brief Creates this network's pollers.
  class Pollers final : public platform::PollerProvider {
   public:
    /// @brief Creates the provider.
    ///
    /// @param network The network the pollers will wait on.
    explicit Pollers(InMemoryNetwork& network) noexcept : network_(network) {}

    /// @brief Creates a poller for this network's objects.
    ///
    /// @return The poller. Never fails.
    [[nodiscard]] auto create_poller()
        -> libchiaking::Expected<std::unique_ptr<platform::Poller>,
                                 platform::NetError> override {
      std::unique_ptr<platform::Poller> poller =
          std::make_unique<Poller>(network_);
      return poller;
    }

   private:
    InMemoryNetwork& network_;
  };

  /// @brief Resolves names from the network's add_host() entries.
  ///
  /// It reads them without the lock, so entries must be set up before the
  /// code under test runs. It never blocks.
  class Dns final : public platform::DnsProvider {
   public:
    /// @brief Creates the resolver.
    ///
    /// @param network The network whose entries it reads.
    explicit Dns(InMemoryNetwork& network) noexcept : network_(network) {}

    /// @brief Keeps the stop-aware resolve() of platform::DnsProvider
    /// visible beside the override below.
    ///
    /// It fails at once when @p stop is already stopped and otherwise
    /// answers as the two-argument overload does. It never blocks.
    ///
    /// @param host An IP address in text, or a name given to add_host().
    /// @param service Ignored.
    /// @param stop Checked once, before the lookup.
    /// @return The addresses for @p host.
    /// @retval platform::NetError::timed_out @p stop was stopped by its
    ///     deadline.
    /// @retval platform::NetError::interrupted @p stop was stopped for
    ///     another reason.
    /// @retval platform::NetError::host_unreachable The name is unknown.
    using platform::DnsProvider::resolve;

    /// @brief Looks a name up. The service is ignored.
    ///
    /// @param host An IP address in text, which resolves to itself, or a
    ///     name given to add_host().
    /// @return The addresses for @p host.
    /// @retval platform::NetError::host_unreachable The name is unknown.
    [[nodiscard]] auto resolve(std::string_view host, std::string_view)
        -> libchiaking::Expected<std::vector<platform::IpAddress>,
                                 platform::NetError> override {
      if (const auto literal = platform::parse_ip_address(host))
        return std::vector<platform::IpAddress>{literal.value()};
      const auto found = network_.hosts_.find(std::string{host});
      if (found == network_.hosts_.end())
        return libchiaking::unexpected(platform::NetError::host_unreachable);
      return found->second;
    }

   private:
    InMemoryNetwork& network_;
  };

  /// @brief Reports the network's interface_list.
  class Interfaces final : public platform::InterfacesProvider {
   public:
    /// @brief Creates the provider.
    ///
    /// @param network The network whose interface_list it reports.
    explicit Interfaces(InMemoryNetwork& network) noexcept
        : network_(network) {}

    /// @brief Lists the interfaces.
    ///
    /// interface_list is copied without the lock.
    ///
    /// @return A copy of interface_list. Never fails.
    [[nodiscard]] auto enumerate()
        -> libchiaking::Expected<std::vector<platform::InterfaceInfo>,
                                 platform::NetError> override {
      return network_.interface_list;
    }

   private:
    InMemoryNetwork& network_;
  };

  static auto wildcard(platform::IpFamily family) -> platform::IpAddress {
    return family == platform::IpFamily::ipv4 ? platform::IpAddress::any_ipv4()
                                              : platform::IpAddress::any_ipv6();
  }

  auto owns(const platform::IpAddress& address) const -> bool {
    return address.is_unspecified() || address.is_loopback() ||
           std::ranges::any_of(interface_list,
                               [&](const platform::InterfaceInfo& info) {
                                 return info.address == address;
                               });
  }

  template <typename Registry>
  auto free_port(const Registry& registry) -> std::uint16_t {
    for (;;) {
      const auto candidate = next_port_;
      next_port_ = next_port_ == 65535 ? 49152 : next_port_ + 1;
      if (std::ranges::none_of(registry, [&](const auto& entry) {
            return entry->endpoint.port == candidate;
          }))
        return candidate;
    }
  }

  static auto overlaps(const platform::SocketEndpoint& bound,
                       const platform::SocketEndpoint& other) -> bool {
    return bound.port == other.port &&
           bound.address.family() == other.address.family() &&
           (bound.address == other.address || bound.address.is_unspecified() ||
            other.address.is_unspecified());
  }

  template <typename Registry>
  auto conflicts(const Registry& registry,
                 const platform::SocketEndpoint& local) const -> bool {
    return std::ranges::any_of(registry, [&](const auto& entry) {
      return overlaps(entry->endpoint, local);
    });
  }

  auto routes(const platform::IpAddress& address) const -> bool {
    return !address.is_link_local() || address.scope_id() == 0 ||
           std::ranges::any_of(interface_list,
                               [&](const platform::InterfaceInfo& info) {
                                 return info.index == address.scope_id();
                               });
  }

  auto deliver(const platform::SocketEndpoint& source,
               const platform::SocketEndpoint& destination,
               std::span<const std::byte> bytes) -> std::size_t {
    std::size_t reached = 0;
    for (const auto& port : ports_) {
      if (port->endpoint.port != destination.port ||
          port->endpoint.address.family() != destination.address.family() ||
          (port->endpoint.address != destination.address &&
           !port->endpoint.address.is_unspecified()))
        continue;
      ++reached;
      if (!port->peer || *port->peer == source)
        port->queue.push_back(Datagram{source, {bytes.begin(), bytes.end()}});
    }
    activity_.notify_all();
    return reached;
  }

  [[nodiscard]] auto bind_tcp_locked(const platform::SocketEndpoint& endpoint,
                                     platform::IpFamily family)
      -> libchiaking::Expected<std::shared_ptr<Binding>, platform::NetError> {
    if (endpoint.address.family() != family)
      return libchiaking::unexpected(platform::NetError::invalid_argument);
    if (!owns(endpoint.address))
      return libchiaking::unexpected(platform::NetError::address_not_available);
    auto local = endpoint;
    if (local.port == 0) local.port = free_port(tcp_bindings_);
    if (conflicts(tcp_bindings_, local))
      return libchiaking::unexpected(platform::NetError::address_in_use);
    auto binding = std::make_shared<Binding>();
    binding->endpoint = local;
    tcp_bindings_.push_back(binding);
    return binding;
  }

  auto source_address_for(const platform::SocketEndpoint& bound,
                          const platform::IpAddress& destination) const
      -> platform::IpAddress {
    if (!bound.address.is_unspecified()) return bound.address;
    if (destination.is_loopback() || destination.is_unspecified())
      return destination.is_ipv4() ? platform::IpAddress::loopback_ipv4()
                                   : platform::IpAddress::loopback_ipv6();
    const auto local = std::ranges::find_if(
        interface_list, [&](const platform::InterfaceInfo& info) {
          return info.address.family() == destination.family();
        });
    return local == interface_list.end() ? destination : local->address;
  }

  void fail_connect_locked(StreamState& state, platform::NetError error) {
    state.phase = StreamState::Phase::failed;
    state.error = error;
  }

  auto settle_connect_locked(StreamState& state) -> bool {
    if (connects_stalled_) return false;
    if (connect_error_) {
      fail_connect_locked(state, *connect_error_);
      return true;
    }
    if (!owns(state.target.address)) {
      fail_connect_locked(state, platform::NetError::host_unreachable);
      return true;
    }
    const auto listener = std::ranges::find_if(
        tcp_bindings_, [&](const std::shared_ptr<Binding>& binding) {
          return binding->listening &&
                 overlaps(binding->endpoint, state.target);
        });
    if (listener == tcp_bindings_.end()) {
      fail_connect_locked(state, platform::NetError::connection_refused);
      return true;
    }
    auto& target = **listener;
    if (target.pending.size() >= target.backlog) return false;
    auto connection = std::make_shared<Connection>();
    const auto destination =
        source_address_for(state.target, state.target.address);
    const auto source =
        source_address_for(state.binding->endpoint, destination);
    connection->endpoints[0] =
        platform::SocketEndpoint{source, state.binding->endpoint.port};
    connection->endpoints[1] = platform::SocketEndpoint{
        state.target.address.is_unspecified() ? destination
                                              : state.target.address,
        state.target.port};
    target.pending.push_back(connection);
    state.connection = std::move(connection);
    state.side = 0;
    state.phase = StreamState::Phase::connected;
    return true;
  }

  void settle_connects_locked() {
    std::erase_if(connecting_, [&](const std::shared_ptr<StreamState>& state) {
      return settle_connect_locked(*state);
    });
  }

  mutable platform::Mutex mutex_;
  platform::ConditionVariable activity_;
  const platform::ClockProvider& clock_;
  std::vector<std::shared_ptr<Port>> ports_;
  std::vector<std::shared_ptr<Binding>> tcp_bindings_;
  std::vector<std::shared_ptr<StreamState>> connecting_;
  std::vector<platform::SocketEndpoint> sent_;
  std::optional<platform::NetError> send_error_;
  std::optional<platform::NetError> receive_error_;
  std::optional<platform::NetError> connect_error_;
  std::optional<std::size_t> stream_send_limit_;
  bool connects_stalled_{};
  std::map<std::string, std::vector<platform::IpAddress>> hosts_;
  std::uint16_t next_port_{49152};
  Sockets sockets_{*this};
  Pollers pollers_{*this};
  Dns dns_{*this};
  Interfaces interfaces_{*this};
};

}  // namespace libchiaking::testing
