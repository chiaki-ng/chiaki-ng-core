/// @file
/// @brief Declares ClientInbound, the Takion client's receive path: it checks
/// and opens each datagram, routes it by family, and turns it into events,
/// acknowledgements and replies.
#pragma once

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "src/core/net/protocol/takion/client/version_negotiation.hpp"
#include "src/core/net/protocol/takion/client_types.hpp"
#include "src/core/net/protocol/takion/control_chunk_codec.hpp"
#include "src/core/net/protocol/takion/control_codec.hpp"
#include "src/core/net/protocol/takion/detail/crypto_context.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/detail/typed_message_codec.hpp"
#include "src/core/net/protocol/takion/extension_metrics.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/net/protocol/takion/received_tos_metrics.hpp"
#include "src/core/net/protocol/takion/reliable_channel.hpp"
#include "src/core/net/protocol/takion/result.hpp"
#include "src/core/net/protocol/takion/video_frame_metrics.hpp"
#include "src/core/net/transport/datagram/transport.hpp"

#include <cstdint>
#include <optional>
#include <source_location>
#include <span>
#include <vector>

namespace libchiaking::net::protocol::takion::client_detail {

/// @brief What the receive path needs from the client that owns it: handshake
/// handling, sends, and failure.
///
/// Client implements it privately; every call comes from within
/// ClientInbound::process_datagram() under the client's owner lock.
class ClientInboundHost {
 public:
  /// @brief Releases the host.
  virtual ~ClientInboundHost() = default;
  /// @brief Handles an INIT_ACK.
  ///
  /// @param chunk The chunk.
  /// @return Nothing.
  /// @retval ClientError A fatal handshake or send failure.
  virtual ClientResult<void> inbound_handle_init_ack(
      const InitAcknowledgementChunk& chunk) = 0;
  /// @brief Handles a COOKIE_ACK.
  ///
  /// @param chunk The chunk.
  /// @return Nothing.
  /// @retval ClientError A fatal handshake or send failure.
  virtual ClientResult<void> inbound_handle_cookie_ack(
      const CookieAcknowledgementChunk& chunk) = 0;
  /// @brief Sends a DATA_ACK.
  ///
  /// @param acknowledgement What to acknowledge.
  /// @return Nothing.
  /// @retval ClientError A fatal send failure.
  virtual ClientResult<void> inbound_send_ack(
      const DataAcknowledgement& acknowledgement) = 0;
  /// @brief Sends a control message, such as the answer to a protocol request.
  ///
  /// @param message The message.
  /// @return Nothing.
  /// @retval ClientError A fatal send failure.
  virtual ClientResult<void> inbound_send_control(
      const ControlMessage& message) = 0;
  /// @brief Sends an extended message, such as an RTT pong.
  ///
  /// @return Nothing by default.
  /// @retval ClientError A send failure.
  virtual ClientResult<void> inbound_send_extended_message(
      std::span<const std::byte>) {
    return {};
  }
  /// @brief Takes a GENERICDATA control message.
  ///
  /// @return Nothing by default.
  /// @retval ClientError A fatal failure.
  virtual ClientResult<void> inbound_generic_data_control(
      const GenericDataControl&) {
    return {};
  }
  /// @brief Takes a decrypted generic-data chunk.
  ///
  /// @return Nothing by default.
  /// @retval ClientError A fatal failure.
  virtual ClientResult<void> inbound_generic_data_chunk(GenericDataChunk) {
    return {};
  }
  /// @brief Resends a fragment a DATA_ACK showed missing.
  ///
  /// @return Nothing by default.
  /// @retval ClientError A fatal send failure.
  virtual ClientResult<void> inbound_send_fragment(const OutboundFragment&) {
    return {};
  }
  /// @brief Fails the client for good.
  ///
  /// @param error Why.
  virtual void inbound_fail_terminal(ClientError error) = 0;
  /// @brief Tells whether the client is connected.
  ///
  /// @return Whether it is.
  virtual bool inbound_connected() const = 0;
};

/// @brief Which received packets are processed.
enum class InboundScope : std::uint8_t {
  /// @brief Every packet.
  all,
  /// @brief Control packets only; the rest are dropped without decoding, as
  /// while a DISCONNECT drains.
  control,
};

/// @brief The session state one datagram is processed against.
struct InboundSession {
  /// @brief The active wire profile; required.
  const WireProfile* profile{};
  /// @brief The association, once up.
  const EstablishedHandshake* established{};
  /// @brief The reliable channel, once up.
  ReliableChannel* reliable{};
  /// @brief Whether keys are installed.
  bool crypto_installed{};
  /// @brief Which packets are processed.
  InboundScope scope{InboundScope::all};
};

/// @brief Processes every datagram a Takion client receives.
///
/// With crypto required, AV datagrams that arrive before the keys are held
/// (up to 32) and replayed once they are installed; other protected families
/// are dropped then, except handshake chunks and, when allowed, plain control.
/// Every authenticated AV unit is shown to the source tap before decryption.
/// Video and audio are delivered as EncodedVideo and EncodedAudio in the
/// version 7 layout and as decoded AvUnitReceived from version 9. Reliable
/// messages are decoded by data type; RTT pings are answered with pongs.
/// Failures that leave the session usable become PacketRejected events.
///
/// Owned by Client and used under its owner lock; holds references to the
/// host, clock, crypto context, event sink, control codec and configuration,
/// which outlive it.
class ClientInbound {
 public:
  /// @brief Makes the receive path.
  ///
  /// @param host The owning client.
  /// @param clock The monotonic clock.
  /// @param crypto The session's crypto context.
  /// @param events Receives the events.
  /// @param control_codec Decodes control messages; replaced on commit.
  /// @param config The client's configuration.
  ClientInbound(ClientInboundHost& host, platform::ClockProvider& clock,
                CryptoContext& crypto, EventSink& events,
                ControlCodec& control_codec, const ClientConfig& config)
      : host_(host),
        clock_(clock),
        crypto_(crypto),
        events_(events),
        control_codec_(control_codec),
        config_(config),
        negotiation_(config) {}

