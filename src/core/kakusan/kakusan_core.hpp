/// @file
/// @brief Declares Surroundings, everything a device's Kakusan context
/// borrows from the platform: crypto, randomness, time, the device's identity,
/// its remembered devices and how it reaches the network.
///
/// Kakusan is a handle on one Surroundings; every SharedSession and
/// GuestVisit made from it shares it too, so it lives until the last of them is
/// gone.
#pragma once

#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/detail/kakusan/kakusan.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "libchiaking/platform/dtls/provider.hpp"
#include "libchiaking/platform/factory.hpp"
#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/kakusan/adapters/connections.hpp"
#include "src/core/kakusan/model/clock.hpp"
#include "src/core/net/kakusan/peer_link.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::kakusan {

/// @brief The Kakusan context of this device, as the rest of Kakusan
/// sees it.
///
/// Holds the providers borrowed from the platform, the device's DeviceIdentity
/// (restored from the DeviceStore or created and saved the first time) and its
/// RememberedDevices, and the network settings each new session or visit
/// starts with. Exactly one way of reaching the network is held: a transport
/// factory lent by an embedder or test, or the platform's network and secure
/// datagram providers with the STUN servers and port guessing to use.
///
/// Only ever made by of(), always behind a std::shared_ptr. Not copyable. The
/// identity never changes after construction; RememberedDevices carries its own
/// lock; the network settings are guarded by a mutex of their own. Every member
/// may therefore be called from any thread.
class Surroundings {
 public:
  /// @brief Reads the time; called for every Clock a step needs, from any
  /// thread, so it must be thread-safe.
  using ClockSource = std::function<Clock()>;

  /// @brief The parts of a context supplied directly instead of from a
  /// PlatformFactory, for tests and embedders.
  ///
  /// Plain data; every reference and pointer must outlive the context.
  struct Injected {
    /// @brief Crypto for identities, invitations and links.
    platform::CryptoProvider& crypto;
    /// @brief Randomness for keys, ids and nonces.
    platform::SecureRandomProvider& random;
    /// @brief The time source; must not be empty.
    ClockSource clock;
    /// @brief Where the identity and remembered devices are kept.
    DeviceStore& store;
    /// @brief Makes the locks and condition variables of the context and
    /// of every session and visit made from it.
    platform::ThreadingProvider& threading;
    /// @brief Where log lines go; null drops them.
    platform::DiagnosticsProvider* log{};
    /// @brief A transport factory to lend every session and visit; when
    /// set, network and secure_datagram are ignored.
    net::kakusan::SpokeTransportFactory* transports{};
    /// @brief Sockets and DNS, used with secure_datagram when no transports
    /// are lent.
    platform::NetworkProvider* network{};
    /// @brief Encryption for each link, used with network when no
    /// transports are lent.
    platform::SecureDatagramProvider* secure_datagram{};
  };

