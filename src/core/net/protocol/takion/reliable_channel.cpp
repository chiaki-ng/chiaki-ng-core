#include "src/core/net/protocol/takion/reliable_channel.hpp"
#include "src/core/utils/bytes/endian.hpp"

#include "src/core/net/protocol/takion/limits.hpp"
#include "src/core/utils/numeric/checked.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

namespace libchiaking::net::protocol::takion {
OutboundFragment::OutboundFragment(const OutboundFragment& other)
    : sequence(other.sequence),
      channel(other.channel),
      layout(other.layout),
      end_of_message(other.end_of_message),
      data_type(other.data_type),
      payload(other.payload.clone()),
      datagram(other.datagram.clone()) {}

OutboundFragment& OutboundFragment::operator=(const OutboundFragment& other) {
  if (this != &other) *this = OutboundFragment{other};
  return *this;
}

OutboundFragment& OutboundFragment::operator=(
    OutboundFragment&& other) noexcept {
  if (this != &other) {
    sequence = other.sequence;
    channel = other.channel;
    layout = other.layout;
    end_of_message = other.end_of_message;
    data_type = other.data_type;
    payload = std::move(other.payload);
    datagram = std::move(other.datagram);
  }
  return *this;
}

ReliableChannel::ReliableChannel(ReliableChannelConfig config)
    : config_(config),
      next_local_sequence_(config.initial_local_sequence),
      cumulative_(config.initial_remote_sequence - 1U),
      peer_window_(config.initial_peer_receive_window),
      sent_sequence_(config.initial_local_sequence - 1U),
      round_trip_(config.retransmission_timeout_us,
                  config.minimum_retransmission_timeout_us,
                  config.maximum_retransmission_timeout_us) {}

bool ReliableChannel::is_valid(const ReliableChannelConfig& config) noexcept {
  return config.maximum_message_size != 0 &&
         config.maximum_fragment_size >= first_fragment_overhead &&
         config.maximum_fragment_count != 0 &&
         config.retransmission_window_size != 0 &&
         config.retransmission_window_size < serial_half_range &&
         config.receive_reorder_size != 0 &&
         config.receive_reorder_size < serial_half_range &&
         config.retransmission_timeout_us != 0 &&
         config.minimum_retransmission_timeout_us != 0 &&
         config.minimum_retransmission_timeout_us <=
             config.maximum_retransmission_timeout_us;
}

auto ReliableChannel::create(ReliableChannelConfig config)
    -> Expected<ReliableChannel, ReliableChannelError> {
  if (!is_valid(config))
    return unexpected(ReliableChannelError::invalid_configuration);
  return ReliableChannel{config};
}

std::size_t ReliableChannel::outstanding_fragment_count() const noexcept {
  return static_cast<std::size_t>(
      std::ranges::count(pending_, false, &PendingFragment::acknowledged));
}

bool ReliableChannel::has_incomplete_message() const noexcept {
  return !reassemblies_.empty();
}

auto ReliableChannel::send(std::uint16_t channel, MessageDataType data_type,
                           std::span<const std::byte> message,
                           std::uint64_t now_us,
                           std::optional<std::uint64_t> operation_id,
                           MessageFragmentation fragmentation)
    -> Expected<SentFragments, ReliableChannelError> {
  if (message.size() > config_.maximum_message_size)
    return unexpected(ReliableChannelError::message_too_large);

  const bool split =
      fragmentation == MessageFragmentation::split_to_fragment_size;
  const auto first_capacity =
      split ? config_.maximum_fragment_size - first_fragment_overhead
            : message.size();
  const auto continuation_capacity =
      config_.maximum_fragment_size - continuation_fragment_overhead;
  std::size_t fragment_count = 1;
  if (message.size() > first_capacity) {
    const auto remainder = message.size() - first_capacity;
    fragment_count +=
        remainder / continuation_capacity +
        static_cast<std::size_t>(remainder % continuation_capacity != 0);
  }
  if (fragment_count > config_.maximum_fragment_count)
    return unexpected(ReliableChannelError::fragment_limit_exceeded);

  SentFragments result;
  result.fragments.reserve(fragment_count);
  std::vector<OutboundFragment> untracked;
  std::size_t offset = 0;
  for (std::size_t index = 0; index < fragment_count; ++index) {
    const auto layout =
        index == 0 ? FragmentLayout::first : FragmentLayout::continuation;
    const auto capacity = layout == FragmentLayout::first
                              ? first_capacity
                              : continuation_capacity;
    const auto data_size = std::min(capacity, message.size() - offset);

    OutboundFragment fragment;
    fragment.sequence = next_local_sequence_++;
    fragment.channel = channel;
    fragment.layout = layout;
    fragment.end_of_message = index + 1 == fragment_count;
    fragment.data_type = data_type;
    fragment.payload.reserve((layout == FragmentLayout::first
                                  ? first_fragment_overhead
                                  : continuation_fragment_overhead) +
                             data_size);
    bytes::append_be<std::uint32_t>(fragment.payload, fragment.sequence);
    bytes::append_be<std::uint16_t>(fragment.payload, fragment.channel);
    bytes::append_be<std::uint16_t>(fragment.payload, 0);
    if (layout == FragmentLayout::first)
      fragment.payload.push_back(static_cast<std::byte>(data_type.raw));
    fragment.payload.append(message.subspan(offset, data_size));
    offset += data_size;

    if (pending_.size() < config_.retransmission_window_size) {
      PendingFragment pending;
      pending.payload_size = static_cast<std::uint32_t>(
          fragment.payload.size() - data_chunk_body_header_size);
      pending.first_send_us = now_us;
      pending.timer_start_us = now_us;
      pending.operation_id = operation_id;
      pending.fragment = std::move(fragment);
      pending_.push_back(std::move(pending));
    } else {
      ++result.untracked_fragments;
      untracked.push_back(std::move(fragment));
    }
  }
  transmit_unsent(now_us, result.fragments);
  std::ranges::move(untracked, std::back_inserter(result.fragments));
  if (!pending_.empty())
    schedule_pass_by(timing::Deadline::after_us(
        now_us, std::min(round_trip_.timeout_us(), scheduler_wait_cap_us)));
  return result;
}

void ReliableChannel::schedule_pass_by(std::uint64_t deadline_us) noexcept {
  if (!next_pass_us_ || deadline_us < *next_pass_us_)
    next_pass_us_ = deadline_us;
}

bool ReliableChannel::attempt(std::size_t index, std::uint64_t now_us,
                              std::vector<OutboundFragment>& transmissions,
                              std::vector<std::uint32_t>& exhausted) {
  auto& pending = pending_[index];
  const auto decision = DataAttemptDecision::next(
      pending.transmission_count, pending.accumulated_ms,
      DataAttemptDecision::discarding_attempt(config_.maximum_retries));
  pending.previous_sent_sequence = sent_sequence_;
  pending.transmission_count = decision.transmission_count;
  const auto sequence = pending.fragment.sequence;
  if (decision.discard) {
    if (sequence > sent_sequence_) sent_sequence_ = sequence;
    exhausted.push_back(sequence);
    pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
  }
  if (decision.transmission_count == 1) pending.first_send_us = now_us;
  pending.timer_start_us = now_us;
  transmissions.push_back(pending.fragment);
  if (serial_difference(sent_sequence_, sequence) < 0)
    sent_sequence_ = sequence;
  peer_window_ = numeric::saturating_sub(peer_window_, pending.payload_size);
  return false;
}

void ReliableChannel::transmit_unsent(
    std::uint64_t now_us, std::vector<OutboundFragment>& transmissions) {
  std::vector<std::uint32_t> exhausted;
  for (std::size_t index = 0; index < pending_.size() && peer_window_ != 0;) {
    if (pending_[index].transmission_count != 0 ||
        !attempt(index, now_us, transmissions, exhausted))
      ++index;
  }
}

ReceiveOutcome ReliableChannel::receive(std::span<const std::byte> chunk_body,
                                        bool final_fragment,
                                        std::uint64_t now_us) {
  ReceiveOutcome result;
  if (chunk_body.size() < data_chunk_body_header_size) {
    result.anomaly = ReliableChannelError::truncated_fragment;
    return result;
  }
  const auto sequence = bytes::load_be<std::uint32_t>(chunk_body);
  const auto offset = static_cast<std::uint32_t>(sequence - cumulative_);
  if (serial_difference(sequence, cumulative_) <= 0 ||
      std::ranges::contains(receive_queue_, sequence,
                            &InboundFragment::sequence)) {
    result.anomaly = ReliableChannelError::duplicate;
    result.acknowledgement = selective_acknowledgement();
    return result;
  }
  if (offset > config_.receive_reorder_size) {
    if (!beyond_window_since_us_) beyond_window_since_us_ = now_us;
    if (timing::Deadline::elapsed_us(*beyond_window_since_us_, now_us) <
        missing_sequence_expiry_us()) {
      result.anomaly = ReliableChannelError::receive_window_exceeded;
      result.acknowledgement = selective_acknowledgement();
      return result;
    }
    beyond_window_since_us_.reset();
    receive_queue_.clear();
    reassemblies_.clear();
    cumulative_ = sequence - 1U;
    result.anomaly = ReliableChannelError::reassembly_expired;
  } else {
    beyond_window_since_us_.reset();
  }

  InboundFragment fragment;
  fragment.sequence = sequence;
  fragment.channel = bytes::load_be<std::uint16_t>(chunk_body.subspan(4));
  fragment.final_fragment = final_fragment;
  fragment.data.assign(chunk_body.begin() + static_cast<std::ptrdiff_t>(
                                                data_chunk_body_header_size),
                       chunk_body.end());
  fragment.arrival_us = now_us;
  const auto position = std::ranges::find_if(
      receive_queue_,
      [sequence](std::uint32_t queued) {
        return serial_difference(queued, sequence) > 0;
      },
      &InboundFragment::sequence);
  receive_queue_.insert(position, std::move(fragment));

  const auto& first = receive_queue_.front();
  const auto skips_missing = first.sequence != cumulative_ + 1U;
  if (skips_missing) {
    if (timing::Deadline::elapsed_us(first.arrival_us, now_us) <
        missing_sequence_expiry_us()) {
      result.acknowledgement = selective_acknowledgement();
      return result;
    }
    reassemblies_.clear();
    result.anomaly = ReliableChannelError::reassembly_expired;
  }

  auto expected = receive_queue_.front().sequence;
  while (!receive_queue_.empty() &&
         receive_queue_.front().sequence == expected) {
    auto next = std::move(receive_queue_.front());
    receive_queue_.pop_front();
    auto consumed = consume_fragment(std::move(next));
    if (!consumed) {
      if (!result.anomaly) result.anomaly = consumed.error();
    } else if (consumed.value()) {
      result.messages.push_back(std::move(*consumed.value()));
    }
    ++expected;
  }
  cumulative_ = expected - 1U;
  result.acknowledgement = selective_acknowledgement();
  return result;
}

auto ReliableChannel::consume_fragment(InboundFragment fragment)
    -> Expected<std::optional<ReceivedMessage>, ReliableChannelError> {
  std::optional<ReliableChannelError> overflow;
  auto& assembly = reassemblies_[fragment.channel];
  if (!assembly.overflowed) {
    if (assembly.fragment_count >= config_.maximum_fragment_count)
      overflow = ReliableChannelError::fragment_limit_exceeded;
    else if (fragment.data.size() >
             config_.maximum_message_size + 1U - assembly.data.size())
      overflow = ReliableChannelError::message_too_large;
    if (overflow) {
      assembly.overflowed = true;
      assembly.data.clear();
      assembly.fragment_count = 0;
    }
  }
  if (!assembly.overflowed) {
    assembly.data.insert(assembly.data.end(), fragment.data.begin(),
                         fragment.data.end());
    ++assembly.fragment_count;
  }
  if (fragment.final_fragment) {
    auto completed = std::move(assembly);
    reassemblies_.erase(fragment.channel);
    if (!completed.overflowed && completed.data.empty())
      overflow = overflow.value_or(ReliableChannelError::truncated_fragment);
    if (!completed.overflowed && !completed.data.empty()) {
      ReceivedMessage message;
      message.channel = fragment.channel;
      message.data_type.raw =
          std::to_integer<std::uint8_t>(completed.data.front());
      message.payload.assign(completed.data.begin() + 1, completed.data.end());
      return std::optional<ReceivedMessage>{std::move(message)};
    }
  }
  if (overflow) return unexpected(*overflow);
  return std::optional<ReceivedMessage>{};
}

std::uint64_t ReliableChannel::missing_sequence_expiry_us() const noexcept {
  return round_trip_.timeout_us() <= missing_sequence_expiry_rto_threshold_us
             ? missing_sequence_expiry_short_us
             : missing_sequence_expiry_long_us;
}

std::size_t ReliableChannel::held_receive_bytes() const noexcept {
  std::size_t held = 0;
  for (const auto& fragment : receive_queue_)
    held += fragment.data.size() + data_chunk_body_header_size;
  for (const auto& [channel, assembly] : reassemblies_)
    held += assembly.data.size() +
            assembly.fragment_count * data_chunk_body_header_size;
  return held;
}

DataAcknowledgement ReliableChannel::selective_acknowledgement() const {
  DataAcknowledgement acknowledgement;
  acknowledgement.cumulative_sequence = cumulative_;
  const auto held = held_receive_bytes();
  acknowledgement.advertised_receive_window =
      held < config_.local_receive_window
          ? static_cast<std::uint32_t>(config_.local_receive_window - held)
          : 0;
  for (const auto& fragment : receive_queue_) {
    const auto offset =
        static_cast<std::uint32_t>(fragment.sequence - cumulative_);
    if (offset > std::numeric_limits<std::uint16_t>::max()) break;
    auto& blocks = acknowledgement.gap_blocks;
    if (!blocks.empty() &&
        offset == static_cast<std::uint32_t>(blocks.back().end) + 1U) {
      blocks.back().end = static_cast<std::uint16_t>(offset);
      continue;
    }
    if (blocks.size() == maximum_gap_blocks) break;
    blocks.push_back({static_cast<std::uint16_t>(offset),
                      static_cast<std::uint16_t>(offset)});
  }
  return acknowledgement;
}

auto ReliableChannel::acknowledge(const DataAcknowledgement& acknowledgement,
                                  std::uint64_t now_us)
    -> Expected<AcknowledgementOutcome, ReliableChannelError> {
  const auto& gaps = acknowledgement.gap_blocks;
  std::optional<std::uint16_t> previous_end;
  for (const auto& block : gaps) {
    if (block.start == 0 || block.end < block.start ||
        block.end > maximum_gap_offset ||
        (previous_end && block.start <= *previous_end + 1))
      return unexpected(ReliableChannelError::invalid_acknowledgement);
    previous_end = block.end;
  }

  AcknowledgementOutcome result;
  peer_window_ = acknowledgement.advertised_receive_window;
  if (pending_.empty()) return result;
  const auto cumulative = acknowledgement.cumulative_sequence;
  const auto beyond_sent = [this](std::uint32_t sequence) {
    return serial_difference(sent_sequence_, sequence) < 0;
  };
  if (beyond_sent(cumulative) ||
      (!gaps.empty() && beyond_sent(cumulative + gaps.back().end)))
    return unexpected(ReliableChannelError::invalid_acknowledgement);
  if (serial_difference(cumulative, pending_.front().fragment.sequence) < -1)
    return result;

  std::map<std::uint64_t, std::size_t> unreported_by_operation;
  for (const auto& pending : pending_) {
    if (pending.operation_id && !pending.reported)
      ++unreported_by_operation[*pending.operation_id];
  }
  auto sample_us = round_trip_sample_ceiling_ms * microseconds_per_millisecond;
  const auto acknowledge_entry = [&](PendingFragment& pending) {
    if (pending.transmission_count == 1)
      sample_us = std::min(sample_us, timing::Deadline::elapsed_us(
                                          pending.timer_start_us, now_us));
    if (pending.reported) return;
    pending.reported = true;
    auto complete = false;
    if (pending.operation_id)
      complete = --unreported_by_operation[*pending.operation_id] == 0;
    result.acknowledged.push_back(AcknowledgedFragment{
        pending.fragment.sequence,
        timing::Deadline::elapsed_us(pending.first_send_us, now_us),
        pending.operation_id,
        complete,
    });
  };

  while (!pending_.empty() &&
         serial_difference(pending_.front().fragment.sequence, cumulative) <=
             0) {
    acknowledge_entry(pending_.front());
    pending_.pop_front();
  }
  if (!gaps.empty()) {
    const auto highest = cumulative + gaps.back().end;
    std::size_t gap = 0;
    for (std::size_t index = 0; index < pending_.size(); ++index) {
      const auto sequence = pending_[index].fragment.sequence;
      while (gap < gaps.size() &&
             serial_difference(sequence, cumulative + gaps[gap].end) > 0)
        ++gap;
      if (gap == gaps.size()) break;
      if (serial_difference(sequence, cumulative + gaps[gap].start) < 0) {
        if (peer_window_ != 0 &&
            serial_difference(pending_[index].previous_sent_sequence,
                              highest) <= 0)
          static_cast<void>(attempt(index, now_us, result.retransmissions,
                                    result.exhausted_sequences));
        continue;
      }
      pending_[index].acknowledged = true;
      acknowledge_entry(pending_[index]);
    }
  }
  if (const auto sample_ms = sample_us / microseconds_per_millisecond;
      sample_ms < round_trip_sample_ceiling_ms) {
    round_trip_.observe(sample_ms);
    round_trip_samples_.observe(sample_us, now_us);
  }
  if (peer_window_ != 0 &&
      std::ranges::contains(pending_, std::uint32_t{0},
                            &PendingFragment::transmission_count))
    schedule_pass_by(now_us);
  return result;
}

RetransmissionPollResult ReliableChannel::poll(std::uint64_t now_us) {
  RetransmissionPollResult result;
  if (pending_.empty()) {
    next_pass_us_.reset();
    return result;
  }
  if (next_pass_us_ && now_us < *next_pass_us_) return result;

  const auto timeout = round_trip_.timeout_us();
  auto wait = scheduler_wait_cap_us;
  if (peer_window_ != 0) {
    for (std::size_t index = 0; index < pending_.size() && peer_window_ != 0;
         ++index) {
      if (pending_[index].acknowledged) continue;
      const auto elapsed =
          timing::Deadline::elapsed_us(pending_[index].timer_start_us, now_us);
      auto remaining = numeric::saturating_sub(timeout, elapsed);
      if (remaining == 0 || pending_[index].transmission_count == 0) {
        static_cast<void>(attempt(index, now_us, result.retransmissions,
                                  result.exhausted_sequences));
        remaining = timeout;
      }
      wait = std::min(wait, remaining);
    }
  } else {
    std::optional<std::uint64_t> soonest;
    for (const auto& pending : pending_) {
      const auto elapsed =
          timing::Deadline::elapsed_us(pending.timer_start_us, now_us);
      if (!pending.acknowledged && elapsed < timeout)
        soonest = std::min(soonest.value_or(timeout), timeout - elapsed);
    }
    if (soonest) {
      wait = std::min(wait, *soonest);
    } else {
      static_cast<void>(attempt(0, now_us, result.retransmissions,
                                result.exhausted_sequences));
      wait = timeout;
    }
  }
  if (pending_.empty())
    next_pass_us_.reset();
  else
    next_pass_us_ = timing::Deadline::after_us(now_us, wait);
  return result;
}

void ReliableChannel::record_datagram(std::uint32_t sequence,
                                      std::span<const std::byte> datagram) {
  const auto found = std::ranges::find(
      pending_, sequence,
      [](const PendingFragment& pending) { return pending.fragment.sequence; });
  if (found != pending_.end()) found->fragment.datagram.assign(datagram);
}

}  // namespace libchiaking::net::protocol::takion
