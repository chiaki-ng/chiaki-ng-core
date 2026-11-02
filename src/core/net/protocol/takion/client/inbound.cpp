#include "src/core/net/protocol/takion/client/inbound.hpp"
#include "src/core/utils/bytes/byte_io.hpp"
#include "src/core/utils/bytes/endian.hpp"

#include "src/core/net/protocol/takion/control_chunk_codec.hpp"
#include "src/core/net/protocol/takion/control_codec.hpp"
#include "src/core/net/protocol/takion/detail/envelope_codec.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/ext_message_codec.hpp"
#include "src/core/net/protocol/takion/limits.hpp"
#include "src/core/net/protocol/takion/profile.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::net::protocol::takion::client_detail {
bool ClientInbound::is_handshake_chunk(
    std::span<const std::byte> plain) noexcept {
  if (plain.size() <= control_chunk_type_offset) return false;
  const auto type = ControlChunkType::from_code(
      std::to_integer<std::uint8_t>(plain[control_chunk_type_offset]));
  return type == ControlChunk::init ||
         type == ControlChunk::init_acknowledgement ||
         type == ControlChunk::cookie ||
         type == ControlChunk::cookie_acknowledgement;
}

constexpr auto ClientInbound::fixed_controller_index(
    std::uint8_t selector, std::uint32_t version) noexcept -> std::uint8_t {
  return version >= fixed_controller_split_selector_version
             ? static_cast<std::uint8_t>(selector & fixed_controller_index_mask)
             : selector;
}