  /// @brief Records that the version offer was sent.
  ///
  /// @param now_us When, in monotonic microseconds.
  void note_version_request_sent(std::uint64_t now_us) {
    negotiation_.offer_sent(now_us);
  }

  /// @brief Checks the negotiation timeout.
  ///
  /// @param now_us The current time, in monotonic microseconds.
  void expire_negotiation(std::uint64_t now_us) {
    negotiation_.check_wait(now_us);
  }

  /// @brief AV datagrams held at most while waiting for keys.
  static constexpr std::size_t postponed_capacity = 32;
  /// @brief Processes one received datagram.
  ///
  /// Decodes it in place and copies only what an event or the postponed queue
  /// keeps; nothing refers to it once this returns.
  ///
  /// @param datagram The datagram as read, writable for the duration of the
  ///     call.
  /// @param session The session state.
  void process_datagram(net::transport::datagram::Datagram datagram,
                        const InboundSession& session);
  /// @brief Replays the AV datagrams held while waiting for keys; their arrival
  /// times are no longer meaningful.
  ///
  /// @param session The session state, keys installed.
  void replay_postponed(const InboundSession& session);
  /// @brief Counts AV datagrams held while waiting for keys.
  ///
  /// @return The count.
  std::size_t postponed_count() const noexcept { return postponed_.size(); }
  /// @brief Asks whether a negotiated version can be committed now.
  ///
  /// @param reliable The reliable channel, or null.
  /// @return The version, once the channel is quiet; nothing otherwise.
  std::optional<std::uint32_t> take_ready_commit(
      const ReliableChannel* reliable);
  /// @brief Reaches the video frame metrics.
  ///
  /// @return The collector.
  VideoFrameMetricsCollector& video_metrics() noexcept {
    return video_metrics_;
  }
  /// @brief Reaches the traffic-class counters.
  ///
  /// @return The counters.
  ReceivedTosMetrics& tos_metrics() noexcept { return tos_metrics_; }
  /// @brief Reads the video frame metrics.
  ///
  /// @return The collector.
  const VideoFrameMetricsCollector& video_metrics() const noexcept {
    return video_metrics_;
  }
  /// @brief Reads the traffic-class counters.
  ///
  /// @return The counters.
  const ReceivedTosMetrics& tos_metrics() const noexcept {
    return tos_metrics_;
  }
  /// @brief Reads the extended-header metrics.
  ///
  /// @return The metrics.
  const ExtensionReceiveMetrics& extension_metrics() const noexcept {
    return extension_metrics_;
  }

