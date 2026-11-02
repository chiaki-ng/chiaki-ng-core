/// @file
/// @brief Declares ReliableChannel, the SCTP-like reliable, ordered
/// message layer that Takion's control messages travel on inside DATA
/// chunks.
///
/// A message is split into fragments, one per DATA chunk. Each fragment's
/// body starts with an 8-byte header (all big-endian):
///
/// | Offset | Size | Field |
/// |-------:|-----:|-------|
/// | 0 | 4 | sequence number (TSN), one per fragment |
/// | 4 | 2 | channel |
/// | 6 | 2 | reserved, zero |
/// | 8 | 1 | data type (MessageDataType), first fragment only |
/// | 9 | n | message bytes |
///
/// The chunk's 0x01 flag marks a message's last fragment. The receiver
/// acknowledges with DATA_ACK: the highest in-order sequence, its free
/// window, and gap blocks of out-of-order sequences relative to that
/// cumulative point. Unacknowledged fragments are resent on a timeout derived
/// from measured round trips.
#pragma once

#include "libchiaking/detail/secret_string.hpp"
#include "src/core/net/protocol/takion/detail/handshake_types.hpp"
#include "src/core/net/protocol/takion/detail/packet_types.hpp"
#include "src/core/net/protocol/takion/limits.hpp"
#include "src/core/net/protocol/takion/reliable_retry.hpp"
#include "src/core/net/protocol/takion/result.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace libchiaking::net::protocol::takion {

/// @brief Bytes before a first fragment's message data: the 8-byte body header
/// and the data type byte.
inline constexpr std::size_t first_fragment_overhead = 9;
/// @brief Bytes before a continuation fragment's data: the 8-byte body header.
inline constexpr std::size_t continuation_fragment_overhead = 8;
/// @brief Bytes of the fragment body header.
inline constexpr std::size_t data_chunk_body_header_size = 8;
/// @brief The DATA chunk flag marking a message's final fragment.
inline constexpr std::uint8_t data_final_fragment_flag = 0x01;
/// @brief How long, in microseconds, the receiver waits for a missing sequence
/// before skipping it while the retransmission timeout is at most 1 s
/// (2.5 s).
inline constexpr std::uint64_t missing_sequence_expiry_short_us = 2'500'000;
/// @brief How long, in microseconds, the receiver waits for a missing sequence
/// when the retransmission timeout is above 1 s (5 s).
inline constexpr std::uint64_t missing_sequence_expiry_long_us = 5'000'000;
/// @brief Retransmission timeout, in microseconds, above which the long wait
/// for missing sequences applies (1 s).
inline constexpr std::uint64_t missing_sequence_expiry_rto_threshold_us =
    1'000'000;
/// @brief Datagram size, in bytes, a DATA_ACK must fit: the IPv4 minimum, 576.
inline constexpr std::size_t reference_datagram_limit = 576;
/// @brief Bytes of a DATA_ACK datagram other than its gap blocks: IPv4 and UDP
/// headers, packet type, message and chunk headers and the fixed fields.
inline constexpr std::size_t selective_acknowledgement_datagram_overhead = 29;
/// @brief Bytes of one gap block: start and end offsets, u16 each.
inline constexpr std::size_t gap_block_size = 4;
/// @brief Gap blocks one DATA_ACK carries at most: (576 - 29) / 4 = 136.
inline constexpr std::size_t maximum_gap_blocks =
    (reference_datagram_limit - selective_acknowledgement_datagram_overhead) /
    gap_block_size;
/// @brief Highest gap block offset accepted in a received DATA_ACK.
inline constexpr std::uint16_t maximum_gap_offset = 32767;
/// @brief Longest wait, in microseconds, between two retransmission passes
/// (300 ms).
inline constexpr std::uint64_t scheduler_wait_cap_us = 300'000;

/// @brief Which header a fragment's body has.
enum class FragmentLayout : std::uint8_t {
  /// @brief The message's first fragment, with the data type byte.
  first,
  /// @brief A later fragment, without it.
  continuation,
};

/// @brief Whether a message may be split.
enum class MessageFragmentation : std::uint8_t {
  /// @brief The message goes in one fragment however large; used for every
  /// control message but the BIG.
  single_datagram,
  /// @brief The message is split to the maximum fragment size; used for the
  /// BIG,
  /// which can exceed one datagram.
  split_to_fragment_size,
};

