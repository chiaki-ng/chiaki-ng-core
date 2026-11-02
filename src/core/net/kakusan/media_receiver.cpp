#include "src/core/net/kakusan/media_receiver.hpp"

#include "src/core/utils/timing/deadline.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace libchiaking::net::kakusan {

auto MediaReceiver::SequenceWindow::contains(
    std::uint64_t sequence) const noexcept -> bool {
  return started_ && sequence <= largest_ &&
         largest_ - sequence < sequence_window_size;
}

auto MediaReceiver::SequenceWindow::slot(std::uint64_t sequence) const noexcept
    -> Slot {
  return contains(sequence) ? slots_[slot_index(sequence)] : Slot::empty;
}

void MediaReceiver::SequenceWindow::advance_to(
    std::uint64_t sequence) noexcept {
  if (!started_) {
    slots_.fill(Slot::empty);
    largest_ = sequence;
    started_ = true;
    return;
  }
  if (sequence <= largest_) return;
  const auto shift = sequence - largest_;
  if (shift >= sequence_window_size) {
    slots_.fill(Slot::empty);
  } else {
    for (std::uint64_t step = 1; step <= shift; ++step)
      slots_[slot_index(largest_ + step)] = Slot::empty;
  }
  largest_ = sequence;
}

void MediaReceiver::SequenceWindow::set(std::uint64_t sequence,
                                        Slot value) noexcept {
  if (contains(sequence)) slots_[slot_index(sequence)] = value;
}

void MediaReceiver::SequenceWindow::clear() noexcept {
  slots_.fill(Slot::empty);
  largest_ = 0;
  started_ = false;
}

MediaReceiver::MediaReceiver(platform::CryptoProvider& crypto, MediaKey key,
                             const ConnectionId& connection,
                             MediaReceiverLimits limits) noexcept
    : crypto_(crypto),
      key_(std::move(key)),
      connection_(connection),
      limits_{std::min(limits.incomplete_packets, max_incomplete_packets),
              std::min(limits.reassembly_bytes, max_reassembly_bytes),
              std::min(limits.reassembly_age_us, max_reassembly_age_us)},
      ack_tokens_(ack_tokens_per_second, ack_token_capacity, 0) {}

auto MediaReceiver::incomplete_packets() const noexcept -> std::size_t {
  return assemblies_.size();
}

auto MediaReceiver::reassembly_bytes() const noexcept -> std::size_t {
  return reassembly_bytes_;
}

auto MediaReceiver::receive(std::span<const std::byte> datagram,
                            std::uint64_t now_us)
    -> Result<std::optional<SourcePacket>> {
  if (!TransportFrameCodec::media_matches(datagram, connection_))
    return std::optional<SourcePacket>{};
  const auto authentic =
      TransportFrameCodec::authenticate_media(crypto_, key_, datagram);
  if (!authentic) return fail(authentic.error());
  if (!authentic.value()) return std::optional<SourcePacket>{};
  const auto decoded = TransportFrameCodec::decode_media(datagram);
  if (!decoded) return fail(decoded.error());
  const auto& media = decoded.value();
  const auto& header = media.header;
  const bool probe = header.kind == MediaKind::probe;
  if (header.datagram_sequence == counter_limit ||
      (!probe && header.packet_sequence == counter_limit))
    return fail(Error::protocol);

  const auto sequence = header.datagram_sequence;
  const bool fresh = !datagrams_.started() || sequence > datagrams_.largest();
  if (!fresh && (!datagrams_.contains(sequence) ||
                 datagrams_.slot(sequence) != Slot::empty))
    return std::optional<SourcePacket>{};
  note_datagram(sequence, now_us);
  if (probe) return std::optional<SourcePacket>{};
  expire(now_us);
  return reassemble(media, now_us);
}