  /// @brief Builds the context from a platform, as Kakusan::create does.
  ///
  /// Checks the platform in the order the failures are listed, then loads or
  /// creates the identity and loads the remembered devices, blocking while
  /// the store reads and writes. Time comes from the platform's monotonic
  /// clock and wall clock, and locks from its threading provider.
  ///
  /// @param platform Source of every provider; must outlive the context.
  /// @param store Persistence for the identity and remembered devices; must
  ///     outlive the context.
  /// @param stun_servers The STUN servers sessions and visits start with.
  /// @return The context.
  /// @retval Kakusan::Unavailable::no_secure_transport The platform has no
  ///     secure datagram provider.
  /// @retval Kakusan::Unavailable::no_identity_crypto The crypto provider
  ///     is missing or fails has_identity_crypto(), or the saved identity
  ///     could not be restored or a new one created.
  /// @retval Kakusan::Unavailable::invalid_stun_servers The list fails
  ///     usable().
  /// @retval Kakusan::Unavailable::device_store_unavailable The store
  ///     failed to load or save.
  [[nodiscard]] static auto of(platform::PlatformFactory& platform,
                               DeviceStore& store,
                               std::span<const std::string> stun_servers)
      -> Expected<std::shared_ptr<Surroundings>, Kakusan::Unavailable>;
  /// @brief Builds the context from parts supplied directly.
  ///
  /// Without a lent transport factory, sessions and visits use the given
  /// network and secure datagram providers with no STUN servers until
  /// use_stun_servers() is called. Blocks while the store reads and writes.
  ///
  /// @param injected The parts; see Injected for their lifetimes.
  /// @return The context.
  /// @retval Kakusan::Unavailable::no_secure_transport Neither a transport
  ///     factory nor both the network and secure datagram providers were
  ///     given.
  /// @retval Kakusan::Unavailable::no_identity_crypto The clock source is
  ///     empty, the crypto fails has_identity_crypto(), or the identity could
  ///     not be restored or created.
  /// @retval Kakusan::Unavailable::device_store_unavailable The store
  ///     failed to load or save.
  [[nodiscard]] static auto of(Injected injected)
      -> Expected<std::shared_ptr<Surroundings>, Kakusan::Unavailable>;
  /// @brief Tells whether a crypto provider can do everything Kakusan
  /// needs, by running each primitive once on fixed inputs.
  ///
  /// Exercises SHA-512, HMAC-SHA-512, HMAC-SHA-1, scrypt, Ed25519 key
  /// derivation, signing and verification, and P-256 base and point
  /// multiplication and addition. Only the calls' success is checked, plus
  /// that the signature verifies and that multiplying the generator by one
  /// gives it back; outputs are not compared with known answers. scrypt runs
  /// with the smallest parameters, so the check is quick.
  ///
  /// @param crypto The provider to try.
  /// @return True when every primitive succeeded.
  static auto has_identity_crypto(platform::CryptoProvider& crypto) -> bool;

  /// @brief Not copyable; the context is shared through std::shared_ptr.
  Surroundings(const Surroundings&) = delete;
  /// @brief Not copy-assignable; the context is shared through
  /// std::shared_ptr.
  auto operator=(const Surroundings&) -> Surroundings& = delete;

  /// @brief The crypto provider.
  /// @return The provider; valid while the context lives.
  auto crypto() const noexcept -> platform::CryptoProvider& { return *crypto_; }
  /// @brief The secure random provider.
  /// @return The provider; valid while the context lives.
  auto random() const noexcept -> platform::SecureRandomProvider& {
    return *random_;
  }
  /// @brief A platform factory for the guest's forwarded stream, whose clock
  /// is this context's time source rather than the platform's own.
  ///
  /// It carries this context's threading provider, random source, crypto
  /// provider, network when it has one, and log, so the stream a guest
  /// receives runs on the same providers and the same time as the visit,
  /// including time driven by hand in tests.
  ///
  /// @return The factory; valid while the context lives.
  auto stream_platform() noexcept -> platform::PlatformFactory& {
    return stream_platform_;
  }
  /// @brief The threading provider every lock and condition variable of the
  /// context, its sessions and its visits is made from.
  /// @return The provider; valid while the context lives.
  auto threading() const noexcept -> platform::ThreadingProvider& {
    return *threading_;
  }
  /// @brief The monotonic part of the time source as a ClockProvider, for
  /// the condition variables of sessions and visits.
  /// @return The clock; valid while the context lives.
  auto steady() const noexcept -> const platform::ClockProvider& {
    return stream_clock_;
  }
  /// @brief Reads the time source.
  /// @return The current instant.
  auto now() const -> Clock { return clock_(); }
  /// @brief The wall-clock time of now(), as a time point, for stamping
  /// remembered devices.
  /// @return The current wall-clock time, in whole seconds.
  auto wall() const -> std::chrono::system_clock::time_point {
    return std::chrono::system_clock::time_point{
        std::chrono::seconds{clock_().wall_seconds}};
  }
  /// @brief This device's Kakusan identity.
  /// @return The identity; valid and unchanging while the context lives.
  auto identity() const noexcept -> const DeviceIdentity& { return identity_; }
  /// @brief The devices the user has remembered.
  /// @return The collection; valid while the context lives.
  auto remembered() noexcept -> RememberedDevices& { return *remembered_; }
  /// @brief Tells whether a STUN server list may be configured.
  ///
  /// Checks only its shape, not whether the names resolve.
  ///
  /// @param stun_servers The list.
  /// @return True when it has at most Kakusan::most_stun_servers entries,
  ///     none of them empty or longer than Kakusan::longest_stun_server.
  static auto usable(std::span<const std::string> stun_servers) noexcept
      -> bool;
  /// @brief Replaces the STUN servers later sessions and visits start with.
  ///
  /// Takes the network settings lock briefly. Has no effect, though it still
  /// succeeds, when a transport factory was lent.
  ///
  /// @param stun_servers The new list.
  /// @return Nothing on success.
  /// @retval Kakusan::Unavailable::invalid_stun_servers The list fails
  ///     usable(); the old list is kept.
  [[nodiscard]] auto use_stun_servers(std::span<const std::string> stun_servers)
      -> Expected<void, Kakusan::Unavailable>;
  /// @brief Chooses when later sessions and visits offer guessed public
  /// ports.
  ///
  /// Takes the network settings lock briefly. Has no effect when a transport
  /// factory was lent.
  ///
  /// @param guessing The new choice.
  void use_port_guessing(Kakusan::PortGuessing guessing);
  /// @brief Makes the transport source for a new session or visit.
  ///
  /// With a lent factory the result borrows it. Otherwise it builds a UDP
  /// factory with the STUN servers and port guessing in force now, resolving
  /// the server names through DNS, so it blocks for as long as the lookups
  /// take. The network settings lock is held meanwhile.
  ///
  /// @return The source; never null.
  auto connections() const -> std::unique_ptr<Connections>;
  /// @brief Writes one line to the platform log, if there is one.
  ///
  /// @param severity How serious the line is.
  /// @param message The text.
  void log(platform::LogSeverity severity,
           std::string_view message) const noexcept;

