/// @file
/// @brief Declares the stock networking implementations built on the operating
/// system's sockets, resolver and interface list.
#pragma once

#include <libchiaking/platform/net/dns.hpp>
#include <libchiaking/platform/net/interfaces.hpp>
#include <libchiaking/platform/net/poller.hpp>
#include <libchiaking/platform/net/provider.hpp>
#include <libchiaking/platform/net/socket.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <chrono>
#include <vector>

namespace libchiaking::platform {

/// @brief Stock SocketProvider that creates sockets with the operating system's
/// socket API (BSD sockets or Winsock).
///
/// Every socket is opened non-blocking, is not inherited by child processes,
/// and reports the NetworkProvider given to the constructor as its owner. The
/// create calls never block and may be made from any thread. A socket itself
/// is not synchronised: one thread at a time may use it, although a poller may
/// watch it from another.
class DefaultSocketProvider final : public SocketProvider {
 public:
  /// @brief Creates a socket factory on behalf of a network provider.
  ///
  /// @param network The provider every created socket reports as its owner.
  ///     Borrowed; it must outlive this object and the sockets it creates.
  explicit DefaultSocketProvider(NetworkProvider& network) noexcept;
  /// @brief Destroys the factory; sockets already created are unaffected.
  ~DefaultSocketProvider() override;

  /// @brief Not copyable; the factory is tied to one NetworkProvider.
  DefaultSocketProvider(const DefaultSocketProvider&) = delete;
  /// @brief Not copy-assignable; the factory is tied to one NetworkProvider.
  DefaultSocketProvider& operator=(const DefaultSocketProvider&) = delete;

  /// @brief Opens a non-blocking UDP socket of the given address family.
  ///
  /// @param family_of Any address of the wanted family.
  /// @return The socket, owned by the caller.
  /// @retval NetError::resource_exhausted No descriptors are left.
  /// @retval NetError::no_buffers Memory for the socket could not be
  ///     allocated.
  [[nodiscard]] auto create_udp_socket(const IpAddress& family_of)
      -> Expected<std::unique_ptr<UdpSocket>, NetError> override;
  /// @brief Opens a non-blocking TCP socket of the given address family.
  ///
  /// @param family_of Any address of the wanted family.
  /// @return The socket, owned by the caller.
  /// @retval NetError::resource_exhausted No descriptors are left.
  /// @retval NetError::no_buffers Memory for the socket could not be
  ///     allocated.
  [[nodiscard]] auto create_tcp_socket(const IpAddress& family_of)
      -> Expected<std::unique_ptr<TcpSocket>, NetError> override;
  /// @brief Opens a non-blocking TCP listening socket of the given address
  /// family.
  ///
  /// @param family_of Any address of the wanted family.
  /// @return The listener, owned by the caller.
  /// @retval NetError::resource_exhausted No descriptors are left.
  /// @retval NetError::no_buffers Memory for the listener could not be
  ///     allocated.
  [[nodiscard]] auto create_tcp_listener(const IpAddress& family_of)
      -> Expected<std::unique_ptr<TcpListener>, NetError> override;

 private:
  NetworkProvider* network_{nullptr};
};

/// @brief Stock PollerProvider whose pollers wait with the operating system's
/// poll call and an internal wake-up channel.
///
/// Its pollers accept only the sockets made by DefaultSocketProvider for the
/// same NetworkProvider, and at most 8192 requests per wait. A wait sleeps in
/// slices of at most 10 ms, timed on the monotonic clock it is given, so that
/// it notices its stop token, and
/// Poller::wake may be called from any thread to end it early.
class DefaultPollerProvider final : public PollerProvider {
 public:
  /// @brief Creates a poller factory on behalf of a network provider.
  ///
  /// @param network The provider every created poller reports as its owner.
  ///     Borrowed; it must outlive this object and the pollers it creates.
  /// @param clock The monotonic clock the pollers time their waits on.
  ///     Borrowed; it must outlive this object and the pollers it creates.
  DefaultPollerProvider(NetworkProvider& network,
                        const ClockProvider& clock) noexcept;

