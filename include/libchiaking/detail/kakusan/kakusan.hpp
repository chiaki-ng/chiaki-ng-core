/// @file
/// @brief Declares kakusan::Kakusan, the entry point for hosting and
/// joining shared sessions.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/kakusan/device_identity.hpp"
#include "libchiaking/detail/kakusan/device_store.hpp"
#include "libchiaking/detail/kakusan/guest_visit.hpp"
#include "libchiaking/detail/kakusan/invitation.hpp"
#include "libchiaking/detail/kakusan/remembered_devices.hpp"
#include "libchiaking/detail/kakusan/shared_session.hpp"
#include "libchiaking/detail/kakusan/values.hpp"
#include "libchiaking/detail/play/session_share.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace libchiaking {
class ChiakiNg;
}  // namespace libchiaking

namespace libchiaking::testing {
class ServiceBuilder;
}  // namespace libchiaking::testing

namespace libchiaking::platform {
class PlatformFactory;
}

namespace libchiaking::kakusan {

class Surroundings;

/// @brief The Kakusan context: where a host starts sharing a session and
/// where a guest reads and accepts an invitation.
///
/// Kakusan lets the player of a running play::Session (the host) invite
/// other people, each on their own device, to watch the stream and take a
/// controller seat. Host and guest devices connect directly to each other. They
/// find each other by exchanging two short texts out of band, an Invitation
/// from the host and a Reply from the guest, and then confirm each other by
/// reading four-digit codes aloud.
///
/// On the host: share() gives a SharedSession. On the guest: read() gives an
/// Invitation to show, join() gives a GuestVisit to run. Both sides use
/// this_device() and remembered() for identity.
///
/// Kakusan is a cheap handle; copies refer to the same context. A frontend
/// gets it from ChiakiNg::kakusan.
///
/// Every member may be called from any thread. use_stun_servers() and
/// use_port_guessing() take a short lock of their own; read() and
/// recognition take the RememberedDevices lock.
/// @ingroup api_kakusan
class Kakusan {
 public:
  /// @brief Why Kakusan cannot be offered on this device.
  enum class Unavailable : std::uint8_t {
    /// @brief The platform has no secure datagram provider.
    no_secure_transport,
    /// @brief The crypto provider is absent or lacks a primitive shared
    /// play needs, or the device identity could not be restored or created.
    no_identity_crypto,
    /// @brief No DeviceStore was supplied, or it failed while the identity
    /// or the remembered devices were loaded or saved.
    device_store_unavailable,
    /// @brief The STUN server list has too many entries, or an entry is
    /// empty or too long.
    invalid_stun_servers,
    /// @brief This build of the library leaves Kakusan out, as
    /// `LIBCHIAKING_KAKUSAN=OFF` does; only ChiakiNg::kakusan and
    /// ChiakiNg::use_stun_servers answer with it.
    not_built,
  };

  /// @brief When this device offers a peer guessed public ports on top of
  /// the ones the STUN servers reported.
  enum class PortGuessing : std::uint8_t {
    /// @brief Guess only when the STUN servers report different public
    /// ports for the same local port.
    automatic,
    /// @brief Guess whenever a STUN server reports a public port.
    forced,
  };

  /// @brief Most STUN servers that can be configured.
  static constexpr std::size_t most_stun_servers = 16;
  /// @brief Longest STUN server name, in characters.
  static constexpr std::size_t longest_stun_server = 255;

  /// @brief The upper-case name of an Unavailable value, for logs.
  /// @param reason The value to name.
  /// @return A string with static lifetime, such as "NO_SECURE_TRANSPORT".
  static constexpr auto name(Unavailable reason) noexcept -> std::string_view {
    switch (reason) {
      case Unavailable::no_secure_transport:
        return "NO_SECURE_TRANSPORT";
      case Unavailable::no_identity_crypto:
        return "NO_IDENTITY_CRYPTO";
      case Unavailable::device_store_unavailable:
        return "DEVICE_STORE_UNAVAILABLE";
      case Unavailable::not_built:
        return "NOT_BUILT";
      case Unavailable::invalid_stun_servers:
        break;
    }
    return "INVALID_STUN_SERVERS";
  }

  /// @brief Replaces the STUN servers.
  ///
  /// Applies to sessions shared and visits joined afterwards; ones already
  /// running keep the servers they started with.
  ///
  /// @param stun_servers The new list, replacing the old one. May be empty.
  /// @return Nothing on success.
  /// @retval Unavailable::invalid_stun_servers The list breaks
  ///     most_stun_servers or longest_stun_server, or has an empty entry; the
  ///     old list is kept.
  [[nodiscard]] auto use_stun_servers(std::span<const std::string> stun_servers)
      -> Expected<void, Unavailable>;
  /// @brief Chooses when guessed public ports are offered to a peer.
  ///
  /// Guessed ports let a peer reach this device through a router that gives
  /// each destination its own public port. Applies to sessions shared and
  /// visits joined afterwards; ones already running keep the choice they
  /// started with.
  ///
  /// @param guessing The new choice. PortGuessing::automatic until called.
  void use_port_guessing(PortGuessing guessing);