void ClientInbound::process_datagram(
    net::transport::datagram::Datagram datagram,
    const InboundSession& session) {
  if (!session.profile) {
    reject(ClientError::invalid_configuration);
    return;
  }
  const auto profile = *session.profile;
  const auto decoded_envelope = EnvelopeCodec::decode(
      datagram, config_.receive_envelope.value_or(config_.envelope));
  if (!decoded_envelope) {
    reject(ClientError::packet_decode_failure);
    return;
  }
  const auto plain_bytes = datagram.last(decoded_envelope.value().size());
  if (plain_bytes.empty()) {
    reject(ClientError::packet_decode_failure);
    return;
  }
  const PacketType type{std::to_integer<std::uint8_t>(plain_bytes.front())};
  const auto family = type.family();
  if (!family) {
    reject(ClientError::packet_decode_failure);
    return;
  }
  if (session.scope == InboundScope::control &&
      *family != PacketFamily::control)
    return;
  if (plain_bytes.size() < profile.packet_header_size(type)) {
    reject(ClientError::packet_decode_failure);
    return;
  }

  if (*family == PacketFamily::feedback_state ||
      *family == PacketFamily::feedback_history) {
    reject(ClientError::invalid_state);
    return;
  }
  if (*family == PacketFamily::extended_message &&
      !profile.features().contains(Feature::extended_messages)) {
    reject(ClientError::unsupported_version);
    return;
  }
  if ((*family == PacketFamily::generic_data &&
       !TypedMessageCodec::generic_data_enabled(profile)) ||
      (*family == PacketFamily::trace &&
       !TypedMessageCodec::trace_messages_enabled(profile))) {
    reject(ClientError::packet_decode_failure);
    return;
  }

  const bool handshake_packet =
      *family == PacketFamily::control && is_handshake_chunk(plain_bytes);
  const bool unauthenticated_family =
      *family == PacketFamily::length_prefixed ||
      *family == PacketFamily::client_information;
  std::optional<std::uint64_t> remote_position;
  if (config_.crypto_policy == CryptoPolicy::required &&
      !unauthenticated_family) {
    if (!session.crypto_installed) {
      if (*family == PacketFamily::video ||
          *family == PacketFamily::audio_and_haptics) {
        if (postponed_.size() >= postponed_capacity) {
          reject(ClientError::crypto_not_installed);
          return;
        }
        postponed_.emplace_back(datagram.begin(), datagram.end());
        return;
      }
      if (!handshake_packet && !(config_.allow_plain_control_until_crypto &&
                                 *family == PacketFamily::control)) {
        reject(ClientError::crypto_not_installed);
        return;
      }
    } else if (*family != PacketFamily::trace) {
      auto authenticated = crypto_.authenticate_and_decrypt_remote(
          plain_bytes, *family, profile,
          PacketCryptoCoverage{0, plain_bytes.size(), plain_bytes.size(), 0});
      if (!authenticated) {
        reject(ClientError::crypto_failure);
        return;
      }
      remote_position = authenticated.value();
    }
  }

  const auto plain_size = plain_bytes.size();
  auto packet = PacketCodec::decode_plain_packet(plain_bytes, profile);
  if (!packet) {
    reject(ClientError::packet_decode_failure);
    return;
  }
  observe_extension(packet.value(), profile);
  const bool av_family = *family == PacketFamily::video ||
                         *family == PacketFamily::audio_and_haptics;
  const bool unit_layout = profile.av_layout() != AvLayout::version_7;
  std::optional<AvPacketValues> unit;
  std::span<const std::byte> sealed;
  if (av_family && (remote_position || unit_layout)) {
    auto decoded = PacketCodec::decode_av_header(packet.value(), profile);
    if (!decoded) {
      reject(ClientError::packet_decode_failure);
      return;
    }
    unit = std::move(decoded.value().values);
    sealed = decoded.value().data;
    unit->encrypted = remote_position && unit_layout &&
                      *family == PacketFamily::video && config_.video_gate &&
                      config_.video_gate->video_frame_stale(unit->frame_index);
    if (remote_position && unit_layout && !unit->encrypted)
      unit->data.resize(sealed.size());
    else
      unit->data.assign(sealed.begin(), sealed.end());
  }
  if (remote_position && unit) {
    if (config_.source_tap)
      config_.source_tap->on_source_datagram(
          PacketCodec::describe_source_datagram(*unit, profile,
                                                *remote_position, plain_bytes));
  }
  if (remote_position && unit && !unit->encrypted) {
    const auto in_place = plain_bytes.last(sealed.size());
    const auto decrypted =
        unit_layout
            ? crypto_.decrypt_remote_payload(sealed, unit->data, *family,
                                             profile, *remote_position)
            : crypto_.decrypt_remote_payload(in_place, in_place, *family,
                                             profile, *remote_position);
    if (!decrypted) {
      reject(ClientError::crypto_failure);
      return;
    }
  }
  switch (*family) {
    case PacketFamily::control:
      control_position_ = remote_position;
      process_control_packet(packet.value().payload, session);
      break;
    case PacketFamily::video:
      if (unit_layout) {
        observe_video_metrics(packet.value(), *unit, profile, plain_size);
        events_.on_event(client_event::AvUnitReceived{std::move(*unit)});
      } else {
        const bool nalu =
            (packet.value().type.flags() & nalu_info_structs_flag) != 0;
        client_event::EncodedVideo video{
            {packet.value().payload.begin(), packet.value().payload.end()}};
        video.uses_nalu_info_structs = nalu;
        events_.on_event(std::move(video));
      }
      break;
    case PacketFamily::audio_and_haptics:
      if (unit_layout) {
        events_.on_event(client_event::AvUnitReceived{std::move(*unit)});
      } else {
        const bool nalu =
            (packet.value().type.flags() & nalu_info_structs_flag) != 0;
        events_.on_event(client_event::EncodedAudio{
            {packet.value().payload.begin(), packet.value().payload.end()},
            nalu});
      }
      break;
    case PacketFamily::extended_message:
      process_extended_message(packet.value().payload, *family, profile,
                               remote_position);
      break;
    case PacketFamily::congestion:
      events_.on_event(client_event::RawFamilyPacketReceived{
          *family,
          {packet.value().payload.begin(), packet.value().payload.end()}});
      break;
    case PacketFamily::length_prefixed: {
      auto payload = TypedMessageCodec::decode_length_prefixed_body(
          packet.value().payload);
      if (!payload) {
        reject(ClientError::packet_decode_failure);
        return;
      }
      events_.on_event(
          client_event::LengthPrefixedReceived{std::move(payload).value()});
      break;
    }
    case PacketFamily::client_information: {
      auto values =
          TypedMessageCodec::decode_client_info_message(packet.value().payload);
      if (!values) {
        reject(ClientError::packet_decode_failure);
        return;
      }
      events_.on_event(
          client_event::ClientInfoReceived{std::move(values).value()});
      break;
    }
    case PacketFamily::trace:
      process_trace(packet.value(), profile,
                    config_.crypto_policy == CryptoPolicy::required);
      break;
    case PacketFamily::generic_data:
      process_generic_data(packet.value(), profile, remote_position);
      break;
    case PacketFamily::feedback_state:
    case PacketFamily::feedback_history:
      reject(ClientError::invalid_state);
      break;
    default:
      events_.on_event(client_event::ProbeReceived{
          *family,
          {packet.value().payload.begin(), packet.value().payload.end()}});
      break;
  }
}