  /// @brief Creates a poller with its own wake-up channel.
  ///
  /// @return The poller, owned by the caller.
  /// @retval NetError::no_buffers The wake-up channel or memory could not be
  ///     allocated.
  /// @retval NetError::system_failure Any other failure.
  [[nodiscard]] auto create_poller()
      -> Expected<std::unique_ptr<Poller>, NetError> override;

 private:
  NetworkProvider* network_{nullptr};
  const ClockProvider* clock_{nullptr};
};

/// @brief Stock DnsProvider that uses the operating system's resolver.
///
/// Both overloads may be called from any thread at once. Lookups that a
/// cancelled resolve() abandoned keep running on threads the resolver owns;
/// they are joined once finished, and at the latest when the resolver is
/// destroyed, which waits for any still running.
class DefaultDnsProvider final : public DnsProvider {
 public:
  /// @brief Longest time the cancellable resolve() waits for the system
  /// resolver before it returns NetError::timed_out.
  static constexpr std::chrono::seconds cancellable_resolve_timeout{30};

  /// @brief Creates the resolver.
  ///
  /// @param threading Starts the threads cancellable lookups run on.
  ///     Borrowed; it must outlive this object.
  /// @param clock The monotonic clock cancellable_resolve_timeout is
  ///     measured on. Borrowed; it must outlive this object.
  DefaultDnsProvider(ThreadingProvider& threading, const ClockProvider& clock);
  /// @brief Joins every abandoned lookup, waiting for those still running.
  ~DefaultDnsProvider() override;
  /// @brief Not copyable; it owns the abandoned lookups' threads.
  DefaultDnsProvider(const DefaultDnsProvider&) = delete;
  /// @brief Not copy-assignable; it owns the abandoned lookups' threads.
  DefaultDnsProvider& operator=(const DefaultDnsProvider&) = delete;
  /// @brief Resolves a host name and returns early when the stop token is
  /// stopped.
  ///
  /// A numeric address is returned directly. Otherwise the lookup runs on a
  /// thread of its own while the caller waits for the result, the stop token
  /// or cancellable_resolve_timeout. A lookup that is abandoned finishes in
  /// the background and its result is discarded; its thread is kept and
  /// joined later.
  ///
  /// @param host Host name or numeric address.
  /// @param service Port number or service name, or empty for none.
  /// @param stop Cancels the wait when stopped.
  /// @return Every address found.
  /// @retval NetError::timed_out The stop token's deadline expired, or the
  ///     resolver took longer than cancellable_resolve_timeout.
  /// @retval NetError::interrupted The stop token was stopped for another
  ///     reason.
  /// @retval NetError::system_failure The lookup thread could not be started.
  /// @retval NetError::host_unreachable The name does not resolve.
  [[nodiscard]] auto resolve(std::string_view host, std::string_view service,
                             const StopToken& stop)
      -> Expected<std::vector<IpAddress>, NetError> override;

  /// @brief Resolves a host name with the system resolver, blocking the
  /// caller until it answers.
  ///
  /// @param host Host name or numeric address.
  /// @param service Port number or service name, or empty for none.
  /// @return Every IPv4 and IPv6 address found, in resolver order.
  /// @retval NetError::host_unreachable The name does not exist or has no
  ///     usable address.
  /// @retval NetError::timed_out The resolver reported a temporary failure.
  /// @retval NetError::no_buffers The resolver ran out of memory.
  [[nodiscard]] auto resolve(std::string_view host, std::string_view service)
      -> Expected<std::vector<IpAddress>, NetError> override;

 private:
  /// @brief Keeps an abandoned lookup's thread, joining any kept earlier
  /// that have finished.
  ///
  /// @param lookup The thread.
  void keep(Thread lookup);

