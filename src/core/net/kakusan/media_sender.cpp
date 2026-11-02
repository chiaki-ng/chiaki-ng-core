#include "src/core/net/kakusan/media_sender.hpp"

#include "src/core/net/kakusan/digest.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace libchiaking::net::kakusan {

auto MediaTuning::bounded() const noexcept -> MediaTuning {
  auto tuning = *this;
  tuning.queue_age_min_us =
      std::max(tuning.queue_age_min_us, media_queue_age_floor_us);
  tuning.queue_age_limit_us =
      std::max(tuning.queue_age_limit_us, tuning.queue_age_min_us);
  tuning.queue_age_round_trips =
      std::max<std::uint32_t>(tuning.queue_age_round_trips, 1);
  tuning.drop_windows_to_fail =
      std::max<std::uint64_t>(tuning.drop_windows_to_fail, 1);
  tuning.progress_timeout_us =
      std::max(tuning.progress_timeout_us, media_progress_timeout_floor_us);
  tuning.queue_bytes = std::max(tuning.queue_bytes, max_source_datagram_size);
  tuning.queue_packets = std::max<std::size_t>(tuning.queue_packets, 1);
  return tuning;
}

MediaSender::MediaSender(platform::CryptoProvider& crypto, MediaKey key,
                         const ConnectionId& connection, std::uint64_t now_us,
                         const MediaTuning& tuning)
    : crypto_(crypto),
      key_(std::move(key)),
      connection_(connection),
      tuning_(tuning.bounded()),
      pacer_(now_us),
      start_us_(now_us),
      now_us_(now_us) {}

auto MediaSender::observe(std::uint64_t now_us) noexcept -> std::uint64_t {
  now_us_ = std::max(now_us_, now_us);
  return now_us_;
}

auto MediaSender::stop(Error error) noexcept -> Unexpected<Error> {
  if (!failure_) failure_ = error;
  queue_.clear();
  queued_bytes_ = 0;
  return fail(*failure_);
}

auto MediaSender::progress_deadline_us() const noexcept
    -> std::optional<std::uint64_t> {
  if (!progress_anchor_us_ || controller_.bytes_in_flight() == 0)
    return std::nullopt;
  return timing::Deadline::after_us(*progress_anchor_us_,
                                    tuning_.progress_timeout_us);
}

auto MediaSender::progress_expired(std::uint64_t now_us) noexcept -> bool {
  const auto deadline = progress_deadline_us();
  return deadline && now_us >= *deadline;
}

auto MediaSender::queue_age_us() const noexcept -> std::uint64_t {
  const auto scaled = static_cast<std::uint64_t>(
      static_cast<double>(tuning_.queue_age_round_trips) *
      controller_.smoothed_rtt_us());
  return std::clamp(scaled, tuning_.queue_age_min_us,
                    tuning_.queue_age_limit_us);
}

auto MediaSender::report() const noexcept -> MediaSenderReport {
  MediaSenderReport report;
  report.stop = stop_;
  report.smoothed_rtt_us =
      static_cast<std::uint64_t>(controller_.smoothed_rtt_us());
  report.floor_rtt_us =
      static_cast<std::uint64_t>(controller_.delay_floor_us());
  report.bytes_in_flight = controller_.bytes_in_flight();
  report.lost_datagrams = controller_.lost_packets();
  report.queue_age_us = queue_age_us();
  report.datagrams_sent = datagrams_sent_;
  report.acks = acks_;
  report.source_packets = next_packet_sequence_;
  report.dropped_packets = dropped_packets_;
  report.dropped_video = dropped_video_;
  report.rtt_sampled = controller_.has_rtt_sample();
  return report;
}

auto MediaSender::oldest_queued_us() const noexcept
    -> std::optional<std::uint64_t> {
  if (queue_.empty()) return std::nullopt;
  return queue_.front().queued_us;
}

void MediaSender::note_drop(MediaKind kind, DropCause cause,
                            std::uint64_t now_us) noexcept {
  ++dropped_packets_;
  if (kind == MediaKind::video) {
    ++dropped_video_;
    if (flow_ == VideoFlow::forwarding) idr_requested_ = true;
  }
  auto* window = drop_window_at(now_us);
  if (window == nullptr) return;
  switch (cause) {
    case DropCause::queue:
      ++window->dropped;
      break;
    case DropCause::path:
      ++window->shed;
      break;
  }
}

