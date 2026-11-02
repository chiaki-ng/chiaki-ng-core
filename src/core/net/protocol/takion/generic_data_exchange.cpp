#include "src/core/net/protocol/takion/generic_data_exchange.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::net::protocol::takion {
constexpr std::uint32_t GenericDataExchange::control_type(
    GenericDataControlType type) noexcept {
  return static_cast<std::uint32_t>(type);
}

bool GenericDataExchange::ranges_overlap(std::uint64_t left_offset,
                                         std::uint64_t left_length,
                                         std::uint64_t right_offset,
                                         std::uint64_t right_length) noexcept {
  return left_offset < right_offset + right_length &&
         right_offset < left_offset + left_length;
}

auto GenericDataExchange::submit(std::vector<std::byte> data,
                                 std::string metadata, std::uint64_t now_us)
    -> Expected<std::uint32_t, GenericDataError> {
  if (data.empty()) return unexpected(GenericDataError::empty_data);
  if (data.size() >= generic_data_data_size_limit)
    return unexpected(GenericDataError::data_too_large);
  if (metadata.size() >= generic_data_metadata_size_limit)
    return unexpected(GenericDataError::metadata_too_large);
  if (outgoing_.queued() >= generic_data_outgoing_queue_capacity)
    return unexpected(GenericDataError::queue_full);
  const auto transfer_id = next_transfer_id_++;
  const auto packet_count = static_cast<std::uint32_t>(
      (data.size() + generic_data_chunk_size - 1) / generic_data_chunk_size);
  outgoing_.enqueue(generic_data_lifecycle::Transfer{
      transfer_id, std::move(data), std::move(metadata), packet_count});
  outgoing_.process(generic_data_lifecycle::NextOffered{now_us});
  return transfer_id;
}

auto GenericDataExchange::receive_control(const GenericDataControl& control,
                                          std::uint64_t now_us)
    -> Expected<void, GenericDataError> {
  switch (control.type) {
    case control_type(GenericDataControlType::announce):
      handle_announce(control);
      if (!incoming_ || incoming_->transfer_id != control.transfer_id)
        return unexpected(GenericDataError::invalid_announcement);
      return {};
    case control_type(GenericDataControlType::ready):
      outgoing_.process(
          generic_data_lifecycle::ReadyHeard{control.transfer_id, now_us});
      return {};
    case control_type(GenericDataControlType::reject):
      finish_outgoing(GenericDataOutcome::rejected, control.transfer_id,
                      now_us);
      return {};
    case control_type(GenericDataControlType::complete):
      finish_outgoing(GenericDataOutcome::completed, control.transfer_id,
                      now_us);
      return {};
    case control_type(GenericDataControlType::missing_query):
      handle_missing_query(control);
      return {};
    case control_type(GenericDataControlType::missing_response):
      outgoing_.process(generic_data_lifecycle::MissingReported{
          control.transfer_id, control.missing_packets});
      return {};
    default:
      return {};
  }
}

auto GenericDataExchange::receive_chunk(const GenericDataChunk& chunk)
    -> Expected<void, GenericDataError> {
  if (!incoming_ || incoming_->transfer_id != chunk.transfer_id) return {};
  auto& incoming = *incoming_;
  const std::uint64_t offset = chunk.transfer_offset;
  const std::uint64_t length = chunk.payload.size();
  if (chunk.packet_index >= incoming.packet_count || length == 0 ||
      offset + length > incoming.buffer.size())
    return unexpected(GenericDataError::invalid_chunk);
  if (!incoming.missing[chunk.packet_index]) return {};
  for (const auto& range : incoming.ranges) {
    if (ranges_overlap(offset, length, range.offset, range.length))
      return unexpected(GenericDataError::invalid_chunk);
  }
  std::copy(chunk.payload.begin(), chunk.payload.end(),
            incoming.buffer.begin() + static_cast<std::ptrdiff_t>(offset));
  incoming.ranges.push_back(
      ReceivedRange{chunk.transfer_offset, static_cast<std::uint32_t>(length)});
  incoming.covered += static_cast<std::size_t>(length);
  incoming.missing[chunk.packet_index] = false;
  --incoming.missing_count;
  if (incoming.missing_count != 0) return {};
  const auto transfer_id = incoming.transfer_id;
  if (incoming.covered != incoming.buffer.size()) {
    incoming_.reset();
    send_control(
        GenericDataControl{transfer_id,
                           control_type(GenericDataControlType::reject),
                           {},
                           {},
                           {},
                           std::string{"incomplete transfer"},
                           {}});
    return unexpected(GenericDataError::incomplete_coverage);
  }
  outgoing_.actions().emplace_back(GenericDataDelivered{
      transfer_id, std::move(incoming.metadata), std::move(incoming.buffer)});
  incoming_.reset();
  last_completed_incoming_ = transfer_id;
  send_control(
      GenericDataControl{transfer_id,
                         control_type(GenericDataControlType::complete),
                         {},
                         {},
                         {},
                         {},
                         {}});
  return {};
}