/// @brief The sizes, timers and starting points of one reliable channel.
struct ReliableChannelConfig {
  /// @brief Our first sequence number.
  std::uint32_t initial_local_sequence{};
  /// @brief The console's first sequence number.
  std::uint32_t initial_remote_sequence{};
  /// @brief Largest message, in bytes, sent or reassembled.
  std::size_t maximum_message_size{};
  /// @brief Largest fragment body, in bytes; at least 9.
  std::size_t maximum_fragment_size{};
  /// @brief Most fragments per message.
  std::size_t maximum_fragment_count{};
  /// @brief Fragments kept for retransmission; further ones are sent once,
  /// untracked.
  std::size_t retransmission_window_size{};
  /// @brief Sequences beyond the cumulative point the receiver will hold.
  std::size_t receive_reorder_size{};
  /// @brief Initial retransmission timeout, in microseconds.
  std::uint64_t retransmission_timeout_us{};
  /// @brief Retransmissions before a fragment is given up.
  std::uint32_t maximum_retries{};
  /// @brief Lower retransmission timeout clamp, in microseconds.
  std::uint64_t minimum_retransmission_timeout_us{
      default_minimum_retransmission_timeout_us};
  /// @brief Upper retransmission timeout clamp, in microseconds.
  std::uint64_t maximum_retransmission_timeout_us{
      default_maximum_retransmission_timeout_us};
  /// @brief The console's window, in bytes, from its INIT_ACK.
  std::uint32_t initial_peer_receive_window{default_advertised_receive_window};
  /// @brief The window, in bytes, we advertise before subtracting what is held.
  std::uint32_t local_receive_window{default_advertised_receive_window};
};

/// @brief Why the reliable channel refused something or noticed an anomaly.
enum class ReliableChannelError : std::uint8_t {
  /// @brief A size, window or timeout is zero, out of range, or inverted.
  invalid_configuration,
  /// @brief A message to send, or one being reassembled, exceeds the maximum.
  message_too_large,
  /// @brief A message needs, or arrived in, more fragments than allowed.
  fragment_limit_exceeded,
  /// @brief Reserved; not reported today.
  retransmission_window_full,
  /// @brief A fragment is shorter than its header, or a completed message is
  /// empty.
  truncated_fragment,
  /// @brief Reserved; not reported today.
  fragment_too_large,
  /// @brief Reserved; not reported today.
  wrong_channel,
  /// @brief Reserved; not reported today.
  nonzero_reserved_field,
  /// @brief A fragment was already received; it is acknowledged again.
  duplicate,
  /// @brief A fragment lies beyond the receive reorder window and was dropped.
  receive_window_exceeded,
  /// @brief Reserved; not reported today.
  unexpected_continuation,
  /// @brief Reserved; not reported today.
  incomplete_message_conflict,
  /// @brief A missing sequence was waited for too long and skipped; partial
  /// messages were discarded.
  reassembly_expired,
  /// @brief Reserved; not reported today.
  selective_acknowledgement_unsupported,
  /// @brief A DATA_ACK's gap blocks are malformed, or it acknowledges a
  /// sequence
  /// never sent.
  invalid_acknowledgement,
};

/// @brief One fragment ready to be put in a DATA chunk.
///
/// Copies duplicate the wiping buffers explicitly.
struct OutboundFragment {
  /// @brief Its sequence number.
  std::uint32_t sequence{};
  /// @brief Its channel.
  std::uint16_t channel{};
  /// @brief Which header its body has.
  FragmentLayout layout{FragmentLayout::first};
  /// @brief Whether it is the message's last fragment.
  bool end_of_message{};
  /// @brief The message's data type.
  MessageDataType data_type{};
  /// @brief The fragment body: header and data.
  SecretBytes payload;
  /// @brief The protected datagram as first sent, recorded so retransmissions
  /// resend the same bytes at the same key position; empty until then.
  SecretBytes datagram;

