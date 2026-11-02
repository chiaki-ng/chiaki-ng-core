#include "src/core/net/kakusan/link.hpp"

#include "libchiaking/detail/secret_array.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace libchiaking::net::kakusan {
Link::Link(std::unique_ptr<SpokeTransport> transport,
           TransportRole role) noexcept
    : transport_(std::move(transport)), role_(role) {}

Link::~Link() { close(); }

auto Link::gather(std::uint64_t now_us,
                  std::optional<IceCredentials> credentials) -> Result<void> {
  if (lifecycle_.closed()) return fail(Error::internal);
  return transport_->gather(now_us, std::move(credentials));
}

auto Link::connect(const TransportDescription& remote,
                   const Transcript& transcript, std::uint64_t now_us)
    -> Result<void> {
  if (lifecycle_.closed() || transcript_) return fail(Error::internal);
  transcript_ = transcript;
  return transport_->connect(remote, now_us);
}

auto Link::pump(std::uint64_t now_us) -> std::span<LinkEvent> {
  lifecycle_.begin_pump();
  if (!lifecycle_.running()) return lifecycle_.events();
  scratch_.clear();
  const auto pumped = transport_->pump(now_us, scratch_);
  for (auto& event : scratch_) {
    if (!lifecycle_.running()) break;
    if (auto* gathered = std::get_if<transport_event::Gathered>(&event)) {
      lifecycle_.emit(link_event::Gathered{std::move(gathered->local)});
    } else if (std::holds_alternative<transport_event::Nominated>(event)) {
      lifecycle_.emit(link_event::Nominated{});
    } else if (std::holds_alternative<transport_event::Established>(event)) {
      on_established(now_us);
    } else if (auto* record = std::get_if<transport_event::Record>(&event)) {
      on_record(record->bytes.span(), now_us);
    } else if (auto* datagram =
                   std::get_if<transport_event::Datagram>(&event)) {
      if (role_ == TransportRole::guest && channel_ && !lifecycle_.deaf())
        lifecycle_.emit(link_event::Media{std::move(datagram->bytes)});
    } else if (const auto* failed =
                   std::get_if<transport_event::Failed>(&event)) {
      lifecycle_.stop(failed->error);
    }
  }
  scratch_.clear();
  if (!pumped && lifecycle_.running()) lifecycle_.stop(pumped.error());
  if (lifecycle_.running()) flush(now_us);
  return lifecycle_.events();
}

auto Link::next_deadline_us() const noexcept -> std::optional<std::uint64_t> {
  if (!lifecycle_.running()) return std::nullopt;
  auto deadline = transport_->next_deadline_us();
  if (channel_)
    deadline =
        timing::Deadline::earliest(deadline, channel_->next_deadline_us());
  return deadline;
}

auto Link::send_control(const control::Message& message, AckToken token,
                        bool terminal, std::uint64_t now_us) -> Result<void> {
  if (!lifecycle_.running() || !channel_) return fail(Error::no_path);
  if (terminal) {
    if (!channel_->send_terminal(message, now_us)) return fail(Error::no_path);
    tickets_.clear();
    terminal_token_ = token;
    return {};
  }
  const auto ticket = channel_->enqueue(message, now_us);
  if (!ticket) return fail(ticket.error());
  tickets_.emplace_back(ticket.value(), token);
  return {};
}

auto Link::send_input(const InputFrame& frame) -> Result<void> {
  if (!lifecycle_.running() || !channel_ || role_ != TransportRole::guest)
    return fail(Error::no_path);
  const auto bytes = ControllerFrameCodec::encode_input(frame);
  if (!bytes) return fail(bytes.error());
  return transport_->send_record(bytes.value());
}

auto Link::send_feedback(const FeedbackFrame& frame) -> Result<void> {
  if (!lifecycle_.running() || !channel_ || role_ != TransportRole::host)
    return fail(Error::no_path);
  const auto bytes = ControllerFrameCodec::encode_feedback(frame);
  if (!bytes) return fail(bytes.error());
  return transport_->send_record(bytes.value());
}