void GenericDataExchange::poll(std::uint64_t now_us) {
  outgoing_.process(generic_data_lifecycle::Tick{now_us});
  outgoing_.process(generic_data_lifecycle::NextOffered{now_us});
}

std::vector<GenericDataAction> GenericDataExchange::take_actions() {
  return std::exchange(outgoing_.actions(), {});
}

std::optional<std::uint32_t> GenericDataExchange::active_outgoing()
    const noexcept {
  return outgoing_.transfer_id();
}

std::optional<std::uint64_t> GenericDataExchange::next_poll_us()
    const noexcept {
  return outgoing_.next_deadline_us();
}

std::optional<std::uint32_t> GenericDataExchange::active_incoming()
    const noexcept {
  return incoming_ ? std::optional<std::uint32_t>{incoming_->transfer_id}
                   : std::nullopt;
}

void GenericDataExchange::finish_outgoing(GenericDataOutcome outcome,
                                          std::uint32_t transfer_id,
                                          std::uint64_t now_us) {
  outgoing_.process(generic_data_lifecycle::Concluded{transfer_id, outcome});
  outgoing_.process(generic_data_lifecycle::NextOffered{now_us});
}

void GenericDataExchange::handle_announce(const GenericDataControl& control) {
  if (incoming_ && incoming_->transfer_id == control.transfer_id) {
    send_control(GenericDataControl{control.transfer_id,
                                    control_type(GenericDataControlType::ready),
                                    {},
                                    {},
                                    {},
                                    {},
                                    {}});
    return;
  }
  if (incoming_ && control.transfer_id < incoming_->transfer_id) {
    reject_incoming_announcement(control.transfer_id, "stale transfer");
    return;
  }
  const auto size = control.data_size.value_or(0);
  const auto count = control.packet_count.value_or(0);
  if (size == 0 || size >= generic_data_data_size_limit || count == 0 ||
      count > generic_data_packet_count_limit || count > size ||
      (control.metadata &&
       control.metadata->size() >= generic_data_metadata_size_limit)) {
    reject_incoming_announcement(control.transfer_id, "invalid announcement");
    return;
  }
  Incoming incoming;
  incoming.transfer_id = control.transfer_id;
  incoming.metadata = control.metadata.value_or(std::string{});
  incoming.packet_count = count;
  incoming.buffer.resize(size);
  incoming.missing.assign(count, true);
  incoming.missing_count = count;
  incoming_ = std::move(incoming);
  send_control(GenericDataControl{control.transfer_id,
                                  control_type(GenericDataControlType::ready),
                                  {},
                                  {},
                                  {},
                                  {},
                                  {}});
}

void GenericDataExchange::handle_missing_query(
    const GenericDataControl& control) {
  if (incoming_ && incoming_->transfer_id == control.transfer_id) {
    GenericDataControl response{
        control.transfer_id,
        control_type(GenericDataControlType::missing_response),
        {},
        {},
        {},
        {},
        {}};
    for (std::uint32_t index = 0;
         index < incoming_->packet_count &&
         response.missing_packets.size() < generic_data_missing_response_limit;
         ++index) {
      if (incoming_->missing[index]) response.missing_packets.push_back(index);
    }
    send_control(std::move(response));
    return;
  }
  if (last_completed_incoming_ == control.transfer_id)
    send_control(
        GenericDataControl{control.transfer_id,
                           control_type(GenericDataControlType::complete),
                           {},
                           {},
                           {},
                           {},
                           {}});
}

void GenericDataExchange::reject_incoming_announcement(
    std::uint32_t transfer_id, std::string error) {
  send_control(GenericDataControl{transfer_id,
                                  control_type(GenericDataControlType::reject),
                                  {},
                                  {},
                                  {},
                                  std::move(error),
                                  {}});
}

void GenericDataExchange::send_control(GenericDataControl control) {
  outgoing_.actions().emplace_back(GenericDataSendControl{std::move(control)});
}

}  // namespace libchiaking::net::protocol::takion
