/// @file
/// @brief Declares ControlChannel, Kakusan's reliable, rate-limited control
/// message channel.
#pragma once

#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/utils/timing/token_bucket.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::net::kakusan {

/// @brief First retransmission interval, in microseconds: 250 ms.
inline constexpr std::uint64_t control_retransmit_initial_us = 250'000;
/// @brief Largest retransmission interval, in microseconds: 2 seconds.
inline constexpr std::uint64_t control_retransmit_max_us = 2'000'000;
/// @brief How long a message may go unacknowledged after its first send, or a
/// reassembly stay incomplete, in microseconds: 15 seconds.
inline constexpr std::uint64_t control_failure_deadline_us = 15'000'000;
/// @brief Most messages queued or in flight.
inline constexpr std::size_t control_queue_capacity = 40;
/// @brief New messages sent per second.
inline constexpr std::uint64_t control_send_rate = 20;
/// @brief Burst of new messages sent.
inline constexpr std::uint64_t control_send_burst = 40;
/// @brief RCC1 bytes sent per second, headers included.
inline constexpr std::uint64_t control_send_byte_rate = 16384;
/// @brief Burst of RCC1 bytes sent.
inline constexpr std::uint64_t control_send_byte_burst = 16384;
/// @brief New messages accepted per second.
inline constexpr std::uint64_t control_receive_rate = 20;
/// @brief Burst of new messages accepted: 100 ms of slack over the sender's.
inline constexpr std::uint64_t control_receive_burst = 42;
/// @brief RCC1 bytes accepted per second.
inline constexpr std::uint64_t control_receive_byte_rate = 16384;
/// @brief Burst of RCC1 bytes accepted.
inline constexpr std::uint64_t control_receive_byte_burst = 18023;

/// @brief Names a queued message so its acknowledgement can be reported.
struct ControlTicket {
  /// @brief Unique per channel, from 1; 0 for a terminal message.
  std::uint64_t value{};

  /// @brief Compares values.
  ///
  /// @return True when equal.
  constexpr bool operator==(const ControlTicket&) const noexcept = default;
};

/// @brief The reliable control channel over DTLS records (RCP TRANSPORT section
/// 1.1): one message in flight per direction, fragments resent every 250 ms
/// doubling to 2 seconds until acknowledged, failure 15 seconds after the first
/// send, PING and SOURCE_IDR coalesced while queued, and token buckets on both
/// sending and receiving.
///
/// Sans-I/O: messages are enqueued, records come out of poll() and go in
/// through receive(). Message bodies are held in wiped storage. Not
/// thread-safe.
class ControlChannel {
 public:
  /// @brief Opens the channel with full token buckets.
  ///
  /// @param now_us The current monotonic time.
  /// @param first_message_id The first id sent; 0 means 1.
  explicit ControlChannel(std::uint64_t now_us,
                          std::uint32_t first_message_id = 1);

