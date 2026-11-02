#include "src/core/net/kakusan/control_channel.hpp"

#include "src/core/net/kakusan/transport_frame.hpp"
#include "src/core/utils/json/strict.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <variant>

namespace libchiaking::net::kakusan {

ControlChannel::ControlChannel(std::uint64_t now_us,
                               std::uint32_t first_message_id)
    : send_logical_(control_send_rate, control_send_burst, now_us),
      send_bytes_(control_send_byte_rate, control_send_byte_burst, now_us),
      receive_logical_(control_receive_rate, control_receive_burst, now_us),
      receive_bytes_(control_receive_byte_rate, control_receive_byte_burst,
                     now_us),
      last_us_(now_us),
      next_message_id_(first_message_id == 0 ? 1 : first_message_id) {}

void ControlChannel::refill(std::uint64_t now_us) noexcept {
  last_us_ = std::max(last_us_, now_us);
  send_logical_.refill(last_us_);
  send_bytes_.refill(last_us_);
  receive_logical_.refill(last_us_);
  receive_bytes_.refill(last_us_);
}

auto ControlChannel::next_id_available() const noexcept -> bool {
  return next_message_id_ <= max_message_id;
}

auto ControlChannel::outstanding_logical() const noexcept -> std::size_t {
  return queue_.size() + (in_flight_ ? 1U : 0U);
}

auto ControlChannel::enqueue(const control::Message& message,
                             std::uint64_t now_us) -> Result<ControlTicket> {
  refill(now_us);
  if (closing_) return fail(Error::protocol);
  auto encoded = control::MessageCodec::encode_secret(message);
  if (!encoded) return fail(encoded.error());
  if (encoded.value().empty() ||
      encoded.value().size() > max_control_message_size)
    return fail(Error::protocol);

  Kind kind = Kind::ordinary;
  if (std::holds_alternative<control::Ping>(message))
    kind = Kind::ping;
  else if (std::holds_alternative<control::SourceIdr>(message))
    kind = Kind::source_idr;

  if (kind != Kind::ordinary) {
    for (auto& pending : queue_) {
      if (pending.kind == kind) {
        pending.body = SecretBytes{encoded.value().bytes()};
        return pending.ticket;
      }
    }
  }

  if (outstanding_logical() >= control_queue_capacity)
    return fail(Error::protocol);
  if (next_message_id_ + queue_.size() > max_message_id)
    return fail(Error::protocol);

  const ControlTicket ticket{next_ticket_++};
  queue_.push_back(
      Pending{ticket, kind, false, SecretBytes{encoded.value().bytes()}});
  return ticket;
}

auto ControlChannel::send_terminal(const control::Message& message,
                                   std::uint64_t now_us) -> bool {
  refill(now_us);
  if (closing_ || !next_id_available()) return false;
  auto encoded = control::MessageCodec::encode_secret(message);
  if (!encoded || encoded.value().empty() ||
      encoded.value().size() > max_control_message_size)
    return false;
  queue_.clear();
  queue_.push_back(Pending{ControlTicket{0}, Kind::ordinary, true,
                           SecretBytes{encoded.value().bytes()}});
  closing_ = true;
  return true;
}

auto ControlChannel::begin_message() -> Result<void> {
  if (!next_id_available()) return fail(Error::protocol);
  const auto id = static_cast<std::uint32_t>(next_message_id_);
  auto fragments = TransportFrameCodec::encode_control_fragments(
      id, queue_.front().body.span());
  if (!fragments) return fail(fragments.error());
  InFlight flight;
  flight.id = id;
  flight.ticket = queue_.front().ticket;
  flight.terminal = queue_.front().terminal;
  flight.fragments = std::move(fragments.value());
  in_flight_ = std::move(flight);
  queue_.pop_front();
  ++next_message_id_;
  return {};
}

void ControlChannel::pump_fragments(
    std::uint64_t now_us, std::vector<std::vector<std::byte>>& records) {
  auto& flight = *in_flight_;
  while (flight.next_fragment < flight.fragments.size()) {
    auto& fragment = flight.fragments[flight.next_fragment];
    if (!send_bytes_.take(fragment.size())) return;
    records.push_back(fragment);
    if (!flight.first_sent) {
      flight.first_sent = true;
      flight.first_sent_us = now_us;
    }
    ++flight.next_fragment;
  }
  flight.delivered_once = true;
  flight.retransmit_at_us =
      timing::Deadline::after_us(now_us, flight.interval_us);
  flight.interval_us =
      std::min(flight.interval_us * 2, control_retransmit_max_us);
}

auto ControlChannel::receive_expired(std::uint64_t now_us) const noexcept
    -> bool {
  return reassembly_ &&
         now_us >= timing::Deadline::after_us(reassembly_->started_us,
                                              control_failure_deadline_us);
}

auto ControlChannel::poll(std::uint64_t now_us,
                          std::vector<std::vector<std::byte>>& records)
    -> Result<void> {
  refill(now_us);
  now_us = last_us_;
  if (in_flight_ && in_flight_->first_sent &&
      now_us >= timing::Deadline::after_us(in_flight_->first_sent_us,
                                           control_failure_deadline_us))
    return fail(Error::timeout);
  if (receive_expired(now_us)) return fail(Error::timeout);

  while (!pending_acks_.empty()) {
    if (!send_bytes_.take(control_header_size)) break;
    const auto ack =
        TransportFrameCodec::encode_control_ack(pending_acks_.front());
    records.emplace_back(ack.begin(), ack.end());
    pending_acks_.erase(pending_acks_.begin());
  }

  if (!in_flight_ && !queue_.empty() && send_logical_.take(1)) {
    const auto started = begin_message();
    if (!started) return started;
  }

  if (in_flight_) {
    auto& flight = *in_flight_;
    if (flight.delivered_once &&
        flight.next_fragment == flight.fragments.size() &&
        now_us >= flight.retransmit_at_us)
      flight.next_fragment = 0;
    if (flight.next_fragment < flight.fragments.size())
      pump_fragments(now_us, records);
  }
  return {};
}

void ControlChannel::queue_ack(std::uint32_t id) {
  if (std::find(pending_acks_.begin(), pending_acks_.end(), id) ==
      pending_acks_.end())
    pending_acks_.push_back(id);
}

auto ControlChannel::receive_ack(std::uint32_t id) -> Result<void> {
  if (id >= next_message_id_) return fail(Error::protocol);
  if (!in_flight_ || in_flight_->id != id) return {};
  if (!in_flight_->delivered_once) return fail(Error::protocol);
  if (!in_flight_->terminal) acknowledged_.push_back(in_flight_->ticket);
  in_flight_.reset();
  return {};
}

auto ControlChannel::receive_duplicate(std::uint16_t index, std::uint16_t count,
                                       std::span<const std::byte> payload)
    -> Result<void> {
  if (!completed_ || completed_->count != count) return fail(Error::protocol);
  const auto offset = static_cast<std::size_t>(index) * control_fragment_size;
  if (offset >= completed_->bytes.size()) return fail(Error::protocol);
  const auto length =
      std::min(control_fragment_size, completed_->bytes.size() - offset);
  const auto expected = completed_->bytes.span().subspan(offset, length);
  if (!std::equal(expected.begin(), expected.end(), payload.begin(),
                  payload.end()))
    return fail(Error::protocol);
  queue_ack(static_cast<std::uint32_t>(expected_id_ - 1));
  return {};
}

auto ControlChannel::complete_message(std::uint32_t id)
    -> Result<std::optional<control::Message>> {
  SecretBytes bytes;
  for (const auto& part : reassembly_->parts) bytes.append(part->span());
  if (bytes.empty() || bytes.size() > max_control_message_size)
    return fail(Error::protocol);
  const auto text = SecretString{bytes.span()};
  const auto parsed = json::strict::Value::parse(text.view());
  if (!parsed || parsed->object() == nullptr) return fail(Error::protocol);

  queue_ack(id);
  completed_ = Completed{reassembly_->count, std::move(bytes)};
  ++expected_id_;
  reassembly_.reset();

  auto message = control::MessageCodec::decode(*parsed);
  if (!message) return fail(message.error());
  return std::optional<control::Message>{std::move(message.value())};
}

auto ControlChannel::receive_data(std::uint32_t id, std::uint16_t index,
                                  std::uint16_t count,
                                  std::span<const std::byte> payload,
                                  std::uint64_t now_us)
    -> Result<std::optional<control::Message>> {
  if (id > expected_id_) return fail(Error::protocol);
  if (static_cast<std::uint64_t>(id) + 1 == expected_id_) {
    const auto matched = receive_duplicate(index, count, payload);
    if (!matched) return fail(matched.error());
    return std::optional<control::Message>{};
  }
  if (id < expected_id_) return std::optional<control::Message>{};

  if (!reassembly_) {
    if (!receive_logical_.take(1)) return fail(Error::protocol);
    Reassembly fresh;
    fresh.count = count;
    fresh.started_us = now_us;
    fresh.parts.resize(count);
    reassembly_ = std::move(fresh);
  }
  auto& assembly = *reassembly_;
  if (assembly.count != count) return fail(Error::protocol);
  auto& slot = assembly.parts[index];
  if (slot) {
    if (!std::ranges::equal(slot->span(), payload))
      return fail(Error::protocol);
    return std::optional<control::Message>{};
  }
  slot = SecretBytes{payload};
  ++assembly.received;
  if (assembly.received < assembly.count)
    return std::optional<control::Message>{};
  return complete_message(id);
}

auto ControlChannel::receive(std::span<const std::byte> record,
                             std::uint64_t now_us)
    -> Result<std::optional<control::Message>> {
  refill(now_us);
  now_us = last_us_;
  if (receive_expired(now_us)) return fail(Error::timeout);
  if (!receive_bytes_.take(record.size())) return fail(Error::protocol);
  const auto fragment = TransportFrameCodec::decode_control(record);
  if (!fragment) return fail(Error::protocol);
  const auto& frame = fragment.value();
  if (frame.kind == ControlKind::ack) {
    const auto acknowledged = receive_ack(frame.message_id);
    if (!acknowledged) return fail(acknowledged.error());
    return std::optional<control::Message>{};
  }
  return receive_data(frame.message_id, frame.index, frame.count, frame.payload,
                      now_us);
}

auto ControlChannel::take_acknowledged() -> std::optional<ControlTicket> {
  if (acknowledged_.empty()) return std::nullopt;
  const auto ticket = acknowledged_.front();
  acknowledged_.pop_front();
  return ticket;
}

auto ControlChannel::next_deadline_us() const noexcept
    -> std::optional<std::uint64_t> {
  timing::Deadline earliest;
  if (reassembly_)
    earliest.consider(timing::Deadline::after_us(reassembly_->started_us,
                                                 control_failure_deadline_us));
  if (!pending_acks_.empty())
    earliest.consider(send_bytes_.ready_at_us(control_header_size));
  if (in_flight_) {
    const auto& flight = *in_flight_;
    if (flight.first_sent)
      earliest.consider(timing::Deadline::after_us(
          flight.first_sent_us, control_failure_deadline_us));
    if (flight.next_fragment < flight.fragments.size())
      earliest.consider(send_bytes_.ready_at_us(
          flight.fragments[flight.next_fragment].size()));
    else if (flight.delivered_once)
      earliest.consider(flight.retransmit_at_us);
  } else if (!queue_.empty()) {
    earliest.consider(send_logical_.ready_at_us(1));
  }
  return earliest.at_us();
}

auto ControlChannel::idle() const noexcept -> bool {
  return queue_.empty() && !in_flight_ && pending_acks_.empty();
}

}  // namespace libchiaking::net::kakusan
