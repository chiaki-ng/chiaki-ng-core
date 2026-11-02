/// @file
/// @brief Declares ServerConnect, which connects a cloud link to its
/// allocated server, and the values it needs: the calibration, the server's
/// endpoint, the handshake key and the prepared handshake.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/platform/net/dns.hpp"
#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/protocol/remote_play/ecdh_wire.hpp"
#include "src/core/net/protocol/remote_play/stream/active_session.hpp"
#include "src/core/net/protocol/remote_play/stream/route.hpp"
#include "src/core/net/protocol/senkusha/types.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/net/transport/datagram/transport.hpp"
#include "src/core/psn/gk/client.hpp"
#include "src/core/psn/gk/datacenter_ping.hpp"
#include "src/core/psn/gk/measurement.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace libchiaking::play {

/// @brief Why connecting to an allocated cloud server failed.
///
/// A value; the reason decides the stage.
class ConnectError {
 public:
  /// @brief Which half of connecting failed.
  enum class Stage : std::uint8_t {
    /// @brief Reaching the server: calibration, address or socket.
    route,
    /// @brief Starting the active session on the reached route.
    active,
  };

  /// @brief What failed.
  enum class Reason : std::uint8_t {
    /// @brief The selected datacenter was not measured.
    not_measured,
    /// @brief No Takion version or wire profile exists for the service.
    unsupported_takion_version,
    /// @brief The allocation names no server.
    invalid_host,
    /// @brief The server's host could not be resolved.
    dns_failure,
    /// @brief The host has no address of the family asked for.
    no_address,
    /// @brief The socket to the server could not be opened.
    transport_failure,
    /// @brief The route is missing, of another kind, or calibrated for another
    /// version or service.
    invalid_route,
    /// @brief The allocation carries no launch specification.
    invalid_launch_specification,
    /// @brief The handshake key does not decode to the expected length.
    handshake_key_length,
    /// @brief The handshake key is not valid base64.
    handshake_key_encoding,
    /// @brief The calibration's wire profile does not match its version.
    invalid_profile,
    /// @brief The crypto provider does not offer the profile's ECDH curve.
    unsupported_curve,
    /// @brief Preparing the ECDH exchange failed.
    ecdh_failure,
    /// @brief The active session could not start.
    start_failed,
  };

  /// @brief Builds the error.
  ///
  /// @param reason What failed.
  explicit constexpr ConnectError(Reason reason) noexcept : reason_(reason) {}

  /// @brief Reads what failed.
  ///
  /// @return The reason.
  constexpr auto reason() const noexcept -> Reason { return reason_; }
  /// @brief Names the half that failed.
  ///
  /// @return Stage::route up to a transport failure, Stage::active after.
  constexpr auto stage() const noexcept -> Stage {
    return reason_ <= Reason::transport_failure ? Stage::route : Stage::active;
  }

  /// @brief Compares reasons.
  ///
  /// @return `true` when both name the same reason.
  friend constexpr auto operator==(const ConnectError&,
                                   const ConnectError&) noexcept
      -> bool = default;

 private:
  /// @brief What failed.
  Reason reason_;
};

/// @brief The server's ECDH handshake key, decoded from PS's base64 and
/// held in wiping storage. Move-only in effect; built by HandshakeKey::decode.
class HandshakeKey {
 public:
  /// @brief The key's wiping storage.
  using Bytes =
      SecretArray<net::protocol::remote_play::ecdh_handshake_key_size>;

  /// @brief Decodes the key.
  ///
  /// @param encoded The base64 key PS sent.
  /// @return The key; Reason::handshake_key_length for a key of the wrong
  ///     length; Reason::handshake_key_encoding for bad base64.
  [[nodiscard]] static auto decode(std::string_view encoded)
      -> Expected<HandshakeKey, ConnectError>;

  /// @brief Reads the key.
  ///
  /// @return The bytes; valid while the key lives.
  auto bytes() const noexcept -> std::span<const std::byte> {
    return {bytes_.data(), bytes_.size()};
  }
  /// @brief Gives up the key's storage.
  ///
  /// @return The storage.
  auto release() && noexcept -> Bytes { return std::move(bytes_); }