  /// @brief Queues a message; a PING or SOURCE_IDR replaces one still queued.
  ///
  /// @param message The message.
  /// @param now_us The current monotonic time.
  /// @return The message's ticket, or the coalesced one's.
  /// @retval Error::protocol The channel is closing, the message is invalid
  ///     or
  ///     too large, 40 are outstanding, or ids would run out.
  [[nodiscard]] auto enqueue(const control::Message& message,
                             std::uint64_t now_us) -> Result<ControlTicket>;
  /// @brief Replaces everything queued with a terminal message and closes the
  /// queue; a message in flight still finishes first.
  ///
  /// @param message The message.
  /// @param now_us The current monotonic time.
  /// @return False when already closing, ids ran out, or the message is
  ///     invalid.
  [[nodiscard]] auto send_terminal(const control::Message& message,
                                   std::uint64_t now_us) -> bool;
  /// @brief Emits due ACKs, starts the next message, and sends or resends
  /// fragments, as the byte bucket allows.
  ///
  /// @param now_us The current monotonic time.
  /// @param records Receives the RCC1 records to send.
  /// @return Nothing on success.
  /// @retval Error::timeout A message went unacknowledged, or a reassembly
  ///     stayed incomplete, for 15 seconds.
  /// @retval Error::protocol The next message could not be fragmented.
  [[nodiscard]] auto poll(std::uint64_t now_us,
                          std::vector<std::vector<std::byte>>& records)
      -> Result<void>;
  /// @brief Feeds a received RCC1 record.
  ///
  /// @param record The record.
  /// @param now_us The current monotonic time.
  /// @return The message it completed; nothing for an ACK, a fragment or a
  ///     duplicate.
  /// @retval Error::protocol The record breaks the rules: rate exceeded,
  ///     malformed, a future id, conflicting fragments, an ACK for an unsent
  ///     id,
  ///     or a message that is not valid JSON or not a known control.
  /// @retval Error::timeout A reassembly stayed incomplete for 15 seconds.
  /// @retval Error::codec A SOURCE_SETUP names an unsupported codec.
  [[nodiscard]] auto receive(std::span<const std::byte> record,
                             std::uint64_t now_us)
      -> Result<std::optional<control::Message>>;
  /// @brief Takes the next acknowledged ticket.
  ///
  /// @return The ticket; nothing when none is waiting. Terminal messages give
  ///     none.
  [[nodiscard]] auto take_acknowledged() -> std::optional<ControlTicket>;
  /// @brief When poll() must next run.
  ///
  /// @return The earliest of the failure deadlines, the retransmission, and
  ///     the
  ///     buckets' readiness; nothing when idle.
  auto next_deadline_us() const noexcept -> std::optional<std::uint64_t>;
  /// @brief Whether nothing is queued, in flight or waiting to be
  /// acknowledged.
  ///
  /// @return True when idle.
  auto idle() const noexcept -> bool;

 private:
  /// @brief Which queued messages coalesce.
  enum class Kind : std::uint8_t {
    /// @brief Never coalesced.
    ordinary,
    /// @brief PING: a newer one replaces a queued one.
    ping,
    /// @brief SOURCE_IDR: a newer one replaces a queued one.
    source_idr,
  };

  /// @brief A queued message.
  struct Pending {
    /// @brief Its ticket.
    ControlTicket ticket{};
    /// @brief Whether it coalesces.
    Kind kind{Kind::ordinary};
    /// @brief Whether it is the terminal message.
    bool terminal{};
    /// @brief The encoded JSON.
    SecretBytes body;
  };

  /// @brief The message being sent.
  struct InFlight {
    /// @brief Its id.
    std::uint32_t id{};
    /// @brief Its ticket.
    ControlTicket ticket{};
    /// @brief Whether it is terminal.
    bool terminal{};
    /// @brief Its RCC1 records.
    std::vector<std::vector<std::byte>> fragments;
    /// @brief The next fragment to send in this round.
    std::size_t next_fragment{};
    /// @brief Whether every fragment was sent at least once.
    bool delivered_once{};
    /// @brief Whether any fragment was sent.
    bool first_sent{};
    /// @brief When the first fragment was first sent; starts the 15-second
    /// deadline.
    std::uint64_t first_sent_us{};
    /// @brief When the fragments are resent.
    std::uint64_t retransmit_at_us{};
    /// @brief The next retransmission interval.
    std::uint64_t interval_us{control_retransmit_initial_us};
  };

  /// @brief The incoming message being reassembled.
  struct Reassembly {
    /// @brief Fragments expected, fixed by the first received.
    std::uint16_t count{};
    /// @brief When its first fragment arrived.
    std::uint64_t started_us{};
    /// @brief The fragments received, by index.
    std::vector<std::optional<SecretBytes>> parts;
    /// @brief How many distinct fragments arrived.
    std::size_t received{};
  };

  /// @brief The last completed incoming message, kept to recognise
  /// retransmissions.
  struct Completed {
    /// @brief Its fragment count.
    std::uint16_t count{};
    /// @brief Its bytes.
    SecretBytes bytes;
  };

  /// @brief Largest message id.
  static constexpr std::uint64_t max_message_id =
      std::numeric_limits<std::uint32_t>::max();

