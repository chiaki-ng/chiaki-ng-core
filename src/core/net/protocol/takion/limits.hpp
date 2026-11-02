/// @file
/// @brief Declares the Takion client's default limits, the fixed reliable
/// channel numbers, and the wire constants shared by its codecs.
#pragma once

#include <cstddef>
#include <cstdint>

namespace libchiaking::net::protocol::takion {

/// @brief Commands a client queues before post() refuses with mailbox_full.
inline constexpr std::size_t default_mailbox_capacity = 64;
/// @brief Queued commands one pump executes at most.
inline constexpr std::size_t default_commands_per_pump = 64;
/// @brief Datagrams one pump reads from the socket at most; when the budget is
/// spent the pump returns early and leaves negotiation checks to the next one.
inline constexpr std::size_t default_datagrams_per_pump = 64;
/// @brief Largest reliable control message, in bytes (1 MiB), before the
/// protobuf codec's own limit is applied.
inline constexpr std::size_t default_maximum_message_size = 1024 * 1024;
/// @brief Largest Takion datagram the client sends by default, in bytes,
/// including any envelope; reliable fragments, probes and stream packets must
/// fit it.
inline constexpr std::size_t default_fragment_size = 1200;
/// @brief Most fragments one reliable message may be split into.
inline constexpr std::size_t default_fragment_count = 1024;
/// @brief Unacknowledged reliable fragments the sender keeps for resending
/// (1024): enough for every fragment within the console's receive window, so
/// a burst of control messages during loss is resent rather than sent once
/// untracked.
inline constexpr std::size_t default_retransmission_window = 1024;
/// @brief Out-of-order reliable fragments the receiver holds while waiting
/// for a gap to fill (1024), so a long burst from the console is held rather
/// than dropped and resent.
inline constexpr std::size_t default_reorder_size = 1024;
/// @brief Initial reliable retransmission timeout, in microseconds (300 ms),
/// before round-trip samples refine it.
inline constexpr std::uint64_t default_retransmission_timeout_us = 300'000;
/// @brief Shortest reliable retransmission timeout, in microseconds (200 ms).
inline constexpr std::uint64_t default_minimum_retransmission_timeout_us =
    200'000;
/// @brief Longest reliable retransmission timeout, in microseconds (2 s).
inline constexpr std::uint64_t default_maximum_retransmission_timeout_us =
    2'000'000;
/// @brief Retransmissions of one reliable fragment before it is given up.
inline constexpr std::uint32_t default_maximum_retries = 24;

/// @brief Reliable channel carrying protobuf control messages (BIG, BANG,
/// heartbeats, stream info and the rest).
inline constexpr std::uint16_t control_channel_id = 1;
/// @brief Reliable channel carrying CORRUPTFRAME and IDRREQUEST messages.
inline constexpr std::uint16_t feedback_channel_id = 2;
/// @brief Reliable channel carrying Senkusha commands.
inline constexpr std::uint16_t senkusha_control_channel_id = 8;
/// @brief Reliable channel carrying STREAMINFOACK.
inline constexpr std::uint16_t stream_info_ack_channel_id = 9;

/// @brief Half of the 32-bit serial number space; a 32-bit counter that moved
/// by less than this is taken to have moved forward (RFC 1982 arithmetic),
/// for reliable sequence numbers and key positions.
inline constexpr std::uint32_t serial_half_range = 0x80000000U;

/// @brief Bit 0x10 of a packet's type byte: on video and audio packets it
/// announces the NALU-info descriptor at the end of the AV header (3 bytes
/// for video and for version 7 audio; one extra byte for audio from
/// version 15).
inline constexpr std::uint8_t nalu_info_structs_flag = 0x10;
/// @brief Units an AV frame can hold at most: 2048, the range of the 11-bit
/// unit fields of the AV header.
inline constexpr std::uint16_t max_units_in_frame = 0x800;
/// @brief Highest unit index, 2047.
inline constexpr std::uint16_t max_unit_index = max_units_in_frame - 1;

/// @brief Bytes of the packet type at the front of every Takion packet.
inline constexpr std::size_t packet_type_size = 1;
/// @brief Offset of the chunk type byte in a control packet: the type byte,
/// then the 12-byte message header (verification tag, MAC, key position).
inline constexpr std::size_t control_chunk_type_offset = 13;
/// @brief Deepest nesting of protobuf messages the decoders accept.
inline constexpr unsigned max_protobuf_depth = 8;

}  // namespace libchiaking::net::protocol::takion