 private:
  /// @brief Builds an empty key for HandshakeKey::decode to fill.
  HandshakeKey() noexcept = default;

  /// @brief The key.
  Bytes bytes_;
};

/// @brief What a cloud stream knows of its route before connecting: the
/// service, the Takion version and the round trip and MTUs.
///
/// A value built from a datacenter measurement or an allocation.
class Calibration {
 public:
  /// @brief Picks the Takion version of a service.
  ///
  /// @param service LegacyCloudStream or CloudStream.
  /// @param takion_version_override A version to use instead; 0 for none.
  /// @return The version; 0 for another service.
  static auto takion_version(psn::StreamingService service,
                             std::uint32_t takion_version_override = 0) noexcept
      -> std::uint32_t;

  /// @brief Builds the calibration from the selected datacenter's
  /// measurement.
  ///
  /// @param row The measurement.
  /// @param service The service.
  /// @param takion_version_override A version to use instead; 0 for none.
  /// @return The calibration, with the reported round trip (or the default)
  ///     and the MTUs (or the defaults); Reason::not_measured for an unmeasured
  ///     row; Reason::unsupported_takion_version.
  [[nodiscard]] static auto from_ping(const psn::gk::Ping& row,
                                      psn::StreamingService service,
                                      std::uint32_t takion_version_override = 0)
      -> Expected<Calibration, ConnectError>;
  /// @brief Builds the calibration from an allocation's own figures.
  ///
  /// @param service The service.
  /// @param takion_version_override A version to use instead; 0 for none.
  /// @param mtu_in The inbound MTU, in bytes; 0 when unknown.
  /// @param mtu_out The outbound MTU, in bytes; 0 when unknown.
  /// @param rtt_us The round trip, in microseconds; 0 when unknown.
  /// @return The calibration; the default MTUs and a 1 ms round trip unless
  ///     both MTUs are known; Reason::unsupported_takion_version.
  [[nodiscard]] static auto from_allocation(
      psn::StreamingService service, std::uint32_t takion_version_override,
      std::uint32_t mtu_in, std::uint32_t mtu_out, std::uint64_t rtt_us)
      -> Expected<Calibration, ConnectError>;

  /// @brief Reads the service.
  ///
  /// @return The service.
  auto service() const noexcept -> psn::StreamingService { return service_; }
  /// @brief Reads the measurement the route is calibrated with.
  ///
  /// @return The Senkusha result.
  auto measurement() const noexcept -> const net::protocol::senkusha::Result& {
    return measurement_;
  }

 private:
  /// @brief The round trip assumed when none was measured, in microseconds.
  static constexpr std::uint64_t unmeasured_rtt_us = 1000;

  /// @brief Builds steady round trip statistics around a mean.
  ///
  /// @param mean The mean round trip.
  /// @return Statistics with that raw and adjusted mean.
  static auto steady_rtt(std::chrono::microseconds mean)
      -> net::protocol::senkusha::RttStatistics;

  /// @brief Builds a calibration.
  ///
  /// @param service The service.
  /// @param measurement The measurement.
  Calibration(psn::StreamingService service,
              net::protocol::senkusha::Result measurement) noexcept
      : service_(service), measurement_(std::move(measurement)) {}

  /// @brief The service.
  psn::StreamingService service_;
  /// @brief The measurement.
  net::protocol::senkusha::Result measurement_;
};

/// @brief The socket address of an allocated server.
class AllocatedEndpoint {
 public:
  /// @brief The port used when the allocation gives none.
  static constexpr std::uint16_t default_port = 9296;