 private:
  /// @brief Handles a control packet's chunk.
  ///
  /// @param payload The bytes after the type byte.
  /// @param session The session state.
  void process_control_packet(std::span<const std::byte> payload,
                              const InboundSession& session);
  /// @brief Handles a DATA chunk: checks it is for us, feeds the reliable
  /// channel, acknowledges, and dispatches completed messages.
  ///
  /// @param chunk The chunk.
  /// @param session The session state.
  void process_data(const DataChunk& chunk, const InboundSession& session);
  /// @brief Handles an extended message: decrypts it when flagged, reports it,
  /// and answers an RTT ping.
  ///
  /// @param received The bytes after the headers.
  /// @param family Its family.
  /// @param profile The version's layout.
  /// @param remote_position Its key position, when authenticated.
  void process_extended_message(std::span<const std::byte> received,
                                PacketFamily family, const WireProfile& profile,
                                std::optional<std::uint64_t> remote_position);
  /// @brief Handles a GKTRACE packet, verifying its detached tag when asked.
  ///
  /// @param packet The split packet.
  /// @param profile The version's layout.
  /// @param authenticate Whether crypto is required.
  void process_trace(const PlainPacket& packet, const WireProfile& profile,
                     bool authenticate);
  /// @brief Handles a generic-data chunk: decodes, decrypts and hands it on.
  ///
  /// @param packet The split packet.
  /// @param profile The version's layout.
  /// @param remote_position Its key position, when authenticated.
  void process_generic_data(const PlainPacket& packet,
                            const WireProfile& profile,
                            std::optional<std::uint64_t> remote_position);
  /// @brief Hands a GENERICDATA direct message on.
  ///
  /// @param message The TakionMessage bytes.
  /// @param profile The version's layout, or null.
  /// @return Whether the message was a GENERICDATA direct message (handled or
  ///     rejected); false when it is some other direct message.
  [[nodiscard]] bool dispatch_generic_data_control(
      std::span<const std::byte> message, const WireProfile* profile);
  /// @brief Decodes a completed reliable message by data type and reports it.
  ///
  /// @param message The message.
  /// @param profile The version's layout, or null.
  void dispatch_reliable_message(ReceivedMessage message,
                                 const WireProfile* profile);
  /// @brief Feeds a video unit with an extended header to the frame metrics.
  ///
  /// @param packet The split packet.
  /// @param unit The decoded unit.
  /// @param profile The version's layout.
  /// @param plain_size The packet's size without envelope.
  void observe_video_metrics(const PlainPacket& packet,
                             const AvPacketValues& unit,
                             const WireProfile& profile,
                             std::size_t plain_size);
  /// @brief Feeds an extended header to the jitter and loss metrics.
  ///
  /// @param packet The split packet.
  /// @param profile The version's layout.
  void observe_extension(const PlainPacket& packet,
                         const WireProfile& profile) noexcept;
  /// @brief Handles a DATA_ACK: updates the reliable channel, resends what it
  /// says, reports acknowledgements, and marks the version offer delivered when
  /// nothing is outstanding.
  ///
  /// @param chunk The chunk.
  /// @param session The session state.
  void process_ack(const DataAcknowledgementChunk& chunk,
                   const InboundSession& session);
  /// @brief Decrypts a direct message's data in place with the far end's
  /// keystream at the carrying control packet's key position plus 16, when
  /// the session encrypts direct messages.
  ///
  /// @param message The TakionMessage bytes.
  /// @param profile The session's profile, if committed.
  /// @return False when decryption failed; true otherwise, also when there
  ///     was nothing to decrypt.
  bool decrypt_direct_message(std::span<std::byte> message,
                              const WireProfile* profile);
  /// @brief The key position of the control packet being processed.
  std::optional<std::uint64_t> control_position_;
  /// @brief Reports a non-fatal failure.
  ///
  /// @param error Why.
  /// @param bytes What was rejected; up to 64 bytes are kept.
  /// @param where The caller, recorded as its line.
  void reject(ClientError error, std::span<const std::byte> bytes = {},
              std::source_location where = std::source_location::current());
  /// @brief Reports a parsed controller message, or a decode failure.
  ///
  /// @tparam Input A controller event type.
  /// @param event The parsed event, or nothing when it did not parse.
  template <class Input>
  void dispatch_parsed(std::optional<Input> event) {
    if (!event) {
      reject(ClientError::packet_decode_failure);
      return;
    }
    events_.on_event(std::move(*event));
  }

