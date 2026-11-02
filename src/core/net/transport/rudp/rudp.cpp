#include "src/core/net/transport/rudp/rudp.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/utils/bytes/endian.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <utility>

namespace libchiaking::net::transport::rudp {

auto Handshake::generate_parameters(platform::SecureRandomProvider& provider)
    -> Expected<HandshakeParameters, platform::PlatformError> {
  std::array<std::byte, 8> bytes{};
  if (auto filled = provider.fill_bytes(bytes); !filled.has_value())
    return unexpected(filled.error());
  const auto sample0 = bytes::load_be<std::uint32_t>(bytes);
  const auto sample1 =
      bytes::load_be<std::uint32_t>(std::span{bytes}.subspan(4));
  return HandshakeParameters{
      static_cast<std::uint16_t>(sample0 % 0x5E00U + 0x1FFU),
      sample1 + 0x8000U};
}

Handshake::Handshake(std::uint64_t attempt_id, std::uint16_t local_counter,
                     std::uint32_t local_header)
    : attempt_id_(attempt_id),
      lifecycle_(std::make_unique<handshake_lifecycle::Lifecycle>(
          local_counter, local_header)) {}

std::uint64_t Handshake::attempt_id() const { return attempt_id_; }
HandshakeState Handshake::state() const { return lifecycle_->state(); }
std::uint32_t Handshake::local_header() const {
  return lifecycle_->context().local_header;
}
std::uint16_t Handshake::next_counter() const {
  return lifecycle_->context().local_counter;
}

std::optional<std::uint16_t> Handshake::remote_counter() const {
  if (state() != HandshakeState::established) return std::nullopt;
  return lifecycle_->context().remote_counter;
}

int Handshake::sends_for_stage() const {
  return lifecycle_->context().stage_sends;
}

bool Handshake::awaiting() const noexcept {
  const auto current = lifecycle_->state();
  return current == HandshakeState::awaiting_init ||
         current == HandshakeState::awaiting_cookie;
}

Handshake::Step Handshake::begin() {
  return advance(handshake_lifecycle::Begin{})
      .value_or(unexpected(HandshakeError::invalid_transition));
}

Handshake::Step Handshake::on_packet(std::uint64_t attempt,
                                     std::span<const std::byte> packet) {
  if (attempt != attempt_id_) {
    lifecycle_->count_stale();
    return std::optional<std::vector<std::byte>>{};
  }
  const auto step = packet.size() <= FrameCodec::header_bytes
                        ? advance(handshake_lifecycle::TruncatedReply{})
                        : advance(read_reply(packet));
  return step.value_or(std::optional<std::vector<std::byte>>{});
}

Handshake::Step Handshake::on_timeout() {
  return advance(handshake_lifecycle::ReplyOverdue{})
      .value_or(std::optional<std::vector<std::byte>>{});
}

auto Handshake::cancel() -> Expected<void, HandshakeError> {
  if (!lifecycle_->process(handshake_lifecycle::Cancel{}))
    return unexpected(HandshakeError::invalid_transition);
  return {};
}

template <class Event>
auto Handshake::advance(const Event& event) -> std::optional<Step> {
  if (!lifecycle_->process(event)) return std::nullopt;
  if (auto effect = lifecycle_->take_effect()) return send(*effect);
  if (const auto failure = lifecycle_->context().failure)
    return Step{unexpected(*failure)};
  return Step{std::optional<std::vector<std::byte>>{}};
}

Handshake::Step Handshake::send(const handshake_lifecycle::Effect& effect) {
  auto encoded = encode(effect);
  if (!encoded) {
    static_cast<void>(
        lifecycle_->process(handshake_lifecycle::RequestUnencodable{}));
    return unexpected(HandshakeError::encode_failed);
  }
  static_cast<void>(lifecycle_->process(handshake_lifecycle::RequestSent{}));
  return std::optional<std::vector<std::byte>>{std::move(encoded).value()};
}

auto Handshake::encode(const handshake_lifecycle::Effect& effect)
    -> Expected<std::vector<std::byte>, FrameError> {
  if (const auto* init = std::get_if<handshake_lifecycle::SendInit>(&effect))
    return FrameCodec::encode_init_request(init->counter, init->header);
  const auto& cookie = std::get<handshake_lifecycle::SendCookie>(effect);
  return FrameCodec::encode_cookie_request(cookie.counter, cookie.header,
                                           cookie.cookie);
}

auto Handshake::read_reply(std::span<const std::byte> packet)
    -> handshake_lifecycle::Reply {
  handshake_lifecycle::Reply reply;
  if (auto init = FrameCodec::decode_init_response(packet))
    reply.init_answer = std::move(init).value();
  if (auto cookie = FrameCodec::decode_cookie_response(packet))
    reply.cookie_answer = cookie.value();
  return reply;
}

HandshakeRunner::HandshakeRunner(Handshake fsm, HandshakeChannel& channel,
                                 platform::ClockProvider& clock,
                                 platform::StopToken stop_token)
    : fsm_(std::move(fsm)),
      channel_(channel),
      clock_(clock),
      stop_token_(std::move(stop_token)) {}

auto HandshakeRunner::connect_error(HandshakeError error) noexcept
    -> ConnectError {
  switch (error) {
    case HandshakeError::retries_exhausted:
      return ConnectError::timeout;
    case HandshakeError::network:
      return ConnectError::receive_failed;
    case HandshakeError::encode_failed:
    case HandshakeError::invalid_transition:
      break;
  }
  return ConnectError::handshake_failed;
}

auto HandshakeRunner::stop() -> Unexpected<ConnectError> {
  static_cast<void>(fsm_.cancel());
  return unexpected(ConnectError::stopped);
}

auto HandshakeRunner::run() -> Expected<HandshakeCompletion, ConnectError> {
  auto step = fsm_.begin();
  while (step && step->has_value()) {
    if (stop_token_.stop_requested()) return stop();
    const auto& packet = **step;
    const auto sent = channel_.send(packet);
    if (!sent) return unexpected(ConnectError::send_failed);
    if (sent.value() != packet.size())
      return unexpected(ConnectError::short_send);
    ++total_sends_;
    auto reply = await_reply(timing::Deadline::after_us(
        clock_.now_microseconds(), rudp_expect_timeout_us));
    if (!reply) return unexpected(reply.error());
    step = std::move(reply).value();
  }
  return finish(step);
}

auto HandshakeRunner::await_reply(std::uint64_t deadline_us)
    -> Expected<Handshake::Step, ConnectError> {
  for (;;) {
    if (stop_token_.stop_requested()) return stop();
    const auto now = clock_.now_microseconds();
    if (now >= deadline_us) return fsm_.on_timeout();
    const auto received = channel_.receive_for(
        std::chrono::microseconds(deadline_us - now), stop_token_);
    if (!received) {
      if (stop_token_.stop_requested()) return stop();
      return unexpected(ConnectError::receive_failed);
    }
    if (!received.value().has_value()) continue;
    auto next = fsm_.on_packet(fsm_.attempt_id(), *received.value());
    if (!next || next->has_value() || !fsm_.awaiting()) return next;
  }
}

auto HandshakeRunner::finish(const Handshake::Step& step) const
    -> Expected<HandshakeCompletion, ConnectError> {
  if (!step) return unexpected(connect_error(step.error()));
  switch (fsm_.state()) {
    case HandshakeState::established:
      return HandshakeCompletion{fsm_.local_header(), fsm_.next_counter(),
                                 fsm_.remote_counter().value()};
    case HandshakeState::cancelled:
      return unexpected(ConnectError::stopped);
    case HandshakeState::idle:
    case HandshakeState::awaiting_init:
    case HandshakeState::awaiting_cookie:
    case HandshakeState::failed:
      break;
  }
  return unexpected(ConnectError::handshake_failed);
}

ProviderRudpHandshakeChannel::ProviderRudpHandshakeChannel(
    net::transport::datagram::ProviderUdpTransport& transport) noexcept
    : transport_(&transport) {}

auto ProviderRudpHandshakeChannel::send(std::span<const std::byte> datagram)
    -> Expected<std::size_t, net::transport::datagram::TransportError> {
  return transport_->send(datagram);
}

auto ProviderRudpHandshakeChannel::receive_for(
    std::chrono::microseconds timeout, const platform::StopToken& stop_token)
    -> Expected<std::optional<net::transport::datagram::OwnedDatagram>,
                net::transport::datagram::TransportError> {
  if (stop_token.stop_requested())
    return std::optional<net::transport::datagram::OwnedDatagram>{};
  const auto readable = transport_->wait_readable(
      std::chrono::duration_cast<std::chrono::nanoseconds>(timeout),
      stop_token);
  if (!readable) return libchiaking::unexpected(readable.error());
  if (!readable.value())
    return std::optional<net::transport::datagram::OwnedDatagram>{};
  const auto received = transport_->receive();
  if (!received) return libchiaking::unexpected(received.error());
  if (!received.value())
    return std::optional<net::transport::datagram::OwnedDatagram>{};
  return std::optional<net::transport::datagram::OwnedDatagram>{
      std::in_place, received.value()->begin(), received.value()->end()};
}

auto Connector::connect_over_channel(HandshakeChannel& channel,
                                     platform::SecureRandomProvider& random,
                                     platform::ClockProvider& clock,
                                     platform::StopToken stop_token,
                                     std::uint64_t attempt)
    -> Expected<HandshakeCompletion, ConnectError> {
  const auto params = Handshake::generate_parameters(random);
  if (!params.has_value()) return unexpected(ConnectError::random_failed);
  return connect_over_channel(channel, params.value(), clock,
                              std::move(stop_token), attempt);
}

auto Connector::connect_over_channel(HandshakeChannel& channel,
                                     HandshakeParameters params,
                                     platform::ClockProvider& clock,
                                     platform::StopToken stop_token,
                                     std::uint64_t attempt)
    -> Expected<HandshakeCompletion, ConnectError> {
  return HandshakeRunner{Handshake{attempt, params.counter, params.header},
                         channel, clock, std::move(stop_token)}
      .run();
}

auto Connector::connect(
    std::unique_ptr<net::transport::datagram::ProviderUdpTransport> transport,
    platform::SecureRandomProvider& random, platform::ClockProvider& clock,
    platform::StopToken stop_token, std::uint64_t attempt)
    -> Expected<Connected, ConnectError> {
  if (!transport) return unexpected(ConnectError::invalid_transport);

  ProviderRudpHandshakeChannel channel{*transport};
  auto completion = connect_over_channel(channel, random, clock,
                                         std::move(stop_token), attempt);
  if (!completion.has_value()) return unexpected(completion.error());

  return Connected{std::move(transport), completion.value()};
}

auto Connector::send_switch_request(Connected connected,
                                    platform::SecureRandomProvider& random)
    -> Expected<Connected, SwitchSendError> {
  if (!connected.transport)
    return unexpected(SwitchSendError::invalid_transport);

  const auto packet = FrameCodec::encode_switch_request_random(
      connected.completion.next_local_counter, random);
  if (!packet.has_value()) return unexpected(SwitchSendError::packet_failed);

  const auto sent = connected.transport->send(packet.value());
  if (!sent.has_value()) return unexpected(SwitchSendError::send_failed);
  if (sent.value() != packet.value().size())
    return unexpected(SwitchSendError::short_send);

  connected.completion.next_local_counter =
      static_cast<std::uint16_t>(connected.completion.next_local_counter + 1U);
  return std::move(connected);
}

auto SendBuffer::can_push(std::uint16_t sequence) const noexcept -> bool {
  return size() < capacity && std::none_of(
                                  packets_.begin(), packets_.end(),
                                  [sequence](const Packet& p) {
                                    return p.acknowledgement == sequence;
                                  });
}

void SendBuffer::push(std::uint16_t sequence, std::span<const std::byte> bytes,
                      std::uint64_t now) {
  if (packets_.empty() && now >= next_poll_)
    next_poll_ = timing::Deadline::after_us(now, poll_us);
  packets_.push_back({sequence, 0, now, {bytes.begin(), bytes.end()}});
}

auto SendBuffer::acknowledge(std::uint16_t sequence) -> std::size_t {
  const auto previous = size();
  std::erase_if(packets_, [sequence](const Packet& p) {
    const auto distance =
        static_cast<std::uint16_t>(sequence - p.acknowledgement);
    return distance < sequence_half_range;
  });
  return previous - size();
}

ReliableChannel::ReliableChannel(HandshakeChannel& channel, SendBuffer& pending,
                                 platform::ClockProvider& clock,
                                 platform::DiagnosticsProvider* diagnostics)
    : channel_(channel),
      pending_(pending),
      clock_(clock),
      diagnostics_(diagnostics) {}

auto ReliableChannel::send(std::span<const std::byte> bytes)
    -> Expected<std::size_t, net::transport::datagram::TransportError> {
  auto frames = FrameCodec::decode(bytes);
  std::optional<std::uint16_t> acknowledgement;
  if (frames && frames.value().size() == 1 &&
      frames.value().front().type ==
          FrameCodec::type_value(FrameType::ctrl_message) &&
      frames.value().front().payload.size() >= 2) {
    acknowledgement = static_cast<std::uint16_t>(
        bytes::load_be<std::uint16_t>(frames.value().front().payload) + 1U);
  }
  auto sent = channel_.send(bytes);
  if (sent && sent.value() == bytes.size() && acknowledgement) {
    if (!pending_.can_push(*acknowledgement)) {
      log(platform::LogSeverity::error, "queue full or duplicate sequence",
          pending_.size());
      return sent;
    }
    pending_.push(*acknowledgement, bytes, clock_.now_microseconds());
    log(platform::LogSeverity::debug, "retained outgoing Ctrl packet",
        pending_.size());
  }
  return sent;
}

auto ReliableChannel::receive_for(std::chrono::microseconds timeout,
                                  const platform::StopToken& stop)
    -> Expected<std::optional<net::transport::datagram::OwnedDatagram>,
                net::transport::datagram::TransportError> {
  const auto duration =
      static_cast<std::uint64_t>(std::max<std::int64_t>(0, timeout.count()));
  const auto deadline =
      timing::Deadline::after_us(clock_.now_microseconds(), duration);
  for (;;) {
    if (stop.stop_requested())
      return std::optional<net::transport::datagram::OwnedDatagram>{};
    const auto now = clock_.now_microseconds();
    service_retries(now, stop);
    if (stop.stop_requested() || now >= deadline)
      return std::optional<net::transport::datagram::OwnedDatagram>{};
    const auto until = pending_.size() == 0
                           ? deadline
                           : std::min(deadline, pending_.next_poll_);
    const auto wait = std::min(
        until - now,
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
    auto received = channel_.receive_for(
        std::chrono::microseconds{static_cast<std::int64_t>(wait)}, stop);
    if (!received) return unexpected(received.error());
    if (stop.stop_requested())
      return std::optional<net::transport::datagram::OwnedDatagram>{};
    if (received.value()) {
      retire_acknowledgements(*received.value());
      return received;
    }
    if (clock_.now_microseconds() == now) return received;
  }
}

void ReliableChannel::retire_acknowledgements(
    std::span<const std::byte> bytes) {
  auto frames = FrameCodec::decode(bytes);
  if (!frames || frames.value().front().payload.size() < 4) return;
  std::uint16_t acknowledgement = 0;
  for (const auto& frame : frames.value()) {
    const auto subtype = FrameCodec::subtype_of(frame.type);
    if (subtype == Subtype::ack_ctrl_offset8 ||
        subtype == Subtype::ack_ctrl_offset6 ||
        subtype == Subtype::ack_ctrl_offset2 || subtype == Subtype::ack) {
      if (frame.payload.size() < 4) return;
      acknowledgement =
          bytes::load_be<std::uint16_t>(std::span{frame.payload}.subspan(2));
    } else {
      continue;
    }
    const auto removed = pending_.acknowledge(acknowledgement);
    if (removed)
      log(platform::LogSeverity::debug, "peer ACK retired packets", removed);
  }
}

void ReliableChannel::service_retries(std::uint64_t now,
                                      const platform::StopToken& stop) {
  if (pending_.size() == 0 || now < pending_.next_poll_) return;
  pending_.next_poll_ = timing::Deadline::after_us(now, SendBuffer::poll_us);
  for (std::size_t i = 0; i < pending_.packets_.size();) {
    if (stop.stop_requested()) return;
    auto& packet = pending_.packets_[i];
    if (now / 1000 - packet.last_send / 1000 <= SendBuffer::retry_us / 1000) {
      ++i;
      continue;
    }
    if (packet.retries >= SendBuffer::max_retries) {
      const auto removed = pending_.acknowledge(packet.acknowledgement);
      log(platform::LogSeverity::warning,
          "retry budget exhausted; retired packets", removed);
      i = 0;
      continue;
    }
    packet.last_send = now;
    ++packet.retries;
    auto sent = channel_.send(packet.bytes);
    log(sent && sent.value() == packet.bytes.size()
            ? platform::LogSeverity::info
            : platform::LogSeverity::warning,
        sent && sent.value() == packet.bytes.size()
            ? "retransmitted retained Ctrl packet"
            : "Ctrl retransmission send failed",
        packet.retries);
    ++i;
  }
}

void ReliableChannel::log(platform::LogSeverity severity,
                          std::string_view event, std::size_t count) {
  if (!diagnostics_) return;
  const auto message = std::format("remote RUDP: {}; count={} pending={}",
                                   event, count, pending_.size());
  diagnostics_->write({severity, message});
}

auto RemoteDatagramDrain::drain(
    net::transport::datagram::DatagramTransport& transport,
    platform::NetworkProvider& network, const platform::ClockProvider& clock,
    const platform::StopToken& stop)
    -> Expected<std::size_t, RemoteDatagramDrainError> {
  const auto* pollable = transport.pollable();
  if (!pollable) return std::size_t{0};
  auto poller = network.pollers().create_poller();
  if (!poller || !poller.value())
    return unexpected(RemoteDatagramDrainError::poller);
  const std::array<platform::PollRequest, 1> requests{
      {{pollable, 1, platform::PollEvent::readable}}};
  const auto started = clock.now_microseconds();
  std::size_t drained = 0;
  for (;;) {
    const auto now = clock.now_microseconds();
    if (timing::Deadline::elapsed_us(started, now) > limit_us) return drained;
    auto ready = poller.value()->wait(requests, quiet, stop);
    if (!ready) return unexpected(RemoteDatagramDrainError::poller);
    if (ready.value().stop_requested || ready.value().ready.empty())
      return drained;
    auto received = transport.receive();
    if (!received) return unexpected(RemoteDatagramDrainError::receive);
    if (received.value()) ++drained;
  }
}

}  // namespace libchiaking::net::transport::rudp
