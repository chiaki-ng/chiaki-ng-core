/// @file
/// @brief Declares ActiveTakion, the Takion client started on a calibrated
/// route for the stream proper.
#pragma once

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/protocol/remote_play/stream/route.hpp"
#include "src/core/net/protocol/takion/client.hpp"

#include <cstdint>
#include <memory>

namespace libchiaking::net::protocol::remote_play {

/// @brief The bytes per datagram assumed for IP and UDP headers when sizing
/// Takion fragments.
inline constexpr std::uint32_t default_active_network_overhead = 50;

/// @brief Why the stream's Takion client could not start.
enum class ActiveTakionError : std::uint8_t {
  /// @brief The route has no transport.
  invalid_route,
  /// @brief The calibration's Takion version and wire profile disagree.
  version_mismatch,
  /// @brief The Takion version is not implemented.
  unsupported_version,
  /// @brief The measured MTU is zero or too small for a control packet with a
  /// cookie after the overhead.
  invalid_mtu,
  /// @brief The client failed to start.
  start_failed,
};

/// @brief The bytes of each datagram that Takion fragments may not use.
///
/// Plain data.
struct ActiveDatagramBudget {
  /// @brief IP and UDP header bytes.
  std::uint32_t network_overhead{default_active_network_overhead};
  /// @brief Extra bytes reserved for an outer envelope, such as a cloud
  /// relay's.
  std::uint32_t reserved_envelope_bytes{};
};

/// @brief The Takion client of a stream, with the route it runs on.
///
/// Neither copyable nor movable; owned through a unique_ptr. The client runs
/// on the stream's worker thread.
class ActiveTakion {
 public:
  /// @brief Starts a Takion client on a calibrated route.
  ///
  /// The fragment size is the smaller of the measured MTUs less the budget
  /// (plus four bytes for a PSN envelope). Cryptography is required, with plain
  /// control messages allowed until the keys are installed. The version is
  /// fixed to the calibrated one unless the config negotiates.
  ///
  /// @param route The calibrated route; its transport is taken over.
  /// @param clock The monotonic clock; must outlive the client.
  /// @param threading Provider of the client's mutex; must outlive the
  ///     client.
  /// @param random Randomness for the handshake; must outlive the client.
  /// @param events Where the client reports; must outlive the client.
  /// @param config The client configuration to start from.
  /// @param budget The bytes to leave out of each datagram.
  /// @return The started client.
  /// @retval ActiveTakionError Why it could not start.
  [[nodiscard]] static auto start(EstablishedRoute route,
                                  platform::ClockProvider& clock,
                                  platform::ThreadingProvider& threading,
                                  platform::SecureRandomProvider& random,
                                  takion::EventSink& events,
                                  takion::ClientConfig config = {},
                                  ActiveDatagramBudget budget = {})
      -> Expected<std::unique_ptr<ActiveTakion>, ActiveTakionError>;

  ActiveTakion(const ActiveTakion&) = delete;
  ActiveTakion& operator=(const ActiveTakion&) = delete;
  ActiveTakion(ActiveTakion&&) = delete;
  ActiveTakion& operator=(ActiveTakion&&) = delete;

  /// @brief The client.
  ///
  /// @return The client.
  auto client() noexcept -> takion::Client& { return *client_; }
  /// @brief The client.
  ///
  /// @return The client.
  auto client() const noexcept -> const takion::Client& { return *client_; }
  /// @brief The route, without its transport, which the client owns.
  ///
  /// @return The route.
  auto route() const noexcept -> const EstablishedRoute& { return route_; }

 private:
  /// @brief Takes the started client and its route.
  ///
  /// @param client The client.
  /// @param route The route.
  ActiveTakion(std::unique_ptr<takion::Client> client,
               EstablishedRoute route) noexcept;

  /// @brief The client.
  std::unique_ptr<takion::Client> client_;
  /// @brief The route it was started on.
  EstablishedRoute route_;
};

}  // namespace libchiaking::net::protocol::remote_play