void ClientInbound::observe_video_metrics(const PlainPacket& packet,
                                          const AvPacketValues& unit,
                                          const WireProfile& profile,
                                          std::size_t plain_size) {
  if (!profile.features().contains(Feature::extended_packet_headers) ||
      !packet.extended_header)
    return;
  const auto data_size = unit.data.size();
  video_metrics_.observe(VideoFrameObservation{
      clock_.now_microseconds(),
      unit.unit_index,
      unit.units_in_frame_total,
      unit.frame_index,
      unit.packet_index,
      packet.extended_header->timestamp,
      packet.extended_header->counter,
      static_cast<std::uint32_t>(data_size),
      static_cast<std::uint32_t>(plain_size - data_size),
      unit.adaptive_stream_index,
      unit.units_in_frame_fec,
      unit.slice ? unit.slice->index : std::uint16_t{},
      unit.slice ? unit.slice->count : std::uint16_t{},
  });
}

void ClientInbound::observe_extension(const PlainPacket& packet,
                                      const WireProfile& profile) noexcept {
  if (!profile.features().contains(Feature::extended_packet_headers) ||
      !packet.extended_header)
    return;
  const auto now = clock_.now_microseconds();
  if (replaying_)
    extension_metrics_.observe_untimed(packet.extended_header->counter, now);
  else
    extension_metrics_.observe(packet.extended_header->timestamp,
                               packet.extended_header->counter, now);
}

void ClientInbound::replay_postponed(const InboundSession& session) {
  if (!session.crypto_installed || postponed_.empty()) return;
  auto postponed = std::move(postponed_);
  postponed_.clear();
  struct ReplayScope {
    bool& flag;
    ~ReplayScope() { flag = false; }
  } scope{replaying_};
  replaying_ = true;
  for (auto& datagram : postponed) process_datagram(datagram, session);
}

void ClientInbound::process_trace(const PlainPacket& packet,
                                  const WireProfile& profile,
                                  bool authenticate) {
  const auto values = TypedMessageCodec::decode_trace_message(packet.payload);
  if (!values) {
    reject(ClientError::packet_decode_failure);
    return;
  }
  if (authenticate) {
    auto message =
        TypedMessageCodec::trace_authenticated_message(packet.payload);
    if (!message) {
      reject(ClientError::packet_decode_failure);
      return;
    }
    PlainPacket canonical{
        PacketType{static_cast<std::uint8_t>(PacketFamily::trace)},
        packet.extended_header, message.value()};
    const auto authenticated =
        PacketCodec::encode_plain_packet(canonical, profile);
    if (!authenticated) {
      reject(ClientError::packet_decode_failure);
      return;
    }
    const auto verified = crypto_.verify_remote_detached(
        authenticated.value(), values.value().key_position,
        TypedMessageCodec::trace_tag_bytes(values.value().tag));
    if (!verified) {
      reject(ClientError::crypto_failure);
      return;
    }
  }
  events_.on_event(client_event::TraceReceived{values.value().sequence});
}

void ClientInbound::process_generic_data(
    const PlainPacket& packet, const WireProfile& profile,
    std::optional<std::uint64_t> remote_position) {
  auto chunk = TypedMessageCodec::decode_generic_data_chunk(packet, profile);
  if (!chunk) {
    reject(ClientError::packet_decode_failure, packet.payload);
    return;
  }
  const auto chunk_payload = chunk.value().payload.span();
  if (remote_position &&
      !crypto_.decrypt_remote_payload(chunk_payload, chunk_payload,
                                      PacketFamily::generic_data, profile,
                                      *remote_position)) {
    reject(ClientError::crypto_failure);
    return;
  }
  if (auto accepted =
          host_.inbound_generic_data_chunk(std::move(chunk).value());
      !accepted)
    host_.inbound_fail_terminal(accepted.error());
}