  /// @brief Makes an empty fragment.
  OutboundFragment() = default;
  /// @brief Copies a fragment, cloning its buffers.
  ///
  /// @param other The fragment.
  OutboundFragment(const OutboundFragment& other);
  /// @brief Moves a fragment.
  ///
  /// @param other The fragment.
  OutboundFragment(OutboundFragment&& other) noexcept = default;
  /// @brief Copy-assigns, cloning the buffers.
  ///
  /// @param other The fragment.
  /// @return This fragment.
  OutboundFragment& operator=(const OutboundFragment& other);
  /// @brief Move-assigns.
  ///
  /// @param other The fragment.
  /// @return This fragment.
  OutboundFragment& operator=(OutboundFragment&& other) noexcept;
};

/// @brief The fragments a send produced, in order.
struct SentFragments {
  /// @brief Fragments to transmit now: the tracked ones allowed by the peer's
  /// window, then any untracked ones.
  std::vector<OutboundFragment> fragments;
  /// @brief Fragments sent once without retransmission because the window was
  /// full.
  std::size_t untracked_fragments{};
};

/// @brief A reassembled message.
struct ReceivedMessage {
  /// @brief The channel it arrived on.
  std::uint16_t channel{};
  /// @brief Its data type.
  MessageDataType data_type{};
  /// @brief Its bytes, after the data type byte.
  std::vector<std::byte> payload;
};

/// @brief A run of sequences received beyond the cumulative point, as offsets
/// from it.
struct GapAcknowledgementBlock {
  /// @brief First offset in the run; at least 1.
  std::uint16_t start{};
  /// @brief Last offset in the run.
  std::uint16_t end{};

  /// @brief Compares both offsets.
  ///
  /// @return Whether both blocks are the same.
  bool operator==(const GapAcknowledgementBlock&) const = default;
};

/// @brief A DATA_ACK's content.
struct DataAcknowledgement {
  /// @brief Highest sequence received with nothing missing before it.
  std::uint32_t cumulative_sequence{};
  /// @brief Free receive buffer, in bytes.
  std::uint32_t advertised_receive_window{};
  /// @brief Runs received beyond the cumulative point, ascending.
  std::vector<GapAcknowledgementBlock> gap_blocks;
  /// @brief Sequences received twice; never filled in by this channel.
  std::vector<std::uint32_t> duplicate_sequences;
};

/// @brief What receiving one fragment led to.
struct ReceiveOutcome {
  /// @brief Messages it completed, in order.
  std::vector<ReceivedMessage> messages;
  /// @brief The DATA_ACK to send, if any.
  std::optional<DataAcknowledgement> acknowledgement;
  /// @brief Anything odd about the fragment.
  std::optional<ReliableChannelError> anomaly;
};

/// @brief One of our fragments the console acknowledged.
struct AcknowledgedFragment {
  /// @brief Its sequence number.
  std::uint32_t sequence{};
  /// @brief Time since its first transmission, in microseconds.
  std::uint64_t round_trip_time_us{};
  /// @brief The operation id of its message, if the sender gave one.
  std::optional<std::uint64_t> operation_id{};
  /// @brief Whether this was the last unacknowledged fragment of that
  /// operation.
  bool operation_complete{};
};

/// @brief What a DATA_ACK led to.
struct AcknowledgementOutcome {
  /// @brief Fragments newly acknowledged.
  std::vector<AcknowledgedFragment> acknowledged;
  /// @brief Fragments the gaps show as missing, to resend now.
  std::vector<OutboundFragment> retransmissions;
  /// @brief Fragments given up on.
  std::vector<std::uint32_t> exhausted_sequences;
};

/// @brief What a retransmission pass led to.
struct RetransmissionPollResult {
  /// @brief Fragments to resend now.
  std::vector<OutboundFragment> retransmissions;
  /// @brief Fragments given up on.
  std::vector<std::uint32_t> exhausted_sequences;
};

/// @brief Both directions of Takion's reliable message channel for one
/// association.
///
/// Sending: messages become fragments with consecutive sequence numbers; up to
/// `retransmission_window_size` are tracked and sent while the console's
/// window allows, the window shrinking by each fragment's data until the next
/// DATA_ACK restores it. Receiving: fragments are ordered by sequence within
/// `receive_reorder_size`, delivered per channel as messages complete, and
/// acknowledged with each arrival. Sequence comparisons use 32-bit serial
/// arithmetic.
///
/// A plain object owned by the Takion client and used under its lock. It does
/// no I/O; the client sends what it returns.
class ReliableChannel {
 public:
  /// @brief Makes a channel after checking its configuration.
  ///
  /// @param config The sizes, timers and starting sequences.
  /// @return The channel.
  /// @retval ReliableChannelError::invalid_configuration A size or timeout is
  ///     zero, the fragment size is below 9, a window reaches 2^31, or the
  ///     timeout clamps are inverted.
  [[nodiscard]] static auto create(ReliableChannelConfig config)
      -> Expected<ReliableChannel, ReliableChannelError>;