  /// @brief Starts the lookup threads.
  ThreadingProvider* threading_;
  /// @brief Times the lookups.
  const ClockProvider* clock_;
  /// @brief Guards abandoned_.
  Mutex abandoned_guard_;
  /// @brief Threads of lookups whose callers stopped waiting.
  std::vector<Thread> abandoned_;
};

/// @brief Stock InterfacesProvider that reads the operating system's interface
/// list.
///
/// On POSIX systems it leaves InterfaceInfo::medium unclassified and
/// InterfaceInfo::lan_adapter true for every entry. On Windows it classifies
/// Ethernet and wireless adapters and marks every other adapter type as not a
/// LAN adapter.
class DefaultInterfacesProvider final : public InterfacesProvider {
 public:
  /// @brief Creates the lister; it holds no state.
  DefaultInterfacesProvider() = default;

  /// @brief Reads the current interface addresses from the operating system.
  ///
  /// Blocks only for the system call, and may be called from any thread.
  ///
  /// @return One InterfaceInfo per assigned IPv4 or IPv6 address.
  /// @retval NetError::system_failure The system call failed. Other NetError
  ///     values are returned when the system reports a more specific cause.
  [[nodiscard]] auto enumerate()
      -> Expected<std::vector<InterfaceInfo>, NetError> override;
};

/// @brief Stock NetworkProvider that owns one each of DefaultSocketProvider,
/// DefaultPollerProvider, DefaultDnsProvider and DefaultInterfacesProvider.
///
/// DefaultPlatformFactory uses one of these when the host supplies no
/// PlatformProviders::network. Besides the operating system it depends only
/// on the threading provider and monotonic clock it is given.
class DefaultNetworkProvider final : public NetworkProvider {
 public:
  /// @brief Creates the provider and makes it the owner of every socket and
  /// poller it will create.
  ///
  /// @param threading Starts the threads cancellable DNS lookups run on.
  ///     Borrowed; it must outlive this object.
  /// @param clock The monotonic clock pollers and DNS lookups are timed on.
  ///     Borrowed; it must outlive this object.
  DefaultNetworkProvider(ThreadingProvider& threading,
                         const ClockProvider& clock)
      : sockets_(*this), pollers_(*this, clock), dns_(threading, clock) {}

  /// @brief Returns the built-in DefaultSocketProvider.
  ///
  /// @return The provider, valid for the lifetime of this object.
  SocketProvider& sockets() noexcept override;
  /// @brief Returns the built-in DefaultPollerProvider.
  ///
  /// @return The provider, valid for the lifetime of this object.
  PollerProvider& pollers() noexcept override;
  /// @brief Returns the built-in DefaultDnsProvider.
  ///
  /// @return The resolver, valid for the lifetime of this object.
  DnsProvider& dns() noexcept override;
  /// @brief Returns the built-in DefaultInterfacesProvider.
  ///
  /// @return The lister, valid for the lifetime of this object.
  InterfacesProvider& interfaces() noexcept override;
  /// @brief Reports whether this operating system lets UDP sockets forbid
  /// fragmentation, for IPv4 and for IPv6.
  ///
  /// @return The don't-fragment support probed once per process on POSIX
  ///     systems, and always both on Windows. The first call opens two short-
  ///     lived probe sockets; later calls return the cached answer.
  NetworkCapabilities capabilities() const noexcept override;

 private:
  DefaultSocketProvider sockets_;
  DefaultPollerProvider pollers_;
  DefaultDnsProvider dns_;
  DefaultInterfacesProvider interfaces_;
};

inline DefaultSocketProvider::DefaultSocketProvider(
    NetworkProvider& network) noexcept
    : network_(&network) {}

inline DefaultPollerProvider::DefaultPollerProvider(
    NetworkProvider& network, const ClockProvider& clock) noexcept
    : network_(&network), clock_(&clock) {}

}  // namespace libchiaking::platform