bool ClientInbound::dispatch_generic_data_control(
    std::span<const std::byte> message, const WireProfile* profile) {
  auto control = TypedMessageCodec::decode_generic_data_direct_message(message);
  if (!control) {
    reject(ClientError::packet_decode_failure, message);
    return true;
  }
  if (!control.value()) return false;
  if (!profile || !TypedMessageCodec::generic_data_enabled(*profile)) {
    reject(ClientError::packet_decode_failure);
    return true;
  }
  if (auto accepted = host_.inbound_generic_data_control(*control.value());
      !accepted)
    host_.inbound_fail_terminal(accepted.error());
  return true;
}

void ClientInbound::process_extended_message(
    std::span<const std::byte> received, PacketFamily family,
    const WireProfile& profile, std::optional<std::uint64_t> remote_position) {
  SecretBytes payload{received};
  const auto header = ExtMessageCodec::decode_header(payload.bytes());
  if (!header) {
    reject(ClientError::packet_decode_failure, received);
    return;
  }
  const auto body = payload.span().subspan(ext_message_header_size);
  if (header->encrypted) {
    if (!remote_position) {
      reject(ClientError::crypto_not_installed);
      return;
    }
    if (!crypto_.decrypt_remote_payload(body, body, family, profile,
                                        *remote_position)) {
      reject(ClientError::crypto_failure);
      return;
    }
  }
  const auto ping = ExtMessageCodec::decode_rtt_ping(body);
  events_.on_event(client_event::ProbeReceived{
      family, std::vector<std::byte>{payload.begin(), payload.end()}});
  if (!ping) return;
  const auto pong = ExtMessageCodec::encode_rtt_pong(*ping);
  if (!pong) {
    reject(ClientError::packet_encode_failure);
    return;
  }
  if (auto sent = host_.inbound_send_extended_message(*pong); !sent)
    reject(sent.error());
}

void ClientInbound::process_control_packet(std::span<const std::byte> payload,
                                           const InboundSession& session) {
  auto decoded = ControlChunkCodec::decode_packet(payload);
  if (!decoded) {
    reject(ClientError::packet_decode_failure);
    return;
  }
  std::visit(
      [this, &session](auto&& chunk) {
        using T = std::decay_t<decltype(chunk)>;
        if constexpr (std::is_same_v<T, InitAcknowledgementChunk>) {
          if (auto result = host_.inbound_handle_init_ack(chunk); !result)
            host_.inbound_fail_terminal(result.error());
        } else if constexpr (std::is_same_v<T, CookieAcknowledgementChunk>) {
          if (auto result = host_.inbound_handle_cookie_ack(chunk); !result)
            host_.inbound_fail_terminal(result.error());
        } else if constexpr (std::is_same_v<T, DataChunk>) {
          process_data(chunk, session);
        } else if constexpr (std::is_same_v<T, DataAcknowledgementChunk>) {
          process_ack(chunk, session);
        } else {
          reject(ClientError::invalid_state);
        }
      },
      std::move(decoded).value());
}

void ClientInbound::process_data(const DataChunk& chunk,
                                 const InboundSession& session) {
  if (!host_.inbound_connected() || !session.reliable || !session.established ||
      chunk.header.verification_tag.value !=
          session.established->local_tag.value) {
    reject(ClientError::invalid_state);
    return;
  }
  auto received = session.reliable->receive(
      chunk.payload, (chunk.header.flags & data_final_fragment_flag) != 0,
      clock_.now_microseconds());
  if (received.anomaly && *received.anomaly != ReliableChannelError::duplicate)
    reject(ClientError::reliable_channel_failure);
  if (received.acknowledgement) {
    auto result = host_.inbound_send_ack(*received.acknowledgement);
    if (!result) {
      host_.inbound_fail_terminal(result.error());
      return;
    }
  }
  for (auto& message : received.messages)
    dispatch_reliable_message(std::move(message), session.profile);
}

auto ClientInbound::parse_rumble(std::span<const std::byte> payload,
                                 std::uint32_t version)
    -> std::optional<client_event::Rumble> {
  bytes::Reader reader{payload};
  const auto selector = reader.u8();
  const auto left = reader.u8();
  const auto right = reader.u8();
  if (!selector || !left || !right) return std::nullopt;
  return client_event::Rumble{
      static_cast<std::uint8_t>(fixed_controller_index(*selector, version) &
                                rumble_player_mask),
      *left, *right, *selector};
}

