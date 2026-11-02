/// @file
/// @brief Declares SpokeTransport, the network path between a Kakusan host and
/// one guest, its events and its factory.
#pragma once

#include <libchiaking/detail/secret_string.hpp>

#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/net/kakusan/short_block.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace libchiaking::net::kakusan {

/// @brief The ALPN both peers require.
inline constexpr std::string_view application_protocol = "rcp/1/draft-05";
/// @brief Time the host has to nominate a pair.
using nat::ice::connectivity_timeout_us;
/// @brief Longest candidate gathering.
using nat::ice::gathering_timeout_us;
/// @brief How often reflexive mappings are refreshed.
using nat::ice::mapping_refresh_us;
/// @brief Which side a spoke's transport serves.
using TransportRole = nat::ice::Policy;

/// @brief What a peer publishes about its transport: ICE parameters and its
/// DTLS certificate digest.
struct TransportDescription {
  /// @brief The ICE parameters.
  IceParameters ice;
  /// @brief SHA-256 of the DTLS certificate.
  Digest256 certificate_digest{};

  /// @brief Copies the description, secrets into new storage.
  ///
  /// @return The copy.
  auto clone() const -> TransportDescription {
    return TransportDescription{ice.clone(), certificate_digest};
  }
};

/// @brief What a SpokeTransport reports from pump().
namespace transport_event {
/// @brief Gathering finished; this is what to publish.
struct Gathered {
  /// @brief The local description.
  TransportDescription local;
};
/// @brief ICE nominated a pair.
struct Nominated {};
/// @brief The DTLS handshake finished.
struct Established {};
/// @brief An application record arrived over DTLS.
struct Record {
  /// @brief The decrypted record.
  SecretBytes bytes;
};
/// @brief A plain datagram (RCPM media) arrived on the nominated pair.
struct Datagram {
  /// @brief The datagram.
  std::vector<std::byte> bytes;
};
/// @brief The transport failed and stops.
struct Failed {
  /// @brief Why.
  Error error{Error::no_path};
};
}  // namespace transport_event

/// @brief Any transport event.
using TransportEvent =
    std::variant<transport_event::Gathered, transport_event::Nominated,
                 transport_event::Established, transport_event::Record,
                 transport_event::Datagram, transport_event::Failed>;

/// @brief One spoke's network path: ICE to find it, DTLS to secure it, and the
/// nominated pair for plain media datagrams.
///
/// Sans-I/O from the caller's view: pump() runs it and reports events. Not
/// copyable; one thread drives it.
class SpokeTransport {
 public:
  /// @brief Releases the transport.
  virtual ~SpokeTransport() = default;
  /// @brief Makes a transport.
  SpokeTransport() = default;
  /// @brief Not copyable.
  SpokeTransport(const SpokeTransport&) = delete;
  /// @brief Not copyable.
  ///
  /// @return Never returns.
  SpokeTransport& operator=(const SpokeTransport&) = delete;

  /// @brief Starts gathering candidates.
  ///
  /// @param now_us The current monotonic time.
  /// @param credentials ICE credentials derived from this side's block seed;
  ///     nothing draws fresh ones.
  /// @return Nothing on success.
  /// @retval Error Gathering could not start.
  [[nodiscard]] virtual auto gather(std::uint64_t now_us,
                                    std::optional<IceCredentials> credentials)
      -> Result<void> = 0;
  /// @brief Starts connectivity checks to the peer and pins its certificate.
  ///
  /// @param remote The peer's description.
  /// @param now_us The current monotonic time.
  /// @return Nothing on success.
  /// @retval Error The description is invalid or connecting cannot start.
  [[nodiscard]] virtual auto connect(const TransportDescription& remote,
                                     std::uint64_t now_us) -> Result<void> = 0;
  /// @brief Receives, runs timers, sends, and reports what happened.
  ///
  /// @param now_us The current monotonic time.
  /// @param events Receives the events.
  /// @return Nothing on success.
  /// @retval Error The transport failed.
  [[nodiscard]] virtual auto pump(std::uint64_t now_us,
                                  std::vector<TransportEvent>& events)
      -> Result<void> = 0;
  /// @brief When pump() must next run.
  ///
  /// @return The monotonic instant; nothing when nothing is scheduled.
  virtual auto next_deadline_us() const noexcept
      -> std::optional<std::uint64_t> = 0;
  /// @brief Sends an application record over DTLS.
  ///
  /// @param record The record.
  /// @return Nothing on success.
  /// @retval Error The association cannot send.
  [[nodiscard]] virtual auto send_record(std::span<const std::byte> record)
      -> Result<void> = 0;
  /// @brief Sends a plain datagram on the nominated pair.
  ///
  /// @param datagram The datagram.
  /// @return Nothing on success.
  /// @retval Error No pair, no consent, or the send failed.
  [[nodiscard]] virtual auto send_datagram(std::span<const std::byte> datagram)
      -> Result<void> = 0;
  /// @brief Exports keying material from the DTLS association.
  ///
  /// @param label The exporter label.
  /// @param context The context value.
  /// @param output Receives the material.
  /// @return Nothing on success.
  /// @retval Error The association cannot export.
  [[nodiscard]] virtual auto export_secret(std::string_view label,
                                           std::span<const std::byte> context,
                                           std::span<std::byte> output)
      -> Result<void> = 0;
  /// @brief Closes the transport and its sockets.
  virtual void close() noexcept = 0;
};

/// @brief Makes spoke transports, one per invitation or reply.
class SpokeTransportFactory {
 public:
  /// @brief Releases the factory.
  virtual ~SpokeTransportFactory() = default;
  /// @brief Makes a transport.
  ///
  /// @param role The side it serves.
  /// @return The transport.
  /// @retval Error It could not be made.
  [[nodiscard]] virtual auto create(TransportRole role)
      -> Result<std::unique_ptr<SpokeTransport>> = 0;
};

}  // namespace libchiaking::net::kakusan