void MediaReceiver::note_datagram(std::uint64_t sequence,
                                  std::uint64_t now_us) noexcept {
  const bool first = !datagrams_.started();
  const bool newest = first || sequence > datagrams_.largest();
  bool gap = true;
  if (first)
    gap = sequence != 0;
  else if (newest)
    gap = sequence - datagrams_.largest() > 1;
  if (newest) largest_arrival_us_ = now_us;
  datagrams_.advance_to(sequence);
  datagrams_.set(sequence, Slot::marked);

  unacknowledged_ = std::min(unacknowledged_ + 1, ack_packet_threshold);
  if (unacknowledged_ == 1)
    ack_due_us_ = timing::Deadline::after_us(now_us, max_ack_delay_us);
  if (gap || unacknowledged_ >= ack_packet_threshold)
    ack_due_us_ = std::min(*ack_due_us_, now_us);
}

auto MediaReceiver::reassemble(const MediaDatagram& media, std::uint64_t now_us)
    -> Result<std::optional<SourcePacket>> {
  const auto sequence = media.header.packet_sequence;
  if (!packets_.started() || sequence > packets_.largest()) {
    packets_.advance_to(sequence);
    evict_outside_window();
  }
  if (!packets_.contains(sequence)) return std::optional<SourcePacket>{};
  if (const auto index = find(sequence)) return merge(*index, media);
  if (packets_.slot(sequence) == Slot::completed) return fail(Error::protocol);
  if (packets_.slot(sequence) == Slot::marked)
    return std::optional<SourcePacket>{};
  return admit(media, now_us);
}

auto MediaReceiver::admit(const MediaDatagram& media, std::uint64_t now_us)
    -> std::optional<SourcePacket> {
  const auto& header = media.header;
  const std::size_t size = header.original_size;
  if (TransportFrameCodec::media_fragment_count(size) == 1) {
    packets_.set(header.packet_sequence, Slot::completed);
    return SourcePacket{
        header.kind, header.key_position,
        std::vector<std::byte>(media.fragment.begin(), media.fragment.end())};
  }
  while (!assemblies_.empty() &&
         (assemblies_.size() >= limits_.incomplete_packets ||
          reassembly_bytes_ + size > limits_.reassembly_bytes))
    evict(0);
  if (limits_.incomplete_packets == 0 || size > limits_.reassembly_bytes) {
    packets_.set(header.packet_sequence, Slot::marked);
    return std::nullopt;
  }
  Assembly assembly;
  assembly.packet_sequence = header.packet_sequence;
  assembly.kind = header.kind;
  assembly.key_position = header.key_position;
  assembly.original_size = header.original_size;
  assembly.first_us = now_us;
  assembly.expected =
      (std::uint32_t{1} << TransportFrameCodec::media_fragment_count(size)) - 1;
  assembly.buffer.assign(size, std::byte{});
  assemblies_.push_back(std::move(assembly));
  reassembly_bytes_ += size;
  auto merged = merge(assemblies_.size() - 1, media);
  return std::move(merged).value();
}

auto MediaReceiver::merge(std::size_t index, const MediaDatagram& media)
    -> Result<std::optional<SourcePacket>> {
  const auto& header = media.header;
  auto& assembly = assemblies_[index];
  if (assembly.kind != header.kind ||
      assembly.key_position != header.key_position ||
      assembly.original_size != header.original_size)
    return fail(Error::protocol);
  const auto fragment =
      static_cast<std::uint32_t>(header.fragment_offset / media_fragment_size);
  const auto bit = std::uint32_t{1} << fragment;
  if ((assembly.present & bit) != 0) return fail(Error::protocol);
  std::copy(media.fragment.begin(), media.fragment.end(),
            assembly.buffer.begin() + header.fragment_offset);
  assembly.present |= bit;
  if (assembly.present != assembly.expected)
    return std::optional<SourcePacket>{};

  SourcePacket packet{assembly.kind, assembly.key_position,
                      std::move(assembly.buffer)};
  reassembly_bytes_ -= assembly.original_size;
  packets_.set(assembly.packet_sequence, Slot::completed);
  assemblies_.erase(assemblies_.begin() + static_cast<std::ptrdiff_t>(index));
  return std::optional<SourcePacket>{std::move(packet)};
}