  /// @brief Resolves the server's address; blocks on DNS.
  ///
  /// An address containing a colon asks for IPv6, anything else for IPv4.
  ///
  /// @param dns The resolver.
  /// @param host The server's host.
  /// @param port The server's port; 0 for AllocatedEndpoint::default_port.
  /// @return The endpoint; Reason::invalid_host, Reason::dns_failure or
  ///     Reason::no_address.
  [[nodiscard]] static auto resolve(platform::DnsProvider& dns,
                                    std::string_view host, std::uint16_t port)
      -> Expected<AllocatedEndpoint, ConnectError>;

  /// @brief Reads the address.
  ///
  /// @return The endpoint.
  auto socket() const noexcept -> const platform::SocketEndpoint& {
    return socket_;
  }

 private:
  /// @brief Builds an endpoint.
  ///
  /// @param socket The address.
  explicit AllocatedEndpoint(platform::SocketEndpoint socket) noexcept
      : socket_(socket) {}

  /// @brief The address.
  platform::SocketEndpoint socket_;
};

/// @brief The secrets an active cloud session starts with: the decoded key,
/// the prepared ECDH exchange, the session id and the launch specification.
class Handshake {
 public:
  /// @brief Checks the allocation against the calibration and prepares the
  /// handshake.
  ///
  /// @param crypto Prepares the ECDH exchange.
  /// @param allocation PS's allocation.
  /// @param calibration The route's calibration.
  /// @param expected_version The Takion version required; 0 to accept
  /// LegacyCloudStream's
  ///     or CloudStream's.
  /// @return The handshake; Reason::invalid_launch_specification; the key's
  ///     error; Reason::invalid_profile; Reason::unsupported_curve;
  ///     Reason::ecdh_failure.
  [[nodiscard]] static auto prepare(
      platform::CryptoProvider& crypto, const psn::gk::Allocation& allocation,
      const net::protocol::senkusha::Result& calibration,
      std::uint32_t expected_version = 0) -> Expected<Handshake, ConnectError>;

  /// @brief Reads the key.
  ///
  /// @return The key.
  auto key() const noexcept -> const HandshakeKey& { return key_; }
  /// @brief Reads the prepared ECDH exchange.
  ///
  /// @return The exchange.
  auto ecdh() const noexcept
      -> const net::protocol::remote_play::PreparedEcdh& {
    return ecdh_;
  }
  /// @brief Reads the session id.
  ///
  /// @return The id, cut to fit the wire.
  auto session_id() const noexcept -> const SecretString& {
    return *session_id_;
  }
  /// @brief Reads the launch specification.
  ///
  /// @return The specification.
  auto launch_specification() const noexcept -> const SecretString& {
    return *launch_specification_;
  }

 private:
  friend class ServerConnect;

  /// @brief Builds a handshake.
  ///
  /// @param key The key.
  /// @param ecdh The prepared exchange.
  /// @param session_id The session id.
  /// @param launch_specification The launch specification.
  Handshake(HandshakeKey key, net::protocol::remote_play::PreparedEcdh ecdh,
            std::shared_ptr<const SecretString> session_id,
            std::shared_ptr<const SecretString> launch_specification) noexcept;

  /// @brief The key.
  HandshakeKey key_;
  /// @brief The prepared exchange.
  net::protocol::remote_play::PreparedEcdh ecdh_;
  /// @brief The session id.
  std::shared_ptr<const SecretString> session_id_;
  /// @brief The launch specification.
  std::shared_ptr<const SecretString> launch_specification_;
};

/// @brief How an active cloud session is set up. Plain data.
struct ConnectOptions {
  /// @brief Whether the server emulates a DualSense rather than a DualShock 4.
  bool dualsense{true};
  /// @brief Whether to ask for a keyframe after a frame FEC cannot repair.
  bool enable_idr_on_fec_failure{};
  /// @brief Developer tuning, audio channels and version negotiation.
  net::protocol::remote_play::SessionTuning tuning{};
  /// @brief How many pads the session starts with.
  std::uint8_t pad_count{1};
};

