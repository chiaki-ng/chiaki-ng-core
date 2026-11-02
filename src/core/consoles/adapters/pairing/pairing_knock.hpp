/// @file
/// @brief Declares PairingKnock, the UDP knock that opens pairing at home:
/// a short datagram that asks the console to get ready for a registration
/// request, and the console's four-byte answer.
///
/// A PS4 is knocked with `SRC2` and answers `RES2`; a PS5 is knocked with
/// `SRC3` and answers `RES3`. The answer also tells HomePairing which address
/// the console is really at when the knock was broadcast.
#pragma once

#include "libchiaking/detail/consoles/firmware.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/net/protocol/remote_play/pairing/limits.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::consoles {

/// @brief One knock on a console's pairing port and the wait for its answer.
///
/// start() opens a UDP socket to the first address that can be opened and
/// sends the knock once; pump_once() then reads one datagram per call
/// without blocking until the console answers or the knock's deadline
/// passes. Answering, failing or cancelling closes the socket; the knock can
/// then be started again.
///
/// Not safe to share between threads: one thread drives it. The providers
/// must outlive it.
class PairingKnock {
 public:
  /// @brief Length of the knock datagram, in bytes: four letters and a zero.
  static constexpr std::size_t datagram_size = 5;
  /// @brief Length of the console's answer that is checked, in bytes.
  static constexpr std::size_t answer_size = 4;
  /// @brief Size of the buffer one received datagram is read into, in bytes;
  /// a larger datagram is a failure.
  static constexpr std::size_t answer_buffer_size =
      net::protocol::remote_play::search_response_buffer_size;

  /// @brief The bytes of a knock.
  using Datagram = std::array<std::byte, datagram_size>;

  /// @brief Why a knock failed.
  enum class Error : std::uint8_t {
    /// @brief start() while a knock is in progress, or pump_once() with
    /// none.
    invalid_state,
    /// @brief A zero port, a zero timeout or a generation with no knock.
    invalid_argument,
    /// @brief No candidate address could be opened, sending or receiving
    /// failed, a datagram was empty or oversized, or the socket did not
    /// close cleanly.
    network_failure,
    /// @brief The socket took only part of the knock.
    short_send,
    /// @brief No answer arrived before the deadline.
    timeout,
  };

  /// @brief Where and how to knock.
  struct Door {
    /// @brief The console generation, which picks the knock and the
    /// expected answer.
    Generation generation{Generation::ps5};
    /// @brief The first address to try; its port must not be zero. After
    /// start() it is the address that was opened.
    platform::SocketEndpoint destination{};
    /// @brief Further addresses of the same host, tried in order when the
    /// one before cannot be opened.
    std::vector<platform::SocketEndpoint> fallbacks{};
    /// @brief Whether the knock is broadcast. A broadcast accepts the
    /// answer from any address; otherwise only from the address knocked on.
    bool broadcast{};
    /// @brief How long to wait for the answer on the monotonic clock; must
    /// be positive.
    std::chrono::microseconds timeout{
        net::protocol::remote_play::phase_timeout};
  };

  /// @brief The knock for a console generation.
  ///
  /// @param generation The generation.
  /// @return `SRC2` for a PS4 and `SRC3` for a PS5, each followed by a zero
  ///     byte; nothing for a value outside the enumeration.
  [[nodiscard]] static auto datagram(Generation generation) noexcept
      -> std::optional<Datagram>;
  /// @brief Whether a datagram is the answer a console of a generation gives.
  ///
  /// @param generation The generation knocked.
  /// @param reply The datagram received.
  /// @return True when it begins with `RES2` (PS4) or `RES3` (PS5); trailing
  ///     bytes are ignored.
  static auto answers(Generation generation,
                      std::span<const std::byte> reply) noexcept -> bool;

  /// @brief Makes an idle knock.
  ///
  /// @param network Where the UDP socket comes from; must outlive this
  ///     object.
  /// @param clock The monotonic clock the deadline is measured by; must
  ///     outlive this object.
  PairingKnock(platform::NetworkProvider& network,
               const platform::ClockProvider& clock) noexcept;
  /// @brief Not copyable: it owns a socket.
  PairingKnock(const PairingKnock&) = delete;
  /// @brief Not copyable.
  auto operator=(const PairingKnock&) -> PairingKnock& = delete;
  /// @brief Closes the socket, if open.
  ~PairingKnock();

