/// @file
/// @brief Declares Microphone::Core, which queues the player's microphone
/// connect, mute and audio frames for the session thread to send to the
/// console.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/play/microphone.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/play/adapters/session/command_route.hpp"
#include "src/core/play/adapters/session/rest_request.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string_view>

namespace libchiaking::play {

/// @brief The implementation behind the public Microphone: connect tickets,
/// the mute state and a bounded queue of encoded frames.
///
/// The frontend calls from its own threads; requests are held and the session
/// thread sends them with Microphone::Core::flush while streaming. Only a
/// console session has a microphone, and nothing is accepted while a rest
/// request is under way. The frame queue holds at most the configured depth;
/// the oldest frame is dropped when it is full. Every member locks one mutex.
/// Owned by ConsoleCommands; the route and rest request must outlive it.
class Microphone::Core {
 public:
  /// @brief How many recent connect tickets are remembered for
  /// Microphone::Core::state.
  static constexpr std::size_t remembered_tickets = 16;

  /// @brief Queues a request to connect the microphone to the console and wakes
  /// the session thread.
  ///
  /// @param tag A caller's value carried in the ticket.
  /// @return The ticket; InputRefusal::not_running or InputRefusal::wrong_route
  ///     from the route; InputRefusal::not_ready while a rest request is under
  ///     way.
  auto connect(std::uint32_t tag = 0) -> Expected<ConnectTicket, InputRefusal>;
  /// @brief Reads the latest connect ticket.
  ///
  /// @return The ticket; empty before the first connect.
  auto latest() const -> std::optional<ConnectTicket>;
  /// @brief Reads where a connect request stands.
  ///
  /// @param ticket A ticket from Microphone::Core::connect.
  /// @return ConnectState::accepted or ConnectState::rejected once sent;
  ///     ConnectState::pending while queued and open; ConnectState::rejected
  ///     for a ticket not among the remembered ones or when the microphone
  ///     closed.
  auto state(ConnectTicket ticket) const -> ConnectState;
  /// @brief Reports whether the microphone may be used now.
  ///
  /// @return `true` for a running console session with no rest request.
  auto open() const noexcept -> bool;
  /// @brief Queues muting or unmuting and wakes the session thread.
  ///
  /// Unmuting starts accepting frames; muting drops the queued ones.
  ///
  /// @param muted Whether to mute.
  /// @return Nothing when queued; the route's refusal; InputRefusal::not_ready
  ///     while a rest request is under way.
  auto set_muted(bool muted) -> Expected<void, InputRefusal>;
  /// @brief Queues one encoded frame and wakes the session thread.
  ///
  /// @param frame One Opus frame of Microphone::frame_size bytes.
  /// @return Nothing when queued, possibly after dropping the oldest frame; the
  ///     route's refusal; InputRefusal::not_ready while muted or a rest request
  ///     is under way.
  auto push(std::span<const std::byte, frame_size> frame)
      -> Expected<void, InputRefusal>;
  /// @brief Reads the running totals.
  ///
  /// @return A copy of the totals.
  auto statistics() const -> Statistics;
  /// @brief Reports whether something waits to be sent, so the session loop
  /// does not sleep.
  ///
  /// @return `true` while a connect, mute change or frame is queued.
  auto pending() const -> bool;

 private:
  friend class ConsoleCommands;

  /// @brief One encoded microphone frame.
  using Frame = std::array<std::byte, frame_size>;
  /// @enum Mute
  /// @brief The mute change waiting to be sent.
  ///
  /// @var Mute idle
  /// @brief None.
  ///
  /// @var Mute on
  /// @brief Mute.
  ///
  /// @var Mute off
  /// @brief Unmute.
  enum class Mute : std::uint8_t { idle, on, off };

  /// @brief Builds the microphone, closed and muted.
  ///
  /// @param threading Provider of the microphone's mutex; must outlive the
  ///     microphone.
  /// @param route Where commands go; must outlive the microphone.
  /// @param rest The session's rest request; must outlive the microphone.
  /// @param depth The most frames queued; at least 1.
  Core(platform::ThreadingProvider& threading, const CommandRoute& route,
       const RestRequest& rest, std::size_t depth);

  /// @brief Sends the queued connect, mute change and up to the queue depth of
  /// frames; called on the session thread while streaming.
  void flush();
  /// @brief Counts and logs a failed send, on powers of two.
  ///
  /// @param step The step named in the log.
  /// @param result The link's result; nothing is done on success.
  void report(
      std::string_view step,
      const Expected<void, net::protocol::remote_play::StreamError>& result);

  /// @brief Where commands go.
  const CommandRoute& route_;
  /// @brief The session's rest request.
  const RestRequest& rest_;
  /// @brief The most frames queued.
  std::size_t depth_;
  /// @brief Guards the fields below.
  mutable platform::Mutex mutex_;
  /// @brief The id of the last ticket issued.
  std::uint32_t next_ticket_{};
  /// @brief The latest ticket.
  std::optional<ConnectTicket> latest_;
  /// @brief The most recent Microphone::Core::remembered_tickets tickets.
  std::deque<ConnectTicket> issued_;
  /// @brief The id of the connect waiting to be sent.
  std::optional<std::uint32_t> connect_pending_;
  /// @brief The id of the last connect sent.
  std::optional<std::uint32_t> connect_done_;
  /// @brief Whether the last connect sent succeeded.
  bool connect_accepted_{};
  /// @brief Whether frames are accepted; set by unmuting.
  bool sending_{};
  /// @brief The mute change waiting to be sent.
  Mute mute_{Mute::idle};
  /// @brief Frames waiting to be sent, oldest first.
  std::deque<Frame> frames_;
  /// @brief Frames queued, sent and dropped, and send failures.
  Statistics statistics_{};
};

}  // namespace libchiaking::play