auto ClientInbound::parse_trigger_effects(std::span<const std::byte> payload,
                                          std::uint32_t version)
    -> std::optional<client_event::TriggerEffects> {
  if (payload.size() < trigger_effects_size) return std::nullopt;
  bytes::Reader reader{payload};
  const auto prefix = payload.size() >= trigger_effects_prefixed_size
                          ? trigger_effects_prefix_size
                          : 0;
  client_event::TriggerEffects effects;
  const auto player = reader.skip(prefix).and_then([&] { return reader.u8(); });
  const auto type_left = reader.u8();
  const auto type_right = reader.u8();
  const auto left = reader.skip(trigger_effects_reserved_size).and_then([&] {
    return reader.take(effects.left.size());
  });
  const auto right = reader.take(effects.right.size());
  if (!player || !type_left || !type_right || !left || !right)
    return std::nullopt;
  effects.player_index = fixed_controller_index(*player, version);
  effects.type_left = *type_left;
  effects.type_right = *type_right;
  const auto octet = [](std::byte value) {
    return std::to_integer<std::uint8_t>(value);
  };
  std::ranges::transform(*left, effects.left.begin(), octet);
  std::ranges::transform(*right, effects.right.begin(), octet);
  return effects;
}

auto ClientInbound::parse_pad_info(std::span<const std::byte> payload,
                                   std::uint32_t version)
    -> std::optional<client_event::PadInfo> {
  const auto byte = [payload](std::size_t offset) {
    return std::to_integer<std::uint8_t>(payload[offset]);
  };
  const auto index_at = [&byte, version](std::size_t offset) {
    return fixed_controller_index(byte(offset), version);
  };
  client_event::PadInfo info;
  std::size_t row = 0;
  switch (payload.size()) {
    case pad_info_user_only_size:
      info.user_valid = true;
      break;
    case pad_info_single_pad_size:
      info.haptic_intensity = byte(12);
      info.trigger_intensity = byte(13);
      info.motion_reset = index_at(0) == 0 && byte(4) != 0;
      info.user_valid = byte(7) != 0;
      if (index_at(0) == 0) info.motion_settings = std::array{byte(5), byte(6)};
      break;
    default:
      if (payload.size() < pad_info_minimum_size) return std::nullopt;
      row = pad_info_row_offset;
      if (payload.size() > 21) {
        info.haptic_intensity = byte(20);
        info.trigger_intensity = byte(21);
      }
      info.motion_reset =
          payload.size() > 12 && index_at(8) == 0 && byte(12) != 0;
      info.user_valid = payload.size() <= 15 || byte(15) != 0;
      if (payload.size() > 14 && index_at(8) == 0)
        info.motion_settings = std::array{byte(13), byte(14)};
      break;
  }
  info.index = index_at(row);
  info.led = {byte(row + 1), byte(row + 2), byte(row + 3)};
  return info;
}

auto ClientInbound::parse_controller_message_13(
    std::span<const std::byte> payload)
    -> std::optional<client_event::ControllerMessage13> {
  bytes::Reader reader{payload};
  const auto selector = reader.u8();
  const auto body = reader.take(controller_message_13_payload_size);
  if (!selector || !body) return std::nullopt;
  client_event::ControllerMessage13 controller_message;
  controller_message.selector = *selector;
  std::ranges::transform(
      *body, controller_message.payload.begin(),
      [](std::byte value) { return std::to_integer<std::uint8_t>(value); });
  return controller_message;
}