auto MediaReceiver::find(std::uint64_t packet_sequence) const noexcept
    -> std::optional<std::size_t> {
  for (std::size_t index = 0; index < assemblies_.size(); ++index) {
    if (assemblies_[index].packet_sequence == packet_sequence) return index;
  }
  return std::nullopt;
}

void MediaReceiver::evict(std::size_t index) noexcept {
  reassembly_bytes_ -= assemblies_[index].original_size;
  packets_.set(assemblies_[index].packet_sequence, Slot::marked);
  assemblies_.erase(assemblies_.begin() + static_cast<std::ptrdiff_t>(index));
}

void MediaReceiver::evict_outside_window() noexcept {
  for (std::size_t index = 0; index < assemblies_.size();) {
    if (packets_.contains(assemblies_[index].packet_sequence))
      ++index;
    else
      evict(index);
  }
}

void MediaReceiver::expire(std::uint64_t now_us) noexcept {
  for (std::size_t index = 0; index < assemblies_.size();) {
    if (timing::Deadline::elapsed_us(assemblies_[index].first_us, now_us) >=
        limits_.reassembly_age_us)
      evict(index);
    else
      ++index;
  }
}

auto MediaReceiver::build_ack(std::uint64_t now_us) const
    -> std::optional<std::vector<std::byte>> {
  MediaAck ack;
  ack.largest = datagrams_.largest();
  ack.delay_us = static_cast<std::uint32_t>(std::min<std::uint64_t>(
      timing::Deadline::elapsed_us(largest_arrival_us_, now_us),
      std::numeric_limits<std::uint32_t>::max()));

  const auto depth =
      std::min<std::uint64_t>(sequence_window_size - 1, ack.largest);
  bool running = false;
  std::uint64_t high = 0;
  std::uint64_t low = 0;
  bool full = false;
  for (std::uint64_t distance = 0; distance <= depth && !full; ++distance) {
    const auto sequence = ack.largest - distance;
    if (datagrams_.slot(sequence) != Slot::empty) {
      if (!running) {
        running = true;
        high = sequence;
      }
      low = sequence;
    } else if (running) {
      ack.ranges.push_back(AckRange{low, high});
      running = false;
      full = ack.ranges.size() == max_ack_ranges;
    }
  }
  if (running && !full) ack.ranges.push_back(AckRange{low, high});

  auto encoded = TransportFrameCodec::encode_media_ack(ack);
  if (!encoded) return std::nullopt;
  return std::move(encoded).value();
}

auto MediaReceiver::poll(std::uint64_t now_us)
    -> std::optional<std::vector<std::byte>> {
  expire(now_us);
  if (!ack_due_us_ || now_us < *ack_due_us_) return std::nullopt;
  ack_tokens_.refill(now_us);
  if (!ack_tokens_.holds(1)) return std::nullopt;
  auto record = build_ack(now_us);
  if (!record) return std::nullopt;
  static_cast<void>(ack_tokens_.take(1));
  unacknowledged_ = 0;
  ack_due_us_.reset();
  return record;
}

auto MediaReceiver::next_deadline_us() const noexcept
    -> std::optional<std::uint64_t> {
  std::optional<std::uint64_t> deadline;
  if (ack_due_us_) {
    auto ready = *ack_due_us_;
    if (!ack_tokens_.holds(1))
      ready = std::max(ready, ack_tokens_.ready_at_us(1));
    deadline = ready;
  }
  for (const auto& assembly : assemblies_) {
    const auto expiry = timing::Deadline::after_us(assembly.first_us,
                                                   limits_.reassembly_age_us);
    deadline = deadline ? std::min(*deadline, expiry) : expiry;
  }
  return deadline;
}

}  // namespace libchiaking::net::kakusan