 private:
  /// @brief Presents the context's time source as a platform ClockProvider,
  /// for stream_platform().
  ///
  /// Refers to the source it was made with and must not outlive it;
  /// thread-safe as the source is.
  class StreamClock final : public platform::ClockProvider {
   public:
    /// @brief Wraps a time source.
    /// @param source The source; must outlive this clock.
    explicit StreamClock(const ClockSource& source) noexcept
        : source_(source) {}

    /// @brief Reads the monotonic part of the source.
    /// @return Clock::now_us, in microseconds.
    std::uint64_t now_microseconds() const noexcept override {
      return source_().now_us;
    }

   private:
    /// @brief The wrapped time source.
    const ClockSource& source_;
  };

  /// @brief The wall part of a time source as a ClockProvider, for
  /// stream_platform() when no wall clock was supplied.
  class StreamWallClock final : public platform::ClockProvider {
   public:
    /// @brief Wraps a time source.
    /// @param source The source; must outlive this clock.
    explicit StreamWallClock(const ClockSource& source) noexcept
        : source_(source) {}

    /// @brief Reads the wall part of the source.
    /// @return Clock::wall_seconds, in microseconds since the Unix epoch.
    std::uint64_t now_microseconds() const noexcept override {
      return source_().wall_seconds * 1'000'000;
    }

   private:
    /// @brief The wrapped time source.
    const ClockSource& source_;
  };

  /// @brief How a context reaches the network when no transport factory is
  /// lent.
  ///
  /// Plain data, guarded by network_mutex_ once inside a context.
  struct Network {
    /// @brief Encryption for each link.
    platform::SecureDatagramProvider& secure_datagram;
    /// @brief Sockets and DNS.
    platform::NetworkProvider& network;
    /// @brief The STUN servers a new session or visit starts with.
    std::vector<std::string> stun_servers;
    /// @brief When a new session or visit offers guessed public ports.
    net::nat::ice::PortGuessing guessing{
        net::nat::ice::PortGuessing::automatic};
  };