  /// @brief Reads the configuration.
  ///
  /// @return The configuration.
  const ReliableChannelConfig& config() const noexcept { return config_; }
  /// @brief Reads the sequence the next fragment will get.
  ///
  /// @return The sequence.
  std::uint32_t next_local_sequence() const noexcept {
    return next_local_sequence_;
  }
  /// @brief Reads the sequence expected next from the console.
  ///
  /// @return The cumulative point plus one.
  std::uint32_t next_remote_sequence() const noexcept {
    return cumulative_ + 1U;
  }
  /// @brief Reads the highest in-order sequence received.
  ///
  /// @return The sequence.
  std::uint32_t cumulative_acknowledgement() const noexcept {
    return cumulative_;
  }
  /// @brief Counts tracked fragments not yet acknowledged.
  ///
  /// @return The count.
  std::size_t outstanding_fragment_count() const noexcept;
  /// @brief Counts tracked fragments, acknowledged by gap or not.
  ///
  /// @return The count.
  std::size_t queued_fragment_count() const noexcept { return pending_.size(); }
  /// @brief Tells whether a message is partly reassembled on any channel.
  ///
  /// @return Whether one is.
  bool has_incomplete_message() const noexcept;
  /// @brief Reads the current retransmission timeout.
  ///
  /// @return Microseconds.
  std::uint64_t retransmission_timeout_us() const noexcept {
    return round_trip_.timeout_us();
  }
  /// @brief Reads the round-trip estimator.
  ///
  /// @return The estimator.
  const RoundTripEstimator& round_trip() const noexcept { return round_trip_; }
  /// @brief Reads the percentiles of the latest round-trip samples.
  ///
  /// @param now_us The current time, in microseconds.
  /// @return The percentiles and the recent largest sample.
  RoundTripSpread round_trip_spread(std::uint64_t now_us) const noexcept {
    return round_trip_samples_.spread(now_us);
  }
  /// @brief Reads how many bytes the console can still take.
  ///
  /// @return Bytes.
  std::uint32_t peer_receive_window() const noexcept { return peer_window_; }
  /// @brief Reads the highest sequence transmitted.
  ///
  /// @return The sequence.
  std::uint32_t highest_sent_sequence() const noexcept {
    return sent_sequence_;
  }
  /// @brief Reads when poll() next has work to do.
  ///
  /// @return Monotonic microseconds; nothing when nothing is outstanding.
  std::optional<std::uint64_t> next_poll_us() const noexcept {
    return next_pass_us_;
  }

  /// @brief Fragments a message and tracks its fragments for retransmission.
  ///
  /// @param channel The channel to send on.
  /// @param data_type The message's data type.
  /// @param message The message.
  /// @param now_us The current time, in monotonic microseconds.
  /// @param operation_id An id reported back with each acknowledgement, so the
  ///     caller learns when the whole message was received.
  /// @param fragmentation Whether the message may be split.
  /// @return The fragments to transmit now.
  /// @retval ReliableChannelError::message_too_large It exceeds the maximum.
  /// @retval ReliableChannelError::fragment_limit_exceeded It needs too many
  ///     fragments.
  [[nodiscard]] auto send(
      std::uint16_t channel, MessageDataType data_type,
      std::span<const std::byte> message, std::uint64_t now_us,
      std::optional<std::uint64_t> operation_id = std::nullopt,
      MessageFragmentation fragmentation =
          MessageFragmentation::single_datagram)
      -> Expected<SentFragments, ReliableChannelError>;