  /// @brief Opens a socket to the first address that can be opened and sends
  /// the knock, starting the deadline.
  ///
  /// A broadcast knock binds an unconnected socket (with SO_BROADCAST for
  /// IPv4) and sends to the address; otherwise the socket is connected to it.
  ///
  /// @param door Where and how to knock.
  /// @return Nothing once the knock is sent.
  /// @retval Error::invalid_state A knock is in progress.
  /// @retval Error::invalid_argument The port or timeout is zero, or the
  ///     generation has no knock.
  /// @retval Error::network_failure No address could be opened or the send
  ///     failed; the socket is closed.
  /// @retval Error::short_send The socket took only part of the knock; the
  ///     socket is closed.
  [[nodiscard]] auto start(Door door) -> Expected<void, Error>;
  /// @brief Reads at most one datagram, without blocking.
  ///
  /// A datagram that is not the expected answer, or comes from another
  /// address on a direct knock, is ignored.
  ///
  /// @return The address the console answered from, after which the socket is
  ///     closed; nothing while no answer has arrived and the deadline has not
  ///     passed.
  /// @retval Error::invalid_state No knock is in progress.
  /// @retval Error::timeout The deadline passed with no answer; the socket is
  ///     closed.
  /// @retval Error::network_failure Receiving failed, a datagram was empty or
  ///     oversized, or the socket did not close cleanly.
  [[nodiscard]] auto pump_once()
      -> Expected<std::optional<platform::SocketEndpoint>, Error>;
  /// @brief Closes the socket; does nothing when no knock is in progress.
  ///
  /// @return Nothing on success.
  /// @retval Error::network_failure The socket did not close cleanly; it is
  ///     dropped regardless.
  [[nodiscard]] auto cancel() -> Expected<void, Error>;
  /// @brief The socket to wait on for the answer.
  ///
  /// @return The socket, or null when no knock is in progress.
  auto pollable() const noexcept -> const platform::Pollable* {
    return socket_.get();
  }

 private:
  /// @brief The PS4 knock, `SRC2` and a zero byte.
  static constexpr Datagram ps4_datagram{std::byte{'S'}, std::byte{'R'},
                                         std::byte{'C'}, std::byte{'2'},
                                         std::byte{0}};
  /// @brief The PS5 knock, `SRC3` and a zero byte.
  static constexpr Datagram ps5_datagram{std::byte{'S'}, std::byte{'R'},
                                         std::byte{'C'}, std::byte{'3'},
                                         std::byte{0}};
  /// @brief A PS4's answer, `RES2`.
  static constexpr std::array<std::byte, answer_size> ps4_answer{
      std::byte{'R'}, std::byte{'E'}, std::byte{'S'}, std::byte{'2'}};
  /// @brief A PS5's answer, `RES3`.
  static constexpr std::array<std::byte, answer_size> ps5_answer{
      std::byte{'R'}, std::byte{'E'}, std::byte{'S'}, std::byte{'3'}};

  /// @brief Opens the UDP socket for one candidate address.
  ///
  /// @param destination The address.
  /// @param broadcast Whether to bind for broadcast instead of connecting.
  /// @return Nothing once the socket is ready.
  /// @retval Error::invalid_argument The port is zero.
  /// @retval Error::network_failure The socket could not be created, set up,
  ///     bound or connected; it is closed.
  [[nodiscard]] auto open(const platform::SocketEndpoint& destination,
                          bool broadcast) -> Expected<void, Error>;
  /// @brief Closes and drops the socket.
  ///
  /// @return Nothing when there was no socket or it closed.
  /// @retval Error::network_failure The socket did not close cleanly.
  [[nodiscard]] auto close() -> Expected<void, Error>;

  /// @brief Where the UDP socket comes from.
  platform::NetworkProvider& network_;
  /// @brief The monotonic clock the deadline is measured by.
  const platform::ClockProvider& clock_;
  /// @brief The knock's socket; null when no knock is in progress.
  std::unique_ptr<platform::UdpSocket> socket_;
  /// @brief The knock in progress, its destination being the address opened.
  Door door_{};
  /// @brief When the knock gives up, on the monotonic clock.
  timing::Deadline deadline_;
};

}  // namespace libchiaking::consoles