auto Link::send_media_ack(std::span<const std::byte> record) -> Result<void> {
  if (!lifecycle_.running() || !channel_ || role_ != TransportRole::guest)
    return fail(Error::no_path);
  return transport_->send_record(record);
}

auto Link::send_media(std::span<const std::byte> datagram) -> Result<void> {
  if (!lifecycle_.running() || !channel_ || role_ != TransportRole::host)
    return fail(Error::no_path);
  return transport_->send_datagram(datagram);
}

auto Link::media_key() -> Result<MediaKey> {
  if (lifecycle_.closed() || !channel_ || !transcript_)
    return fail(Error::internal);
  MediaKey key{};
  const auto exported =
      transport_->export_secret(media_exporter_label, *transcript_, key);
  if (!exported) return fail(exported.error());
  return key;
}

void Link::close() noexcept {
  if (lifecycle_.closed()) return;
  lifecycle_.close();
  channel_.reset();
  tickets_.clear();
  terminal_token_.reset();
  transport_->close();
}

void Link::on_established(std::uint64_t now_us) {
  if (channel_ || !transcript_) return lifecycle_.stop(Error::internal);
  Exporter exporter;
  const auto exported =
      transport_->export_secret(pairing_exporter_label, *transcript_, exporter);
  if (!exported) return lifecycle_.stop(exported.error());
  channel_.emplace(now_us);
  lifecycle_.emit(link_event::Established{std::move(exporter)});
}

void Link::on_record(std::span<const std::byte> record, std::uint64_t now_us) {
  if (!channel_) return;
  const bool host = role_ == TransportRole::host;
  if (std::ranges::starts_with(record, Hash::bytes_of(control_prefix))) {
    auto message = channel_->receive(record, now_us);
    if (lifecycle_.deaf()) return;
    if (!message) return lifecycle_.deafen(message.error());
    if (message.value()) {
      lifecycle_.emit(link_event::Control{std::move(*message.value())});
    }
    return;
  }
  if (lifecycle_.deaf()) return;
  if (host && std::ranges::starts_with(record, Hash::bytes_of(input_prefix))) {
    const auto frame = ControllerFrameCodec::decode_input(record);
    if (!frame) return lifecycle_.deafen(Error::protocol);
    lifecycle_.emit(link_event::Input{frame.value()});
    return;
  }
  if (!host &&
      std::ranges::starts_with(record, Hash::bytes_of(feedback_prefix))) {
    const auto frame = ControllerFrameCodec::decode_feedback(record);
    if (!frame) return lifecycle_.deafen(Error::protocol);
    lifecycle_.emit(link_event::Feedback{frame.value()});
    return;
  }
  if (host &&
      std::ranges::starts_with(record, Hash::bytes_of(media_ack_prefix))) {
    lifecycle_.emit(link_event::MediaAck{{record.begin(), record.end()}});
    return;
  }
  lifecycle_.deafen(Error::protocol);
}

void Link::flush(std::uint64_t now_us) {
  if (!channel_) return;
  while (const auto ticket = channel_->take_acknowledged()) {
    for (auto entry = tickets_.begin(); entry != tickets_.end();) {
      if (entry->first == *ticket) {
        lifecycle_.emit(link_event::Acked{entry->second});
        entry = tickets_.erase(entry);
      } else {
        ++entry;
      }
    }
  }
  records_.clear();
  const auto polled = channel_->poll(now_us, records_);
  for (const auto& record : records_) {
    const auto sent = transport_->send_record(record);
    if (!sent) return lifecycle_.stop(sent.error());
  }
  records_.clear();
  if (!polled) return lifecycle_.stop(polled.error());
  if (terminal_token_ && channel_->idle()) {
    lifecycle_.emit(link_event::Acked{*terminal_token_});
    terminal_token_.reset();
  }
}

}  // namespace libchiaking::net::kakusan