auto MediaSender::drop_window_at(std::uint64_t now_us) noexcept -> DropWindow* {
  const auto running = timing::Deadline::elapsed_us(start_us_, now_us);
  if (running < tuning_.startup_grace_us) return nullptr;
  const auto index = running / media_drop_window_us;
  if (drop_window_ && drop_window_->index == index) return &*drop_window_;
  if (drop_window_ && drop_window_->lossy())
    ++lossy_windows_;
  else
    lossy_windows_ = 0;
  if (lossy_windows_ >= tuning_.drop_windows_to_fail && !failure_) {
    stop_ = MediaStop::drop_windows;
    failure_ = Error::congestion;
  }
  if (drop_window_ && drop_window_->index + 1 != index) lossy_windows_ = 0;
  drop_window_ = DropWindow{index, 0, 0, 0};
  return &*drop_window_;
}

void MediaSender::drop_front(std::uint64_t now_us) {
  note_drop(queue_.front().kind, DropCause::queue, now_us);
  queued_bytes_ -= queue_.front().bytes.size();
  queue_.pop_front();
}

void MediaSender::expire(std::uint64_t now_us) {
  const auto limit = queue_age_us();
  while (!queue_.empty() &&
         timing::Deadline::elapsed_us(queue_.front().queued_us, now_us) > limit)
    drop_front(now_us);
}

void MediaSender::OfferedRate::add(std::size_t size,
                                   std::uint64_t now_us) noexcept {
  const auto index = now_us / media_rate_window_us;
  if (index != window) {
    bytes_per_us = media_rate_retain * bytes_per_us +
                   (1.0 - media_rate_retain) * static_cast<double>(bytes) /
                       static_cast<double>(media_rate_window_us);
    const auto idle = std::min(index - window - 1, media_rate_idle_windows);
    for (std::uint64_t skipped = 0; skipped < idle; ++skipped)
      bytes_per_us *= media_rate_retain;
    window = index;
    bytes = 0;
  }
  bytes += size;
}

void MediaSender::follow_path(std::uint64_t now_us) {
  const auto path = controller_.path();
  if (flow_ == VideoFlow::forwarding && path == PathState::congested) {
    flow_ = VideoFlow::shedding;
    std::erase_if(queue_, [&](const Queued& queued) {
      if (queued.kind != MediaKind::video) return false;
      note_drop(queued.kind, DropCause::path, now_us);
      queued_bytes_ -= queued.bytes.size();
      return true;
    });
  } else if (flow_ == VideoFlow::shedding && path == PathState::clear) {
    flow_ = VideoFlow::forwarding;
    idr_requested_ = true;
  }
}

void MediaSender::pace(std::uint64_t now_us) noexcept {
  if (queue_.empty()) burst_bytes_ = 0;
  const auto drain = static_cast<double>(queue_age_us()) * media_drain_fraction;
  pacer_.set_rate(
      std::max({media_pace_floor, media_pace_headroom * offered_.bytes_per_us,
                static_cast<double>(burst_bytes_) / drain}),
      now_us);
}

auto MediaSender::submit(MediaKind kind, std::uint64_t key_position,
                         std::span<const std::byte> source_datagram,
                         std::uint64_t now_us) -> Result<void> {
  if (kind == MediaKind::probe || source_datagram.empty() ||
      source_datagram.size() > max_source_datagram_size)
    return fail(Error::protocol);
  if (stopped()) return {};
  now_us = observe(now_us);
  if (next_packet_sequence_ == std::numeric_limits<std::uint64_t>::max())
    return stop(Error::protocol);
  follow_path(now_us);
  expire(now_us);
  const bool shed = flow_ == VideoFlow::shedding && kind == MediaKind::video;
  while (!shed && !queue_.empty() &&
         (queued_bytes_ + source_datagram.size() > tuning_.queue_bytes ||
          queue_.size() >= tuning_.queue_packets))
    drop_front(now_us);
  if (failure_) return stop(*failure_);
  if (auto* window = drop_window_at(now_us)) ++window->offered;
  if (failure_) {
    static_cast<void>(stop(*failure_));
    return {};
  }
  if (shed) {
    ++next_packet_sequence_;
    note_drop(kind, DropCause::path, now_us);
    return {};
  }
  if (queue_.empty()) burst_bytes_ = 0;
  burst_bytes_ += source_datagram.size();
  offered_.add(source_datagram.size(), now_us);
  Queued queued;
  queued.kind = kind;
  queued.packet_sequence = next_packet_sequence_++;
  queued.key_position = key_position;
  queued.queued_us = now_us;
  queued.bytes.assign(source_datagram.begin(), source_datagram.end());
  queued_bytes_ += queued.bytes.size();
  queue_.push_back(std::move(queued));
  pacer_.set_idle(false, now_us);
  pace(now_us);
  return {};
}

void MediaSender::run_timers(std::uint64_t now_us) {
  for (int round = 0; round < media_max_timer_rounds; ++round) {
    const auto event = controller_.on_timeout(now_us);
    if (event != TimeoutEvent::loss) return;
  }
}