void ClientInbound::dispatch_reliable_message(ReceivedMessage message,
                                              const WireProfile* profile) {
  if (config_.report_control_payload_lengths) {
    const auto prefix_byte = message.data_type.known() ? 0U : 1U;
    events_.on_event(client_event::ControlPayloadReceived{
        message.payload.size() + prefix_byte});
  }
  if (const auto prefix =
          profile ? profile->data_message_prefix_size(message.data_type) : 0;
      prefix != 0) {
    if (message.payload.size() <= prefix) return;
    message.payload.erase(
        message.payload.begin(),
        message.payload.begin() + static_cast<std::ptrdiff_t>(prefix));
  }
  if (message.data_type.known() != MessageDataKind::protobuf) {
    const auto version =
        profile ? profile->version() : config_.bootstrap_version;
    const auto kind = message.data_type.known();
    const auto payload = std::span<const std::byte>{message.payload};
    if (kind == MessageDataKind::rumble) {
      dispatch_parsed(parse_rumble(payload, version));
    } else if (kind == MessageDataKind::trigger_effects) {
      dispatch_parsed(parse_trigger_effects(payload, version));
    } else if (kind == MessageDataKind::pad_information) {
      if (payload.size() != pad_info_ignored_size)
        dispatch_parsed(parse_pad_info(payload, version));
    } else if (kind == MessageDataKind::controller_message_13 &&
               version >= controller_message_13_version) {
      dispatch_parsed(parse_controller_message_13(payload));
    } else {
      events_.on_event(client_event::ProbeReceived{PacketFamily::control,
                                                   std::move(message.payload)});
    }
    return;
  }
  if (!decrypt_direct_message(message.payload, profile)) {
    reject(ClientError::crypto_failure, message.payload);
    return;
  }
  auto decoded = control_codec_.decode(message.payload);
  if (!decoded) {
    reject(ClientError::control_codec_failure, message.payload);
    return;
  }
  if (const auto* unsupported =
          std::get_if<UnsupportedControlMessage>(&decoded.value());
      unsupported && unsupported->type == ControlPayloadType::direct_message &&
      dispatch_generic_data_control(message.payload, profile))
    return;
  if (auto* selected = std::get_if<ProtocolRequestAck>(&decoded.value())) {
    negotiation_.answer_heard(selected->selected_version);
  } else if (const auto* info = std::get_if<StreamInfo>(&decoded.value())) {
    tos_metrics_.configure(info->tos_report_packet_threshold,
                           info->tos_report_interval_ms);
  } else if (auto* request = std::get_if<ProtocolRequest>(&decoded.value())) {
    if (auto result = host_.inbound_send_control(ProtocolRequestAck{
            negotiation_.highest_allowed(request->supported_versions)});
        !result) {
      host_.inbound_fail_terminal(result.error());
      return;
    }
  }
  if (std::holds_alternative<UnsupportedControlMessage>(decoded.value())) {
    events_.on_event(client_event::ControlReceived{std::move(decoded).value(),
                                                   std::move(message.payload)});
    return;
  }
  events_.on_event(client_event::ControlReceived{std::move(decoded).value()});
}

void ClientInbound::process_ack(const DataAcknowledgementChunk& chunk,
                                const InboundSession& session) {
  if (!session.reliable || !session.established ||
      chunk.header.verification_tag.value !=
          session.established->local_tag.value) {
    reject(ClientError::invalid_state);
    return;
  }
  const auto result =
      session.reliable->acknowledge(chunk.values, clock_.now_microseconds());
  if (!result) {
    reject(ClientError::reliable_channel_failure);
    return;
  }
  for (const auto& fragment : result->retransmissions) {
    if (auto sent = host_.inbound_send_fragment(fragment); !sent) {
      host_.inbound_fail_terminal(sent.error());
      return;
    }
  }
  for (std::size_t index = 0; index < result->exhausted_sequences.size();
       ++index)
    reject(ClientError::reliable_channel_failure);
  for (const auto& acked : result->acknowledged)
    events_.on_event(client_event::ReliableAcked{
        acked.sequence,
        acked.round_trip_time_us,
        acked.operation_id
            ? std::optional<ControlOperationId>{ControlOperationId{
                  *acked.operation_id}}
            : std::nullopt,
        acked.operation_complete,
    });
  if (session.reliable->outstanding_fragment_count() == 0)
    negotiation_.offer_delivered();
}

std::optional<std::uint32_t> ClientInbound::take_ready_commit(
    const ReliableChannel* reliable) {
  return negotiation_.commit(reliable && !reliable->has_incomplete_message() &&
                             reliable->outstanding_fragment_count() == 0);
}

bool ClientInbound::decrypt_direct_message(std::span<std::byte> message,
                                           const WireProfile* profile) {
  if (!profile || !control_position_ ||
      !profile->features().contains(Feature::direct_message_encryption))
    return true;
  const auto size = TypedMessageCodec::direct_message_data_size(message);
  if (size == 0) return true;
  return crypto_
      .apply_remote_keystream(*control_position_ + takion_crypto_block_size,
                              message.last(size))
      .has_value();
}

void ClientInbound::reject(ClientError error, std::span<const std::byte> bytes,
                           std::source_location where) {
  constexpr std::size_t kept = 64;
  const auto shown = bytes.first(std::min(bytes.size(), kept));
  events_.on_event(client_event::PacketRejected{
      error, std::nullopt, where.line(),
      std::vector<std::byte>(shown.begin(), shown.end())});
}

}  // namespace libchiaking::net::protocol::takion::client_detail
