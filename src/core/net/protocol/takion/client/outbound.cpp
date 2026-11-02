#include "src/core/net/protocol/takion/client/outbound.hpp"

#include "src/core/net/protocol/takion/client/limits.hpp"
#include "src/core/net/protocol/takion/detail/handshake_types.hpp"
#include "src/core/net/protocol/takion/detail/typed_message_codec.hpp"
#include "src/core/net/protocol/takion/ext_message_codec.hpp"
#include "src/core/net/protocol/takion/limits.hpp"
#include "src/core/utils/numeric/checked.hpp"

#include <limits>
#include <type_traits>
#include <utility>
#include <variant>

namespace libchiaking::net::protocol::takion::client_detail {
ClientResult<void> ClientOutbound::without_datagram(
    ClientResult<SecretBytes> datagram) {
  return datagram.transform([](const SecretBytes&) {});
}

ClientResult<SecretBytes> ClientOutbound::send_fragment(
    const OutboundFragment& fragment, const EstablishedHandshake& established,
    std::size_t encrypted_tail) {
  const std::uint8_t flags = fragment.end_of_message ? data_end_flag : 0;
  const auto prefix = fragment.layout == FragmentLayout::first
                          ? first_fragment_overhead
                          : continuation_fragment_overhead;
  const auto message_size =
      numeric::checked_sub(fragment.payload.size(), prefix);
  if (!message_size || encrypted_tail > *message_size)
    return unexpected(ClientError::packet_encode_failure);
  return send_encoded_chunk(
      ControlChunkCodec::encode_data_packet(
          ControlHeader{established.remote_tag, {}, 0, flags},
          fragment.payload.bytes()),
      false, *message_size, encrypted_tail);
}

ClientResult<SecretBytes> ClientOutbound::send_encoded_chunk(
    Expected<SecretBytes, ParseFailure> encoded, bool handshake_packet,
    std::optional<std::size_t> key_advance, std::size_t encrypted_tail,
    SendClass send_class) {
  if (!encoded || encrypted_tail > encoded.value().size())
    return unexpected(ClientError::packet_encode_failure);
  return send_packet(
      PacketFamily::control, encoded.value().bytes(), handshake_packet,
      key_advance.value_or(control_wire_size(encoded.value().size())),
      encoded.value().size() - encrypted_tail,
      encrypted_tail == 0
          ? std::optional<std::uint64_t>{}
          : std::optional<std::uint64_t>{takion_crypto_block_size},
      send_class);
}

ClientError ClientOutbound::send_error(
    net::transport::datagram::TransportError error) noexcept {
  if (error == net::transport::datagram::TransportError::closed)
    return ClientError::transport_failure;
  return ClientError::send_dropped;
}

auto ClientOutbound::offer(std::span<const std::byte> datagram)
    -> ClientResult<Offer> {
  if (backlog_ == Backlog::waiting) return Offer::refused;
  last_send_attempt_us_ = clock_.now_microseconds();
  auto sent = transport_->send(datagram);
  if (sent) {
    if (sent.value() != datagram.size())
      return unexpected(ClientError::short_send);
    return Offer::sent;
  }
  if (sent.error() != net::transport::datagram::TransportError::would_block)
    return unexpected(send_error(sent.error()));
  refused_at_us_ = last_send_attempt_us_;
  return Offer::refused;
}

bool ClientOutbound::make_room(SendClass send_class) {
  if (send_class == SendClass::immediate) return false;
  if (unsent_count() < config_.limits.unsent_datagram_capacity) return true;
  if (send_class != SendClass::essential) return false;
  if (unsent_head_ && unsent_head_->send_class == SendClass::best_effort) {
    unsent_head_ = unsent_tail_.take();
    return true;
  }
  bool displaced = false;
  const auto waiting = unsent_tail_.size();
  for (std::size_t index = 0; index < waiting; ++index) {
    auto entry = unsent_tail_.take();
    if (!displaced && entry->send_class == SendClass::best_effort) {
      displaced = true;
      continue;
    }
    static_cast<void>(unsent_tail_.push(std::move(*entry)));
  }
  return displaced;
}

void ClientOutbound::keep_unsent(SecretBytes datagram, SendClass send_class) {
  UnsentDatagram entry{std::move(datagram), send_class, send_deadline_us_};
  if (!unsent_head_)
    unsent_head_ = std::move(entry);
  else
    static_cast<void>(unsent_tail_.push(std::move(entry)));
  backlog_ = Backlog::waiting;
}

ClientResult<UnsentRetry> ClientOutbound::retry_unsent(std::uint64_t now_us) {
  UnsentRetry retry;
  while (backlog_ == Backlog::waiting) {
    if (!unsent_head_) unsent_head_ = unsent_tail_.take();
    if (!unsent_head_) {
      backlog_ = Backlog::clear;
      break;
    }
    if (unsent_head_->expires_us && now_us >= *unsent_head_->expires_us) {
      ++retry.expired;
      unsent_head_.reset();
      continue;
    }
    last_send_attempt_us_ = clock_.now_microseconds();
    const auto sent = transport_->send(unsent_head_->bytes.bytes());
    if (!sent &&
        sent.error() == net::transport::datagram::TransportError::would_block) {
      refused_at_us_ = last_send_attempt_us_;
      break;
    }
    if (!sent &&
        sent.error() == net::transport::datagram::TransportError::closed)
      return unexpected(ClientError::transport_failure);
    if (sent && sent.value() == unsent_head_->bytes.size())
      ++retry.sent;
    else
      ++retry.dropped;
    unsent_head_.reset();
  }
  return retry;
}

std::optional<std::uint64_t> ClientOutbound::next_retry_us() const noexcept {
  if (backlog_ == Backlog::clear) return std::nullopt;
  return refused_at_us_ + config_.limits.unsent_retry_interval_us;
}

std::size_t ClientOutbound::unsent_count() const noexcept {
  return (unsent_head_ ? 1 : 0) + unsent_tail_.size();
}

ClientResult<void> ClientOutbound::resend_datagram(
    std::span<const std::byte> datagram) {
  auto offered = offer(datagram);
  if (!offered) return unexpected(offered.error());
  if (offered.value() == Offer::refused)
    return unexpected(ClientError::send_blocked);
  return {};
}

ClientResult<void> ClientOutbound::send_ack(
    const DataAcknowledgement& acknowledgement,
    const EstablishedHandshake& established) {
  DataAcknowledgement sent{acknowledgement.cumulative_sequence,
                           acknowledgement.advertised_receive_window,
                           acknowledgement.gap_blocks,
                           {}};
  return send_chunk(
      DataAcknowledgementChunk{ControlHeader{established.remote_tag, {}, 0, 0},
                               std::move(sent)},
      false, std::nullopt, SendClass::best_effort);
}

ClientResult<void> ClientOutbound::send_plain(
    PacketFamily family, std::span<const std::byte> payload,
    bool handshake_packet, std::optional<std::size_t> key_advance,
    SendClass send_class) {
  return without_datagram(send_packet(
      family, payload, handshake_packet, key_advance.value_or(payload.size()),
      payload.size(), std::nullopt, send_class));
}

ClientResult<void> ClientOutbound::send_extended_message(
    std::span<const std::byte> message) {
  if (!profile_) return unexpected(ClientError::invalid_configuration);
  if (!profile_->features().contains(Feature::extended_messages))
    return unexpected(ClientError::unsupported_version);
  if (message.size() > ext_message_payload_limit)
    return unexpected(ClientError::packet_encode_failure);
  const bool encrypted = config_.crypto_policy == CryptoPolicy::required;
  if (encrypted && !crypto_installed_)
    return unexpected(ClientError::crypto_not_installed);
  SecretBytes payload{
      ExtMessageCodec::encode_header(ExtMessageHeader{encrypted})};
  payload.append(message);
  const PacketType type{
      static_cast<std::uint8_t>(PacketFamily::extended_message)};
  const auto key_advance = profile_->packet_header_size(type) +
                           ext_message_header_size + message.size() +
                           takion_crypto_block_size;
  return without_datagram(send_packet(
      PacketFamily::extended_message, payload.bytes(), !encrypted, key_advance,
      ext_message_header_size, std::nullopt, SendClass::best_effort));
}

ExtendedPacketHeader ClientOutbound::next_extended_header() noexcept {
  return ExtendedPacketHeader{
      static_cast<std::uint32_t>(clock_.now_microseconds() -
                                 extended_epoch_us_),
      tx_extended_counter_++};
}

ClientResult<SecretBytes> ClientOutbound::send_packet(
    PacketFamily family, std::span<const std::byte> payload,
    bool handshake_packet, std::size_t key_advance,
    std::size_t encrypted_payload_offset,
    std::optional<std::uint64_t> cipher_key_position_addition,
    SendClass send_class) {
  if (!profile_) return unexpected(ClientError::invalid_configuration);
  const PacketType type{static_cast<std::uint8_t>(family)};
  std::optional<ExtendedPacketHeader> extended_header;
  if (profile_->extended_header() ==
          ExtendedHeaderPolicy::version_20_except_bootstrap_families &&
      PacketType::has_extended_header(type.family_code())) {
    extended_header = next_extended_header();
  }
  auto plain = PacketCodec::encode_plain_packet(type, extended_header, payload,
                                                *profile_);
  if (!plain) return unexpected(ClientError::packet_encode_failure);

  if (handshake_packet)
    return transmit(plain.value().bytes(), std::nullopt, send_class);
  if (config_.crypto_policy == CryptoPolicy::required && !crypto_installed_ &&
      config_.allow_plain_control_until_crypto &&
      (family == PacketFamily::control || family == PacketFamily::congestion))
    return transmit(plain.value().bytes(), std::nullopt, send_class);
  auto reservation =
      protect(plain.value().span(), family,
              plain.value().size() - payload.size() + encrypted_payload_offset,
              payload.size() - encrypted_payload_offset, key_advance,
              cipher_key_position_addition);
  if (!reservation) return unexpected(reservation.error());
  return transmit(plain.value().bytes(), std::move(reservation).value(),
                  send_class);
}

auto ClientOutbound::protect(
    std::span<std::byte> packet, PacketFamily family,
    std::size_t payload_offset, std::size_t payload_size,
    std::uint64_t key_advance,
    std::optional<std::uint64_t> cipher_key_position_addition)
    -> ClientResult<Reservation> {
  if (config_.crypto_policy != CryptoPolicy::required) return Reservation{};
  if (!crypto_installed_) return unexpected(ClientError::crypto_not_installed);
  auto reservation = crypto_.protect_local(
      packet, family, *profile_,
      PacketCryptoCoverage{0, packet.size(), payload_offset, payload_size,
                           cipher_key_position_addition},
      key_advance);
  if (!reservation) return unexpected(ClientError::crypto_failure);
  return Reservation{std::move(reservation).value()};
}

ClientResult<void> ClientOutbound::send_probe(const SendProbe& probe) {
  if (!profile_) return unexpected(ClientError::invalid_configuration);
  const auto profile = *profile_;
  const auto family = probe.media == SenkushaProbeMedia::video
                          ? PacketFamily::video
                          : PacketFamily::audio_and_haptics;
  const PacketType type{static_cast<std::uint8_t>(family)};
  const auto envelope_size = EnvelopeCodec::overhead(config_.envelope);
  const auto header_size = profile.packet_header_size(type);
  const auto without_payload = numeric::checked_add(envelope_size, header_size);
  const auto actual =
      without_payload ? numeric::checked_add(*without_payload,
                                             probe.encoded_av_payload.size())
                      : std::optional<std::size_t>{};
  if (probe.expected_datagram_size == 0 || !actual ||
      *actual != probe.expected_datagram_size ||
      *actual > config_.limits.maximum_fragment_size)
    return unexpected(ClientError::packet_encode_failure);
  return send_plain(family, probe.encoded_av_payload, false, std::nullopt,
                    SendClass::immediate);
}

ClientResult<void> ClientOutbound::send_congestion(
    const SendCongestion& congestion) {
  if (!profile_) return unexpected(ClientError::invalid_configuration);
  auto encoded = PacketCodec::encode_congestion(congestion.values, *profile_,
                                                config_.envelope);
  if (!encoded) return unexpected(ClientError::packet_encode_failure);
  if (encoded.value().datagram_size > config_.limits.maximum_fragment_size)
    return unexpected(ClientError::packet_encode_failure);
  return send_plain(PacketFamily::congestion, encoded.value().encoded_payload,
                    false, congestion_packet_size, SendClass::best_effort);
}

ClientResult<void> ClientOutbound::send_feedback_state(
    const SendFeedbackState& feedback) {
  if (!profile_) return unexpected(ClientError::invalid_configuration);

  const PacketType type{
      static_cast<std::uint8_t>(PacketFamily::feedback_state)};
  auto state = PacketCodec::encode_feedback_state(feedback.values, *profile_,
                                                  config_.envelope);
  if (!state) return unexpected(ClientError::packet_encode_failure);
  auto assembled = PacketCodec::assemble_feedback_state_packet(
      state.value(),
      FeedbackStatePacketHeaderInput{
          .sequence = feedback.sequence,
          .extended_header =
              profile_->uses_extended_header(type)
                  ? std::optional<ExtendedPacketHeader>{next_extended_header()}
                  : std::nullopt,
      },
      *profile_, config_.envelope);
  if (!assembled ||
      assembled.value().datagram_size > config_.limits.maximum_fragment_size)
    return unexpected(ClientError::packet_encode_failure);

  auto& packet = assembled.value();
  auto reservation =
      protect(packet.encoded_packet, PacketFamily::feedback_state,
              packet.state_payload_offset, packet.state_payload_size,
              packet.state_payload_size + takion_crypto_block_size);
  if (!reservation) return unexpected(reservation.error());
  return without_datagram(transmit(packet.encoded_packet,
                                   std::move(reservation).value(),
                                   SendClass::best_effort));
}

ClientResult<void> ClientOutbound::send_feedback_history(
    const SendFeedbackHistory& feedback) {
  if (!profile_) return unexpected(ClientError::invalid_configuration);

  const PacketType type{
      static_cast<std::uint8_t>(PacketFamily::feedback_history)};
  FeedbackHistoryValues values{
      .sequence = feedback.sequence,
      .events = feedback.events,
      .extended_header =
          profile_->uses_extended_header(type)
              ? std::optional<ExtendedPacketHeader>{next_extended_header()}
              : std::nullopt,
      .controller_id = feedback.controller_id,
      .event_controller_ids = feedback.event_controller_ids,
  };
  auto history = PacketCodec::encode_feedback_history_packet(values, *profile_,
                                                             config_.envelope);
  if (!history ||
      history.value().datagram_size > config_.limits.maximum_fragment_size)
    return unexpected(ClientError::packet_encode_failure);

  auto& packet = history.value();
  auto reservation =
      protect(packet.encoded_packet, PacketFamily::feedback_history,
              packet.event_payload_offset, packet.event_payload_size,
              packet.event_payload_size + takion_crypto_block_size);
  if (!reservation) return unexpected(reservation.error());
  return without_datagram(transmit(packet.encoded_packet,
                                   std::move(reservation).value(),
                                   SendClass::best_effort));
}

ClientResult<void> ClientOutbound::send_microphone(
    const SendMicrophone& microphone) {
  if (!profile_) return unexpected(ClientError::invalid_configuration);
  if (microphone.payload_offset == 0 ||
      microphone.payload_offset > microphone.packet.size())
    return unexpected(ClientError::packet_encode_failure);

  const PacketType type{
      std::to_integer<std::uint8_t>(microphone.packet.front())};
  if (type.family() != PacketFamily::audio_and_haptics)
    return unexpected(ClientError::packet_encode_failure);
  std::optional<ExtendedPacketHeader> extended_header;
  if (profile_->uses_extended_header(type)) {
    extended_header = next_extended_header();
  }
  auto plain = PacketCodec::encode_plain_packet(
      type, extended_header,
      std::span<const std::byte>{microphone.packet}.subspan(1), *profile_);
  if (!plain) return unexpected(ClientError::packet_encode_failure);
  if (plain.value().size() > config_.limits.maximum_fragment_size)
    return unexpected(ClientError::packet_encode_failure);

  const auto payload_offset =
      profile_->packet_header_size(type) + microphone.payload_offset - 1;
  const auto payload_size =
      microphone.packet.size() - microphone.payload_offset;
  auto reservation = protect(
      plain.value().span(), PacketFamily::audio_and_haptics, payload_offset,
      payload_size, payload_size + takion_crypto_block_size);
  if (!reservation) return unexpected(reservation.error());
  return without_datagram(transmit(plain.value().bytes(),
                                   std::move(reservation).value(),
                                   SendClass::best_effort));
}

ClientResult<void> ClientOutbound::send_length_prefixed(
    std::span<const std::byte> payload) {
  if (!profile_) return unexpected(ClientError::invalid_configuration);
  auto body = TypedMessageCodec::encode_length_prefixed_body(payload);
  if (!body) return unexpected(ClientError::packet_encode_failure);
  const auto datagram_size = numeric::checked_add(
      EnvelopeCodec::overhead(config_.envelope) + packet_type_size,
      body.value().size());
  if (!datagram_size || *datagram_size > config_.limits.maximum_fragment_size)
    return unexpected(ClientError::packet_encode_failure);
  return send_plain(PacketFamily::length_prefixed, body.value(), true);
}

ClientResult<void> ClientOutbound::send_trace(std::uint32_t sequence) {
  if (!profile_) return unexpected(ClientError::invalid_configuration);
  if (!TypedMessageCodec::trace_messages_enabled(*profile_))
    return unexpected(ClientError::packet_encode_failure);
  const bool authenticated = config_.crypto_policy == CryptoPolicy::required;
  if (authenticated && !crypto_installed_)
    return unexpected(ClientError::crypto_not_installed);
  PlainPacket packet;
  packet.type.raw = static_cast<std::uint8_t>(PacketFamily::trace);
  if (profile_->uses_extended_header(packet.type))
    packet.extended_header = next_extended_header();
  auto body =
      TypedMessageCodec::encode_trace_message(TraceValues{sequence, 0, 0});
  if (!body) return unexpected(ClientError::packet_encode_failure);
  packet.payload = body.value();
  auto plain = PacketCodec::encode_plain_packet(packet, *profile_);
  if (!plain ||
      plain.value().size() + EnvelopeCodec::overhead(config_.envelope) >
          config_.limits.maximum_fragment_size)
    return unexpected(ClientError::packet_encode_failure);
  if (!authenticated)
    return without_datagram(
        transmit(plain.value().bytes(), std::nullopt, SendClass::essential));

  auto authentication = crypto_.authenticate_local_detached(
      plain.value().bytes(), trace_key_advance);
  if (!authentication) return unexpected(ClientError::crypto_failure);
  auto reservation = std::move(authentication.value().reservation);
  auto final_body = TypedMessageCodec::encode_trace_message(TraceValues{
      sequence, TypedMessageCodec::trace_tag_value(authentication.value().mac),
      static_cast<std::uint32_t>(reservation.key_position())});
  if (!final_body) return unexpected(ClientError::packet_encode_failure);
  packet.payload = final_body.value();
  auto final_plain = PacketCodec::encode_plain_packet(packet, *profile_);
  if (!final_plain ||
      final_plain.value().size() + EnvelopeCodec::overhead(config_.envelope) >
          config_.limits.maximum_fragment_size)
    return unexpected(ClientError::packet_encode_failure);
  return without_datagram(transmit(final_plain.value().bytes(),
                                   std::move(reservation),
                                   SendClass::essential));
}

ClientResult<void> ClientOutbound::send_generic_data_chunk(
    const GenericDataChunk& chunk) {
  if (!profile_) return unexpected(ClientError::invalid_configuration);
  if (!TypedMessageCodec::generic_data_enabled(*profile_))
    return unexpected(ClientError::packet_encode_failure);
  GenericDataChunk unprotected{{},
                               0,
                               chunk.transfer_id,
                               chunk.packet_index,
                               chunk.transfer_offset,
                               chunk.payload.clone()};
  auto body =
      TypedMessageCodec::encode_generic_data_chunk_body(unprotected, *profile_);
  if (!body) return unexpected(ClientError::packet_encode_failure);
  const PacketType type{static_cast<std::uint8_t>(PacketFamily::generic_data)};
  const auto datagram_size =
      numeric::checked_add(EnvelopeCodec::overhead(config_.envelope) +
                               profile_->packet_header_size(type),
                           body.value().size());
  if (!datagram_size || *datagram_size > config_.limits.maximum_fragment_size)
    return unexpected(ClientError::packet_encode_failure);
  const auto key_advance =
      numeric::checked_add(chunk.payload.size(), takion_crypto_block_size);
  if (!key_advance) return unexpected(ClientError::packet_encode_failure);
  return without_datagram(
      send_packet(PacketFamily::generic_data, body.value().bytes(), false,
                  *key_advance, generic_data_header_size - packet_type_size));
}

std::size_t ClientOutbound::control_wire_size(
    std::size_t payload_size) const noexcept {
  return packet_type_size + payload_size +
         EnvelopeCodec::overhead(config_.envelope);
}

ClientResult<SecretBytes> ClientOutbound::transmit(
    std::span<const std::byte> wire, Reservation reservation,
    SendClass send_class) {
  auto datagram = EnvelopeCodec::encode(wire, config_.envelope);
  if (!datagram) return unexpected(ClientError::packet_encode_failure);
  const auto offered = offer(datagram.value().bytes());
  if (!offered) return unexpected(offered.error());
  if (offered.value() == Offer::refused && !make_room(send_class))
    return unexpected(ClientError::send_blocked);
  if (reservation) {
    auto committed = reservation->commit();
    if (!committed) return unexpected(ClientError::crypto_failure);
  }
  if (offered.value() == Offer::refused)
    keep_unsent(datagram.value().clone(), send_class);
  return std::move(datagram).value();
}

}  // namespace libchiaking::net::protocol::takion::client_detail