auto MediaSender::transmit(const MediaHeader& header,
                           std::span<const std::byte> fragment, bool probe,
                           std::uint64_t now_us,
                           std::vector<std::vector<std::byte>>& datagrams)
    -> Result<void> {
  auto encoded = TransportFrameCodec::encode_media(crypto_, key_, connection_,
                                                   header, fragment);
  if (!encoded) return stop(encoded.error());
  const auto size = encoded.value().size();
  if (controller_.bytes_in_flight() == 0) progress_anchor_us_ = now_us;
  const auto sent = controller_.on_packet_sent(size, probe, now_us);
  if (!sent) return stop(sent.error());
  pacer_.on_sent(size, now_us);
  ++datagrams_sent_;
  datagrams.push_back(std::move(encoded).value());
  return {};
}

auto MediaSender::poll(std::uint64_t now_us,
                       std::vector<std::vector<std::byte>>& datagrams)
    -> Result<void> {
  if (failure_) return fail(*failure_);
  now_us = observe(now_us);
  if (progress_expired(now_us)) {
    stop_ = MediaStop::ack_timeout;
    return stop(Error::congestion);
  }
  run_timers(now_us);
  follow_path(now_us);
  expire(now_us);
  pace(now_us);
  if (failure_) return stop(*failure_);

  if (controller_.probe_pending()) {
    if (controller_.exhausted()) return stop(Error::protocol);
    MediaHeader header;
    header.kind = MediaKind::probe;
    header.datagram_sequence = controller_.next_packet_number();
    const auto sent = transmit(header, {}, true, now_us, datagrams);
    if (!sent) return sent;
  }

  while (!queue_.empty()) {
    auto& head = queue_.front();
    const auto fragment_size =
        std::min(media_fragment_size, head.bytes.size() - head.sent);
    const auto wire_size = media_header_size + fragment_size + media_tag_size;
    if (controller_.exhausted()) return stop(Error::protocol);
    if (!controller_.can_track() || !pacer_.can_send(wire_size, now_us)) break;
    MediaHeader header;
    header.kind = head.kind;
    header.datagram_sequence = controller_.next_packet_number();
    header.packet_sequence = head.packet_sequence;
    header.key_position = head.key_position;
    header.original_size = static_cast<std::uint16_t>(head.bytes.size());
    header.fragment_offset = static_cast<std::uint16_t>(head.sent);
    header.fragment_size = static_cast<std::uint16_t>(fragment_size);
    const auto sent = transmit(header,
                               std::span<const std::byte>{head.bytes}.subspan(
                                   head.sent, fragment_size),
                               false, now_us, datagrams);
    if (!sent) return sent;
    head.sent += fragment_size;
    if (head.sent == head.bytes.size()) {
      queued_bytes_ -= head.bytes.size();
      queue_.pop_front();
    }
  }
  pacer_.set_idle(queue_.empty() && !controller_.probe_pending(), now_us);
  pace(now_us);
  return {};
}

auto MediaSender::on_ack(std::span<const std::byte> record,
                         std::uint64_t now_us) -> Result<void> {
  if (stopped()) return {};
  now_us = observe(now_us);
  if (progress_expired(now_us)) {
    stop_ = MediaStop::ack_timeout;
    return stop(Error::congestion);
  }
  const auto ack = TransportFrameCodec::decode_media_ack(record);
  if (!ack) return fail(Error::protocol);
  const auto outcome = controller_.on_ack(ack.value(), now_us);
  if (!outcome) return fail(outcome.error());
  if (outcome.value().newly_acked > 0) {
    progress_anchor_us_ = now_us;
    ++acks_;
  }
  follow_path(now_us);
  return {};
}

auto MediaSender::take_idr_request() noexcept -> bool {
  const auto requested = idr_requested_;
  idr_requested_ = false;
  return requested;
}

auto MediaSender::next_deadline_us() const noexcept
    -> std::optional<std::uint64_t> {
  if (stopped()) return std::nullopt;
  timing::Deadline earliest;
  earliest.consider(progress_deadline_us());
  earliest.consider(controller_.loss_detection_deadline());
  if (controller_.probe_pending()) earliest.consider(now_us_);
  if (!queue_.empty()) {
    const auto& head = queue_.front();
    earliest.consider(
        timing::Deadline::after_us(head.queued_us, queue_age_us() + 1));
    const auto fragment_size =
        std::min(media_fragment_size, head.bytes.size() - head.sent);
    const auto wire_size = media_header_size + fragment_size + media_tag_size;
    if (controller_.can_track())
      earliest.consider(std::max(pacer_.next_send_time(wire_size), now_us_));
  }
  return earliest.at_us();
}

}  // namespace libchiaking::net::kakusan