  /// @brief Whether another message id remains.
  ///
  /// @return True below 2^32.
  auto next_id_available() const noexcept -> bool;
  /// @brief Messages queued or in flight.
  ///
  /// @return The count.
  auto outstanding_logical() const noexcept -> std::size_t;
  /// @brief Moves the first queued message in flight.
  ///
  /// @return Nothing on success.
  /// @retval Error::protocol No id remains or it cannot be fragmented.
  [[nodiscard]] auto begin_message() -> Result<void>;
  /// @brief Sends the in-flight fragments the byte bucket allows, and
  /// schedules
  /// the retransmission once all are out.
  ///
  /// @param now_us The current monotonic time.
  /// @param records Receives the records.
  void pump_fragments(std::uint64_t now_us,
                      std::vector<std::vector<std::byte>>& records);
  /// @brief Handles an ACK.
  ///
  /// @param id The acknowledged id.
  /// @return Nothing; a stale ACK is ignored.
  /// @retval Error::protocol It names an id never sent or not fully sent.
  [[nodiscard]] auto receive_ack(std::uint32_t id) -> Result<void>;
  /// @brief Handles a data fragment.
  ///
  /// @param id The message id.
  /// @param index The fragment index.
  /// @param count The fragment count.
  /// @param payload The fragment.
  /// @param now_us The current monotonic time.
  /// @return The completed message; nothing otherwise.
  /// @retval Error The fragment breaks the rules.
  [[nodiscard]] auto receive_data(std::uint32_t id, std::uint16_t index,
                                  std::uint16_t count,
                                  std::span<const std::byte> payload,
                                  std::uint64_t now_us)
      -> Result<std::optional<control::Message>>;
  /// @brief Checks a retransmitted fragment of the last completed message and
  /// re-acknowledges it.
  ///
  /// @param index The fragment index.
  /// @param count The fragment count.
  /// @param payload The fragment.
  /// @return Nothing when it matches.
  /// @retval Error::protocol It conflicts.
  [[nodiscard]] auto receive_duplicate(std::uint16_t index, std::uint16_t count,
                                       std::span<const std::byte> payload)
      -> Result<void>;
  /// @brief Joins the fragments, checks the JSON, acknowledges and decodes.
  ///
  /// @param id The message id.
  /// @return The message.
  /// @retval Error The bytes are not a valid control message.
  [[nodiscard]] auto complete_message(std::uint32_t id)
      -> Result<std::optional<control::Message>>;
  /// @brief Whether the reassembly passed its deadline.
  ///
  /// @param now_us The current monotonic time.
  /// @return True after 15 seconds.
  auto receive_expired(std::uint64_t now_us) const noexcept -> bool;
  /// @brief Queues an ACK, once.
  ///
  /// @param id The message id.
  void queue_ack(std::uint32_t id);
  /// @brief Advances the clock and refills every bucket.
  ///
  /// @param now_us The current monotonic time; never moves backwards.
  void refill(std::uint64_t now_us) noexcept;

  /// @brief New messages sent: 20/s, burst 40.
  timing::TokenBucket send_logical_;
  /// @brief RCC1 bytes sent: 16384/s, burst 16384.
  timing::TokenBucket send_bytes_;
  /// @brief New messages accepted: 20/s, burst 42.
  timing::TokenBucket receive_logical_;
  /// @brief RCC1 bytes accepted: 16384/s, burst 18023.
  timing::TokenBucket receive_bytes_;
  /// @brief The latest time seen.
  std::uint64_t last_us_;
  /// @brief The id of the next message sent.
  std::uint64_t next_message_id_;
  /// @brief The next ticket.
  std::uint64_t next_ticket_{1};
  /// @brief Messages waiting to be sent.
  std::deque<Pending> queue_;
  /// @brief The message being sent.
  std::optional<InFlight> in_flight_;
  /// @brief Tickets acknowledged and not yet taken.
  std::deque<ControlTicket> acknowledged_;
  /// @brief ACKs to send.
  std::vector<std::uint32_t> pending_acks_;
  /// @brief The next incoming message id.
  std::uint64_t expected_id_{1};
  /// @brief The incoming message being reassembled.
  std::optional<Reassembly> reassembly_;
  /// @brief The last completed incoming message.
  std::optional<Completed> completed_;
  /// @brief Whether a terminal message was queued.
  bool closing_{};
};

}  // namespace libchiaking::net::kakusan