  /// @brief Takes one received DATA chunk body.
  ///
  /// A missing sequence is waited for 2.5 s (5 s when the timeout exceeds 1 s)
  /// after the first later fragment arrived; then it is skipped and partial
  /// messages are dropped. Fragments beyond the reorder window are dropped,
  /// but once they have kept arriving for that same wait, as after an outage
  /// in which the console gave up on more fragments than the window holds,
  /// the cumulative point moves up to just before the one that arrived, so
  /// the channel does not refuse every later fragment for good.
  ///
  /// @param chunk_body The body, starting with its 8-byte header.
  /// @param final_fragment Whether the chunk's final-fragment flag was set.
  /// @param now_us The current time, in monotonic microseconds.
  /// @return Completed messages, the DATA_ACK to send, and any anomaly.
  ReceiveOutcome receive(std::span<const std::byte> chunk_body,
                         bool final_fragment, std::uint64_t now_us);

  /// @brief Builds the DATA_ACK for what has been received.
  ///
  /// @return The cumulative point, free window and up to 136 gap blocks.
  DataAcknowledgement selective_acknowledgement() const;

  /// @brief Takes a DATA_ACK from the console.
  ///
  /// Fragments at or below its cumulative point, and inside its gap blocks, are
  /// acknowledged and the round-trip estimate is updated from those sent only
  /// once. Fragments the gaps skip are resent at once if they were last sent
  /// before the highest acknowledged one.
  ///
  /// @param acknowledgement The DATA_ACK.
  /// @param now_us The current time, in monotonic microseconds.
  /// @return The acknowledged, resent and given-up fragments.
  /// @retval ReliableChannelError::invalid_acknowledgement A gap block is
  /// empty,
  ///     reversed, overlapping, beyond offset 32767, or the acknowledgement
  ///     covers a sequence never sent.
  [[nodiscard]] auto acknowledge(const DataAcknowledgement& acknowledgement,
                                 std::uint64_t now_us)
      -> Expected<AcknowledgementOutcome, ReliableChannelError>;

  /// @brief Resends fragments whose timeout has passed.
  ///
  /// When the console's window is closed, only the oldest fragment is resent
  /// as a probe once nothing is waiting on a timer.
  ///
  /// @param now_us The current time, in monotonic microseconds.
  /// @return The fragments to resend and those given up on.
  RetransmissionPollResult poll(std::uint64_t now_us);

  /// @brief Records the protected datagram a fragment went out in, for exact
  /// retransmission.
  ///
  /// @param sequence The fragment's sequence.
  /// @param datagram The bytes sent.
  void record_datagram(std::uint32_t sequence,
                       std::span<const std::byte> datagram);

 private:
  /// @brief Stores a checked configuration and sets the starting points.
  ///
  /// @param config The configuration.
  explicit ReliableChannel(ReliableChannelConfig config);

  /// @brief A tracked fragment and its retransmission state.
  struct PendingFragment {
    /// @brief The fragment.
    OutboundFragment fragment;
    /// @brief Data bytes, which the peer's window is charged on each send.
    std::uint32_t payload_size{};
    /// @brief When it was first transmitted, in monotonic microseconds.
    std::uint64_t first_send_us{};
    /// @brief When its retransmission timer last started.
    std::uint64_t timer_start_us{};
    /// @brief Transmissions so far; 0 while waiting for window.
    std::uint32_t transmission_count{};
    /// @brief Age fed to the retry decision, in milliseconds; never advanced,
    /// so it stays 0.
    std::uint64_t accumulated_ms{};
    /// @brief The highest sequence sent before this fragment's latest
    /// transmission.
    std::uint32_t previous_sent_sequence{};
    /// @brief Whether a gap block acknowledged it.
    bool acknowledged{};
    /// @brief Whether its acknowledgement was reported.
    bool reported{};
    /// @brief The operation id of its message.
    std::optional<std::uint64_t> operation_id{};
  };

  /// @brief A received fragment waiting for its turn.
  struct InboundFragment {
    /// @brief Its sequence.
    std::uint32_t sequence{};
    /// @brief Its channel.
    std::uint16_t channel{};
    /// @brief Whether it ends a message.
    bool final_fragment{};
    /// @brief Its bytes after the 8-byte header.
    std::vector<std::byte> data;
    /// @brief When it arrived, in monotonic microseconds.
    std::uint64_t arrival_us{};
  };