  /// @brief Stores the assembled parts; reached only through assemble().
  ///
  /// @param crypto The crypto provider; must outlive the context.
  /// @param random The random provider; must outlive the context.
  /// @param clock The time source.
  /// @param threading The threading provider; must outlive the context.
  /// @param wall The wall clock handed to stream_platform(), or null to read
  ///     the wall part of `clock`.
  /// @param log The log, or null.
  /// @param identity This device's identity.
  /// @param remembered The remembered devices, already loaded.
  /// @param network How the network is reached; empty when `transports` is
  ///     set.
  /// @param transports A lent transport factory, or null.
  Surroundings(platform::CryptoProvider& crypto,
               platform::SecureRandomProvider& random, ClockSource clock,
               platform::ThreadingProvider& threading,
               platform::ClockProvider* wall,
               platform::DiagnosticsProvider* log, DeviceIdentity identity,
               std::unique_ptr<RememberedDevices> remembered,
               std::optional<Network> network,
               net::kakusan::SpokeTransportFactory* transports);

  /// @brief Loads the identity and remembered devices and builds the context.
  ///
  /// Blocks while the store reads and, the first time, saves the identity.
  ///
  /// @param crypto The crypto provider.
  /// @param random The random provider.
  /// @param clock The time source.
  /// @param threading The threading provider.
  /// @param wall The wall clock for stream_platform(), or null to read the
  ///     wall part of `clock`.
  /// @param log The log, or null.
  /// @param store Where the identity and remembered devices are kept.
  /// @param network How the network is reached, or nothing with `transports`.
  /// @param transports A lent transport factory, or null.
  /// @return The context.
  /// @retval Kakusan::Unavailable::no_identity_crypto The identity could
  ///     not be restored or created.
  /// @retval Kakusan::Unavailable::device_store_unavailable The store
  ///     failed.
  [[nodiscard]] static auto assemble(
      platform::CryptoProvider& crypto, platform::SecureRandomProvider& random,
      ClockSource clock, platform::ThreadingProvider& threading,
      platform::ClockProvider* wall, platform::DiagnosticsProvider* log,
      DeviceStore& store, std::optional<Network> network,
      net::kakusan::SpokeTransportFactory* transports)
      -> Expected<std::shared_ptr<Surroundings>, Kakusan::Unavailable>;
  /// @brief Restores the saved identity, or creates one and saves it.
  ///
  /// Blocks while the store reads and writes.
  ///
  /// @param crypto The crypto provider.
  /// @param random Randomness for a new identity's key.
  /// @param store Where the identity is kept.
  /// @return The identity.
  /// @retval Kakusan::Unavailable::device_store_unavailable Loading or
  ///     saving failed.
  /// @retval Kakusan::Unavailable::no_identity_crypto The saved identity
  ///     is unreadable, or a new one could not be created.
  [[nodiscard]] static auto own_identity(platform::CryptoProvider& crypto,
                                         platform::SecureRandomProvider& random,
                                         DeviceStore& store)
      -> Expected<DeviceIdentity, Kakusan::Unavailable>;

  /// @brief The crypto provider; never null.
  platform::CryptoProvider* crypto_;
  /// @brief The random provider; never null.
  platform::SecureRandomProvider* random_;
  /// @brief The time source; never empty.
  ClockSource clock_;
  /// @brief The threading provider; never null.
  platform::ThreadingProvider* threading_;
  /// @brief The log, or null to drop lines.
  platform::DiagnosticsProvider* log_;
  /// @brief This device's identity; never changes.
  DeviceIdentity identity_;
  /// @brief The remembered devices; never null.
  std::unique_ptr<RememberedDevices> remembered_;
  /// @brief Guards network_.
  mutable platform::Mutex network_mutex_;
  /// @brief How the network is reached; present exactly when no transport
  /// factory is lent.
  std::optional<Network> network_;
  /// @brief The lent transport factory, or null.
  net::kakusan::SpokeTransportFactory* transports_;
  /// @brief clock_ as a ClockProvider, for stream_platform_.
  StreamClock stream_clock_;
  /// @brief The wall part of clock_, for stream_platform_ when no wall clock
  /// was supplied.
  StreamWallClock stream_wall_clock_;
  /// @brief The factory stream_platform() returns.
  platform::DefaultPlatformFactory stream_platform_;
};

}  // namespace libchiaking::kakusan