  /// @brief This device's Kakusan identity.
  /// @return A reference valid while any copy of this Kakusan lives. Show
  ///     its fingerprint so another person can confirm this device.
  auto this_device() const noexcept -> const DeviceIdentity&;
  /// @brief The devices the user has remembered.
  /// @return A reference valid while any copy of this Kakusan lives.
  auto remembered() noexcept -> RememberedDevices&;

  /// @brief Starts sharing a running session with remote guests.
  ///
  /// The session keeps playing for the host as before. Sharing begins with no
  /// guests; call SharedSession::invite_player for each one. Destroying the
  /// returned SharedSession stops sharing.
  ///
  /// @param session The sharing side of the play::Session to share. It must
  ///     outlive the returned SharedSession.
  /// @param rules How many seats there are, how many the host keeps, and
  ///     which buttons guests may press.
  /// @param observer Receives the shared session's events. It must outlive
  ///     the returned SharedSession.
  /// @param pacing Timing and limits; the defaults suit a live stream.
  /// @return The shared session.
  /// @retval Refusal::Kind::cannot_be_shared The session is a cloud session,
  ///     or its stream protocol version cannot be forwarded.
  /// @retval Refusal::Kind::busy The session is already being shared.
  /// @retval Refusal::Kind::not_sharing The session has already ended.
  /// @retval Refusal::Kind::internal The sharing machinery could not be set
  ///     up.
  [[nodiscard]] auto share(play::SessionShare& session,
                           const SharingRules& rules,
                           SharedSessionObserver& observer,
                           SharedSession::Pacing pacing = {})
      -> Expected<std::unique_ptr<SharedSession>, Refusal>;

  /// @brief Reads an invitation text a guest received, without contacting the
  /// host.
  ///
  /// Use the result to show who is inviting and how many seats there are
  /// before the guest commits.
  ///
  /// @param pasted The text exactly as the host produced it.
  /// @return The invitation, with InvitationSummary::recognition filled in
  ///     from the remembered devices.
  /// @retval Refusal::Kind::invalid_argument The text is empty.
  /// @retval Refusal::Kind::invalid_text The text is longer than
  ///     Invitation::most_text or is not a valid invitation.
  /// @retval Refusal::Kind::expired The invitation's expiry time has passed.
  /// @retval Refusal::Kind::unsupported_version The invitation is for a
  ///     protocol version this library does not speak.
  [[nodiscard]] auto read(std::string_view pasted) const
      -> Expected<Invitation, Refusal>;
  /// @brief Accepts an invitation and prepares to visit the host.
  ///
  /// Nothing reaches the host yet. Call GuestVisit::run on the result; it
  /// produces the Reply to send back and carries the visit through.
  ///
  /// @param invitation The invitation from read().
  /// @param ports The frontend's observer, media sink and optional pause.
  ///     They must outlive the visit.
  /// @param pacing Timing of the visit's loop; the defaults suit a live
  ///     stream.
  /// @return The visit, not yet running.
  /// @retval Refusal::Kind::expired The invitation expired since it was read.
  /// @retval Refusal::Kind::invalid_text The invitation is not valid.
  /// @retval Refusal::Kind::internal The visit's machinery could not be set
  ///     up.
  /// @note Other kinds can come back when the network connection for the
  ///     visit could not be prepared.
  [[nodiscard]] auto join(Invitation invitation, GuestVisit::Ports ports,
                          GuestVisit::Pacing pacing = {})
      -> Expected<std::unique_ptr<GuestVisit>, Refusal>;

 private:
  friend class libchiaking::ChiakiNg;
  friend class libchiaking::testing::ServiceBuilder;

  /// @brief Builds the Kakusan context for this device.
  ///
  /// Checks that the platform can do what Kakusan needs, loads this
  /// device's identity from the store (creating and saving one the first
  /// time) and loads the remembered devices. Blocks while the store does
  /// that; the checks are made in the order the failures are listed.
  ///
  /// @param platform Source of the network, secure datagram, crypto, clock,
  ///     random and diagnostics providers. It must outlive the context and
  ///     everything created from it.
  /// @param store Persistence for the identity and remembered devices. It
  ///     must outlive the context and everything created from it.
  /// @param stun_servers Servers used to discover this device's public
  ///     address. May be empty.
  /// @return The context.
  /// @retval Unavailable::no_secure_transport The platform does not supply
  ///     PlatformCapability::secure_datagram.
  /// @retval Unavailable::no_identity_crypto The crypto provider is missing
  ///     or incomplete, or the identity could not be restored or created.
  /// @retval Unavailable::invalid_stun_servers The list breaks
  ///     most_stun_servers or longest_stun_server, or has an empty entry.
  /// @retval Unavailable::device_store_unavailable The store failed.
  [[nodiscard]] static auto create(
      platform::PlatformFactory& platform, DeviceStore& store,
      std::span<const std::string> stun_servers = {})
      -> Expected<Kakusan, Unavailable>;
  /// @brief Wraps an already assembled environment. Used by the library and
  /// its tests.
  /// @param surroundings The environment to share.
  /// @return A handle on it.
  static auto on(std::shared_ptr<Surroundings> surroundings) noexcept
      -> Kakusan;

  explicit Kakusan(std::shared_ptr<Surroundings> surroundings) noexcept;

  std::shared_ptr<Surroundings> surroundings_;
};

}  // namespace libchiaking::kakusan