  /// @brief The owning client.
  ClientInboundHost& host_;
  /// @brief The monotonic clock.
  platform::ClockProvider& clock_;
  /// @brief The session's crypto context.
  CryptoContext& crypto_;
  /// @brief Receives the events.
  EventSink& events_;
  /// @brief Decodes control messages.
  ControlCodec& control_codec_;
  /// @brief The client's configuration.
  const ClientConfig& config_;
  /// @brief The version negotiation.
  VersionNegotiation negotiation_;
  /// @brief AV datagrams held until keys are installed.
  std::vector<net::transport::datagram::OwnedDatagram> postponed_;
  /// @brief Video frame metrics.
  VideoFrameMetricsCollector video_metrics_;
  /// @brief Traffic-class counters.
  ReceivedTosMetrics tos_metrics_;
  /// @brief Extended-header metrics.
  ExtensionReceiveMetrics extension_metrics_;
  /// @brief Whether held datagrams are being replayed.
  bool replaying_{};

 private:
  /// @brief Tells whether a control packet carries a handshake chunk.
  ///
  /// @param plain The packet from its type byte.
  /// @return Whether its chunk type is INIT, INIT_ACK, COOKIE_ECHO or
  ///     COOKIE_ACK.
  static bool is_handshake_chunk(std::span<const std::byte> plain) noexcept;
  /// @brief First version whose controller selectors carry the index in the low
  /// nibble.
  static constexpr std::uint32_t fixed_controller_split_selector_version = 16;
  /// @brief First version with reliable data type 13.
  static constexpr std::uint32_t controller_message_13_version = 17;
  /// @brief Mask of the controller index from version 16.
  static constexpr std::uint8_t fixed_controller_index_mask = 0x0f;
  /// @brief Mask of the rumble player.
  static constexpr std::uint8_t rumble_player_mask = 0x03;
  /// @brief Bytes of a rumble message.
  static constexpr std::size_t rumble_size = 3;
  /// @brief Bytes of a trigger effects message without prefix.
  static constexpr std::size_t trigger_effects_size = 25;
  /// @brief Size from which a trigger effects message has the 8-byte prefix.
  static constexpr std::size_t trigger_effects_prefixed_size = 33;
  /// @brief Bytes of that prefix.
  static constexpr std::size_t trigger_effects_prefix_size = 8;
  /// @brief Reserved bytes between the effect types and the parameters.
  static constexpr std::size_t trigger_effects_reserved_size = 2;
  /// @brief Bytes after a data type 13 message's selector.
  static constexpr std::size_t controller_message_13_payload_size = 4;
  /// @brief Size of pad information messages that are ignored.
  static constexpr std::size_t pad_info_ignored_size = 0x04;
  /// @brief Size of a pad information message carrying only the user flag and
  /// colour.
  static constexpr std::size_t pad_info_user_only_size = 0x07;
  /// @brief Size of a single-pad information message.
  static constexpr std::size_t pad_info_single_pad_size = 0x11;
  /// @brief Smallest other pad information message.
  static constexpr std::size_t pad_info_minimum_size = 12;
  /// @brief Offset of the pad row in the other pad information messages.
  static constexpr std::size_t pad_info_row_offset = 8;
  /// @brief Extracts a controller index from a selector byte.
  ///
  /// @param selector The byte.
  /// @param version The Takion version.
  /// @return The low nibble from version 16, the byte before.
  static constexpr auto fixed_controller_index(std::uint8_t selector,
                                               std::uint32_t version) noexcept
      -> std::uint8_t;
  /// @brief Parses a rumble message.
  ///
  /// @param payload The message.
  /// @param version The Takion version.
  /// @return The rumble; nothing when shorter than 3 bytes.
  static auto parse_rumble(std::span<const std::byte> payload,
                           std::uint32_t version)
      -> std::optional<client_event::Rumble>;
  /// @brief Parses a trigger effects message.
  ///
  /// @param payload The message.
  /// @param version The Takion version.
  /// @return The effects; nothing when shorter than 25 bytes.
  static auto parse_trigger_effects(std::span<const std::byte> payload,
                                    std::uint32_t version)
      -> std::optional<client_event::TriggerEffects>;
  /// @brief Parses a pad information message.
  ///
  /// @param payload The message.
  /// @param version The Takion version.
  /// @return The information; nothing when shorter than 12 bytes and not of a
  ///     known short size.
  static auto parse_pad_info(std::span<const std::byte> payload,
                             std::uint32_t version)
      -> std::optional<client_event::PadInfo>;
  /// @brief Parses a data type 13 message.
  ///
  /// @param payload The message.
  /// @return The message; nothing when shorter than 5 bytes.
  static auto parse_controller_message_13(std::span<const std::byte> payload)
      -> std::optional<client_event::ControllerMessage13>;
};

}  // namespace libchiaking::net::protocol::takion::client_detail