/// @brief Connects to an allocated cloud server: opens the UDP route and
/// starts the active session on it.
///
/// Holds borrowed providers; every call blocks on DNS and the session's
/// first exchange. Used on the cloud link's thread.
class ServerConnect {
 public:
  /// @brief The providers a connection uses, all borrowed.
  struct Providers {
    /// @brief Resolves the host and opens the socket.
    platform::NetworkProvider& network;
    /// @brief The monotonic clock.
    platform::ClockProvider& clock;
    /// @brief Makes the Takion client's mutex.
    platform::ThreadingProvider& threading;
    /// @brief Randomness for the session.
    platform::SecureRandomProvider& random;
    /// @brief Prepares the ECDH exchange.
    platform::CryptoProvider& crypto;
    /// @brief Seals and opens Takion packets.
    net::protocol::takion::CryptoProvider& takion_crypto;
  };

  /// @brief How the session is set up.
  using Options = ConnectOptions;

  /// @brief A started session and what to poll for it.
  struct Connected {
    /// @brief The active session.
    std::unique_ptr<net::protocol::remote_play::ActiveSession> session;
    /// @brief The route's pollable.
    const platform::Pollable* pollable{};
  };

  /// @brief How long a cloud stream may receive nothing before it fails:
  /// 20 s, reset by any packet the server sends.
  static constexpr std::uint64_t peer_silence_timeout_us = 20'000'000;

  /// @brief Builds the connector.
  ///
  /// @param providers The providers; must outlive the connector.
  explicit ServerConnect(Providers providers) noexcept
      : providers_(providers) {}

  /// @brief Builds the Takion client settings for a PSN wrapper type.
  ///
  /// @param psn_wrapper_type The allocation's wrapper type; 0 for none.
  /// @return Settings with the outgoing envelope and a plain receive
  ///     envelope.
  static auto takion_config(std::uint8_t psn_wrapper_type)
      -> net::protocol::takion::ClientConfig;
  /// @brief Builds the active session's control profile.
  ///
  /// @param service The service; CloudStream uses HEVC and cloud audio
  ///     assembly.
  /// @param options How the session is set up.
  /// @return The profile, with the version negotiation offer when asked.
  static auto control_profile(psn::StreamingService service,
                              const Options& options) noexcept
      -> net::protocol::remote_play::ActiveControlProfile;

  /// @brief Opens the route and starts the session.
  ///
  /// @param allocation PS's allocation.
  /// @param calibration The route's calibration.
  /// @param options How the session is set up.
  /// @return The session; the route's or the start's error.
  [[nodiscard]] auto connect(const psn::gk::Allocation& allocation,
                             Calibration calibration, Options options = {})
      -> Expected<Connected, ConnectError>;
  /// @brief Opens a connected UDP socket to the server.
  ///
  /// @param allocation PS's allocation.
  /// @param calibration The route's calibration.
  /// @return The route; the endpoint's error or Reason::transport_failure.
  [[nodiscard]] auto route(const psn::gk::Allocation& allocation,
                           Calibration calibration)
      -> Expected<net::protocol::remote_play::EstablishedRoute, ConnectError>;
  /// @brief Starts the active session on a route; the route is closed on
  /// failure.
  ///
  /// @param route The route.
  /// @param allocation PS's allocation.
  /// @param service The service.
  /// @param options How the session is set up.
  /// @return The session; Reason::invalid_route; the handshake's error;
  ///     Reason::start_failed.
  [[nodiscard]] auto start(net::protocol::remote_play::EstablishedRoute route,
                           const psn::gk::Allocation& allocation,
                           psn::StreamingService service, Options options = {})
      -> Expected<Connected, ConnectError>;

 private:
  /// @brief Bytes kept free in each datagram for the envelope.
  static constexpr std::uint32_t reserved_envelope_bytes = 4;

  /// @brief Gives the outgoing envelope for a PSN wrapper type.
  ///
  /// @param psn_wrapper_type The wrapper type; 0 for none.
  /// @return The PSN envelope, or none.
  static auto active_envelope(std::uint8_t psn_wrapper_type)
      -> net::protocol::takion::PacketEnvelope;

  /// @brief The providers.
  Providers providers_;
};

}  // namespace libchiaking::play