  /// @brief A message being reassembled on one channel.
  struct Reassembly {
    /// @brief The bytes so far, data type byte first.
    std::vector<std::byte> data;
    /// @brief Fragments so far.
    std::size_t fragment_count{};
    /// @brief Whether it outgrew the limits and is being discarded.
    bool overflowed{};
  };

  /// @brief Appends an in-order fragment to its channel's message, completing
  /// the message on a final fragment.
  ///
  /// @param fragment The fragment.
  /// @return The message a final fragment completed, or nothing.
  /// @retval ReliableChannelError::fragment_limit_exceeded The message has too
  ///     many fragments and is being discarded.
  /// @retval ReliableChannelError::message_too_large The message outgrew the
  ///     size limit and is being discarded.
  /// @retval ReliableChannelError::truncated_fragment A final fragment
  ///     completed an empty message.
  [[nodiscard]] auto consume_fragment(InboundFragment fragment)
      -> Expected<std::optional<ReceivedMessage>, ReliableChannelError>;
  /// @brief Checks a configuration.
  ///
  /// @param config The configuration.
  /// @return Whether create() would accept it.
  static bool is_valid(const ReliableChannelConfig& config) noexcept;
  /// @brief Picks how long to wait for a missing sequence.
  ///
  /// @return 2.5 s or 5 s, in microseconds.
  std::uint64_t missing_sequence_expiry_us() const noexcept;
  /// @brief Sums the bytes held for reordering and reassembly, counting 8 bytes
  /// of header per fragment.
  ///
  /// @return Bytes.
  std::size_t held_receive_bytes() const noexcept;
  /// @brief Compares two sequence numbers in serial arithmetic.
  ///
  /// @param lhs One sequence.
  /// @param rhs The other.
  /// @return Negative when `lhs` precedes `rhs`, zero when equal, positive when
  ///     it follows.
  static constexpr std::int32_t serial_difference(std::uint32_t lhs,
                                                  std::uint32_t rhs) noexcept {
    return static_cast<std::int32_t>(lhs - rhs);
  }
  /// @brief Transmits a tracked fragment again, or gives it up when its budget
  /// is spent.
  ///
  /// @param index Its position among the tracked fragments.
  /// @param now_us The current time, in monotonic microseconds.
  /// @param transmissions Receives it when sent.
  /// @param exhausted Receives its sequence when given up.
  /// @return Whether it was given up and removed.
  bool attempt(std::size_t index, std::uint64_t now_us,
               std::vector<OutboundFragment>& transmissions,
               std::vector<std::uint32_t>& exhausted);
  /// @brief Sends every tracked fragment not yet transmitted, while the peer's
  /// window allows.
  ///
  /// @param now_us The current time, in monotonic microseconds.
  /// @param transmissions Receives them.
  void transmit_unsent(std::uint64_t now_us,
                       std::vector<OutboundFragment>& transmissions);
  /// @brief Brings the next retransmission pass forward to a deadline.
  ///
  /// @param deadline_us The deadline, in monotonic microseconds.
  void schedule_pass_by(std::uint64_t deadline_us) noexcept;

  /// @brief The configuration.
  ReliableChannelConfig config_;
  /// @brief Sequence of the next fragment to send.
  std::uint32_t next_local_sequence_{};
  /// @brief Highest in-order sequence received.
  std::uint32_t cumulative_{};
  /// @brief Tracked fragments, oldest first.
  std::deque<PendingFragment> pending_;
  /// @brief When the next retransmission pass is due.
  std::optional<std::uint64_t> next_pass_us_;
  /// @brief Bytes the console can still take.
  std::uint32_t peer_window_{};
  /// @brief Highest sequence transmitted.
  std::uint32_t sent_sequence_{};
  /// @brief The round-trip estimator.
  RoundTripEstimator round_trip_;
  /// @brief The latest round-trip samples, for their percentiles.
  RoundTripSamples round_trip_samples_;
  /// @brief Received fragments beyond the cumulative point, by sequence.
  std::deque<InboundFragment> receive_queue_;
  /// @brief When fragments beyond the reorder window started arriving with
  /// none inside it, in monotonic microseconds.
  std::optional<std::uint64_t> beyond_window_since_us_;
  /// @brief Partial messages by channel.
  std::map<std::uint16_t, Reassembly> reassemblies_;

 private:
};

}  // namespace libchiaking::net::protocol::takion
