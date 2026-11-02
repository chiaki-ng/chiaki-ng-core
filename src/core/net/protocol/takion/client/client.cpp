#include "src/core/net/protocol/takion/client.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"

#include "src/core/net/protocol/takion/client/inbound.hpp"
#include "src/core/net/protocol/takion/client/lifecycle.hpp"
#include "src/core/net/protocol/takion/client/limits.hpp"
#include "src/core/net/protocol/takion/client/outbound.hpp"
#include "src/core/net/protocol/takion/control_chunk_codec.hpp"
#include "src/core/net/protocol/takion/control_codec.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/generic_data_exchange.hpp"
#include "src/core/net/protocol/takion/handshake.hpp"
#include "src/core/net/protocol/takion/limits.hpp"
#include "src/core/net/protocol/takion/reliable_channel.hpp"
#include "src/core/utils/bytes/endian.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <boost/sml.hpp>

#include <algorithm>
#include <array>
#include <deque>
#include <initializer_list>
#include <limits>
#include <type_traits>
#include <utility>

namespace libchiaking::net::protocol::takion {
namespace sml = boost::sml;

using namespace client_detail;

Client::Client(
    std::unique_ptr<net::transport::datagram::DatagramTransport> transport,
    platform::ClockProvider& clock, platform::ThreadingProvider& threading,
    platform::SecureRandomProvider& random, EventSink& events,
    ClientConfig config)
    : owner_mutex_(threading),
      transport_(std::move(transport)),
      clock_(clock),
      random_(random),
      events_(events),
      config_(std::move(config)),
      lifecycle_context_{valid_configuration(transport_.get(), config_)},
      lifecycle_machine_{lifecycle_context_},
      handshake_(config_.handshake_retry),
      crypto_(config_.crypto_policy),
      commands_(std::max<std::size_t>(config_.limits.mailbox_capacity, 1)),
      outbound_(transport_.get(), clock_, crypto_, config_),
      inbound_{*this, clock_, crypto_, events_, control_codec_, config_},
      snapshot_mutex_(threading) {
  const auto bootstrap = resolve_takion_wire_profile(config_.bootstrap_version);
  if (bootstrap) profile_ = *bootstrap;
  if (profile_) outbound_.set_profile(&profile_.value());
  publish();
}

Client::~Client() {
  const platform::Lock lock{owner_mutex_};
  if (!lifecycle_context_.close_attempted) {
    process(StopRequested{false});
    static_cast<void>(run_effects());
  }
}

ClientResult<void> Client::start() {
  const platform::Lock lock{owner_mutex_};
  outbound_.begin_extended_epoch(clock_.now_microseconds());
  process(StartRequested{});
  auto started = run_effects();
  publish();
  return started;
}

ClientResult<void> Client::post(TakionCommand command) {
  const auto state = published_state_.load(std::memory_order_acquire);
  if (state != ClientState::handshaking && state != ClientState::connected)
    return unexpected(ClientError::invalid_state);
  if (!commands_.try_push(std::move(command)))
    return unexpected(ClientError::mailbox_full);
  return {};
}

ClientResult<void> Client::request_stop() {
  const platform::Lock lock{owner_mutex_};
  discard_commands();
  process(StopRequested{true});
  auto stopped = run_effects();
  publish();
  return stopped;
}

ClientResult<void> Client::request_stop_with_disconnect(
    Disconnect message, std::chrono::microseconds drain) {
  const platform::Lock lock{owner_mutex_};
  discard_commands();
  ClientResult<void> disconnected{};
  if (lifecycle_state() == ClientState::connected &&
      (config_.crypto_policy != CryptoPolicy::required ||
       lifecycle_context_.crypto_installed ||
       config_.allow_plain_control_until_crypto)) {
    disconnected = send_control(ControlMessage{std::move(message)});
    if (disconnected && drain.count() > 0 && reliable_ &&
        reliable_->outstanding_fragment_count() != 0)
      process(DisconnectSent{timing::Deadline::after_us(
          clock_.now_microseconds(),
          static_cast<std::uint64_t>(drain.count()))});
  }
  auto drained = drain_disconnect();
  process(StopRequested{true});
  auto closed = run_effects();
  publish();
  if (!disconnected) return disconnected;
  return drained ? closed : drained;
}

void Client::note_readiness(const platform::Pollable& polled, bool readable) {
  const platform::Lock lock{owner_mutex_};
  if (!transport_ || !transport_->readiness_exact() ||
      transport_->pollable() != &polled) {
    readiness_.reset();
    return;
  }
  readiness_ = readable;
}

std::optional<std::uint64_t> Client::next_deadline_us() const {
  const auto state = published_state_.load(std::memory_order_acquire);
  if (!commands_.empty() &&
      (state == ClientState::handshaking || state == ClientState::connected))
    return clock_.now_microseconds();
  const auto due = published_due_us_.load(std::memory_order_acquire);
  if (due == no_deadline) return std::nullopt;
  return due;
}

ClientResult<std::unique_ptr<net::transport::datagram::DatagramTransport>>
Client::release_transport() {
  const platform::Lock lock{owner_mutex_};
  if (!transport_) return unexpected(ClientError::invalid_state);
  discard_commands();
  auto released = std::move(transport_);
  process(StopRequested{true});
  auto stopped = run_effects();
  publish();
  if (!stopped) return unexpected(stopped.error());
  return released;
}

bool Client::crypto_installed() const {
  return published_crypto_installed_.load(std::memory_order_acquire);
}

Snapshot Client::snapshot() const {
  Snapshot copy;
  {
    const platform::Lock lock{snapshot_mutex_};
    copy = published_snapshot_;
  }
  copy.queued_commands = queued_command_count();
  return copy;
}

void Client::publish() {
  published_crypto_installed_.store(lifecycle_context_.crypto_installed,
                                    std::memory_order_release);
  published_due_us_.store(due_us().value_or(no_deadline),
                          std::memory_order_release);
  const auto current = gather_snapshot();
  const platform::Lock lock{snapshot_mutex_};
  published_snapshot_ = current;
}

void Client::discard_commands() {
  TakionCommand discarded;
  while (commands_.try_pop(discarded)) {
  }
}

std::size_t Client::queued_command_count() const noexcept {
  return static_cast<std::size_t>(
      std::max<std::ptrdiff_t>(commands_.size(), 0));
}

Snapshot Client::gather_snapshot() const {
  return Snapshot{
      lifecycle_state(),
      profile_ ? std::optional<std::uint32_t>{profile_->version()}
               : std::nullopt,
      lifecycle_context_.profile_committed,
      lifecycle_context_.crypto_installed,
      0,
      reliable_ ? reliable_->outstanding_fragment_count() : 0,
      reliable_ ? reliable_->has_incomplete_message() : false,
      lifecycle_context_.close_attempted,
      untracked_fragments_,
      link_metrics(),
      transport_ ? transport_->kernel_receive_buffer() : std::nullopt,
      outbound_.unsent_count(),
  };
}

LinkMetrics Client::link_metrics() const noexcept {
  LinkMetrics link;
  if (reliable_ && reliable_->round_trip().samples() != 0) {
    const auto& round_trip = reliable_->round_trip();
    link.round_trip = RoundTripMetrics{
        round_trip.smoothed_ms() * microseconds_per_millisecond,
        round_trip.variation_ms() * microseconds_per_millisecond,
        reliable_->round_trip_spread(clock_.now_microseconds())};
  }
  if (!profile_ || !lifecycle_context_.profile_committed) return link;
  const auto features = profile_->features();
  link.features = features.bits();
  if (features.contains(Feature::extended_packet_headers)) {
    const auto& extension = inbound_.extension_metrics();
    link.extension =
        ExtensionMetrics{extension.jitter_us(), extension.received(),
                         extension.lost(), extension.loss_ratio()};
    const auto& collector = inbound_.video_metrics();
    const auto& snapshot = collector.snapshot();
    const bool reports = features.contains(Feature::video_frame_metrics);
    link.video = VideoMetrics{reports,
                              collector.windows().throughput_bps(),
                              collector.windows().loss_ratio(),
                              snapshot.send_int_us,
                              snapshot.recv_int_us,
                              snapshot.transfer_time_us,
                              snapshot.transmission_time_us,
                              snapshot.recv_size,
                              snapshot.frame_no_wide,
                              snapshot.is_trans_time_estimated,
                              reports && video_report_worker_
                                  ? video_report_worker_->sequence()
                                  : 0U};
  }
  const auto& tos = inbound_.tos_metrics();
  if (features.contains(Feature::received_tos_metrics) && tos.started()) {
    link.tos = TosMetrics{
        .last = tos.last_traffic_class().transform([](std::uint8_t last) {
          return TrafficClass{static_cast<std::uint8_t>(last >> 2U),
                              static_cast<std::uint8_t>(last & 0x3U)};
        }),
        .packets = tos.total_packets(),
        .reports = tos.reports(),
        .dscp_counts = tos.total_dscp_counts(),
        .ecn_counts = tos.total_ecn_counts(),
    };
  }
  return link;
}

ClientResult<void> Client::flush_commands() {
  const platform::Lock lock{owner_mutex_};
  auto flushed = flush();
  publish();
  return flushed;
}

ClientResult<void> Client::flush() {
  const auto state = lifecycle_state();
  if (state == ClientState::idle || state == ClientState::stopping)
    return unexpected(ClientError::invalid_state);
  if (state == ClientState::closed || state == ClientState::disconnecting)
    return {};
  if (state == ClientState::failed)
    return unexpected(
        lifecycle_context_.terminal_error.value_or(ClientError::invalid_state));
  if (auto retried = retry_unsent(); !retried) return retried;
  return drain_commands();
}

ClientResult<void> Client::pump_once() {
  const platform::Lock lock{owner_mutex_};
  auto pumped = pump();
  publish();
  return pumped;
}

ClientResult<void> Client::retry_unsent() {
  const auto retried = outbound_.retry_unsent(clock_.now_microseconds());
  if (!retried) return fail_terminal(retried.error());
  for (std::size_t count = 0; count < retried->expired; ++count)
    reject(ClientError::send_blocked);
  for (std::size_t count = 0; count < retried->dropped; ++count)
    reject(ClientError::send_dropped);
  return {};
}

ClientResult<void> Client::pump() {
  const auto readable = readiness_;
  const auto state = lifecycle_state();
  if (state == ClientState::idle) return unexpected(ClientError::invalid_state);
  if (state == ClientState::stopping)
    return unexpected(ClientError::invalid_state);
  if (state == ClientState::closed) return {};
  if (state == ClientState::failed)
    return unexpected(
        lifecycle_context_.terminal_error.value_or(ClientError::invalid_state));
  if (state == ClientState::disconnecting) return service_drain();

  if (auto retried = retry_unsent(); !retried) return retried;
  auto drained = drain_commands();
  if (!drained || lifecycle_state() == ClientState::closed) return drained;

  if (lifecycle_context_.crypto_installed && inbound_.postponed_count() != 0) {
    inbound_.replay_postponed(inbound_session(InboundScope::all));
    if (lifecycle_state() == ClientState::failed)
      return unexpected(lifecycle_context_.terminal_error.value());
  }

  const auto now = clock_.now_microseconds();
  if (lifecycle_state() == ClientState::handshaking) {
    const auto snapshot = handshake_.snapshot();
    if (snapshot.deadline.monotonic_microseconds != 0 &&
        now >= snapshot.deadline.monotonic_microseconds) {
      auto result = handle_handshake_action(handshake_.handle(
          Timeout{{snapshot.generation, snapshot.attempt}, now}));
      if (!result) return fail_terminal(result.error());
    }
  }

  if (lifecycle_state() == ClientState::connected &&
      lifecycle_context_.profile_committed) {
    generic_data_.poll(now);
    if (auto result = run_generic_data_actions(); !result)
      return fail_terminal(result.error());
  }

  if (auto resent = resend_due(now); !resent) return resent;

  run_video_frame_reports(now);
  enable_received_tos_metrics(now);

  if (!readable.value_or(true)) return finish_pass();
  const auto received = receive_datagrams(
      InboundScope::all, config_.limits.maximum_datagrams_per_pump);
  if (!received) return unexpected(received.error());
  if (readable) readiness_ = received.value() == ReceivePass::budget_spent;
  if (received.value() == ReceivePass::budget_spent && !readable) return {};
  return finish_pass();
}

ClientResult<void> Client::finish_pass() {
  inbound_.expire_negotiation(clock_.now_microseconds());
  if (const auto ready = inbound_.take_ready_commit(reliable_.get())) {
    auto committed = commit_profile(*ready);
    if (!committed) return fail_terminal(committed.error());
  }
  return {};
}

ClientResult<void> Client::resend_due(std::uint64_t now) {
  if (!reliable_) return {};
  const auto poll = reliable_->poll(now);
  for (const auto& fragment : poll.retransmissions) {
    auto result = send_fragment(fragment);
    if (!result && !is_transient_send_error(result.error()))
      return fail_terminal(result.error());
    if (!result) reject(result.error());
  }
  for (std::size_t index = 0; index < poll.exhausted_sequences.size(); ++index)
    reject(ClientError::reliable_channel_failure);
  return {};
}

ClientResult<Client::ReceivePass> Client::receive_datagrams(InboundScope scope,
                                                            std::size_t limit) {
  for (std::size_t count = 0; count < limit; ++count) {
    auto received = transport_->receive();
    if (!received) {
      if (received.error() ==
          net::transport::datagram::TransportError::would_block)
        return ReceivePass::drained;
      if (received.error() ==
              net::transport::datagram::TransportError::io_failure &&
          lifecycle_state() == ClientState::handshaking) {
        const auto snapshot = handshake_.snapshot();
        auto retried = handle_handshake_action(
            handshake_.handle(Timeout{{snapshot.generation, snapshot.attempt},
                                      clock_.now_microseconds(),
                                      true}));
        if (!retried) return unexpected(fail_terminal(retried.error()).error());
        return ReceivePass::drained;
      }
      return unexpected(fail_terminal(ClientError::transport_failure).error());
    }
    if (!received.value()) return ReceivePass::drained;
    if (scope == InboundScope::all) account_received_tos(*received.value());
    inbound_.process_datagram(*received.value(), inbound_session(scope));
    if (lifecycle_state() == ClientState::failed)
      return unexpected(lifecycle_context_.terminal_error.value());
  }
  return ReceivePass::budget_spent;
}

InboundSession Client::inbound_session(InboundScope scope) const {
  return InboundSession{
      profile_ ? &profile_.value() : nullptr,
      established_ ? &established_.value() : nullptr,
      reliable_.get(),
      lifecycle_context_.crypto_installed,
      scope,
  };
}

ClientResult<void> Client::drain_disconnect() {
  std::size_t still_passes = 0;
  auto previous = clock_.now_microseconds();
  while (lifecycle_state() == ClientState::disconnecting) {
    if (auto serviced = service_drain(); !serviced) return serviced;
    if (lifecycle_state() != ClientState::disconnecting) break;
    const auto now = clock_.now_microseconds();
    still_passes = now == previous ? still_passes + 1 : 0;
    previous = now;
    if (still_passes >= drain_still_pass_limit) return settle_drain();
    const auto due = due_us().value_or(now);
    const auto waited = transport_->wait_readable(
        std::chrono::microseconds{
            due > now ? static_cast<std::int64_t>(due - now) : 0},
        platform::StopToken{});
    if (!waited) return settle_drain();
  }
  return {};
}

ClientResult<void> Client::service_drain() {
  if (!transport_) return settle_drain();
  if (auto retried = retry_unsent(); !retried) return retried;
  if (auto resent = resend_due(clock_.now_microseconds()); !resent)
    return resent;
  if (const auto received = receive_datagrams(
          InboundScope::control, config_.limits.maximum_datagrams_per_pump);
      !received)
    return unexpected(received.error());
  if (lifecycle_state() != ClientState::disconnecting ||
      !lifecycle_context_.drain)
    return {};
  if (!reliable_ || reliable_->outstanding_fragment_count() == 0 ||
      clock_.now_microseconds() >= lifecycle_context_.drain->deadline_us)
    return settle_drain();
  return {};
}

ClientResult<void> Client::settle_drain() {
  if (!lifecycle_context_.drain) return {};
  process(DisconnectSettled{lifecycle_context_.drain->id});
  return run_effects();
}

std::optional<std::uint64_t> Client::due_us() const {
  const auto state = lifecycle_state();
  const auto now = clock_.now_microseconds();
  std::optional<std::uint64_t> due;
  const auto consider = [&due](std::optional<std::uint64_t> at) noexcept {
    due = timing::Deadline::earliest(due, at);
  };
  if (reliable_ && reliable_->queued_fragment_count() != 0)
    consider(reliable_->next_poll_us());
  consider(outbound_.next_retry_us());
  if (state == ClientState::disconnecting) {
    if (lifecycle_context_.drain)
      consider(lifecycle_context_.drain->deadline_us);
    return due;
  }
  if (state != ClientState::handshaking && state != ClientState::connected)
    return std::nullopt;
  if (!commands_.empty() ||
      (lifecycle_context_.crypto_installed && inbound_.postponed_count() != 0))
    return now;
  if (state == ClientState::handshaking) {
    const auto deadline = handshake_.snapshot().deadline.monotonic_microseconds;
    if (deadline != 0) consider(deadline);
    return due;
  }
  if (!lifecycle_context_.profile_committed || !profile_) return due;
  consider(generic_data_.next_poll_us());
  const auto features = profile_->features();
  if (lifecycle_context_.crypto_installed) {
    if (features.contains(Feature::video_frame_metrics)) {
      if (!video_report_worker_ || inbound_.video_metrics().signalled())
        return now;
      consider(video_report_worker_->deadline_us());
    }
    if (features.contains(Feature::received_tos_metrics) &&
        !tos_enable_attempted_ && !inbound_.tos_metrics().started())
      return now;
  }
  return due;
}

ClientState Client::lifecycle_state() const {
  if (lifecycle_machine_.is(sml::state<IdleState>)) return ClientState::idle;
  if (lifecycle_machine_.is(sml::state<HandshakingState>))
    return ClientState::handshaking;
  if (lifecycle_machine_.is(sml::state<ConnectedState>))
    return ClientState::connected;
  if (lifecycle_machine_.is(sml::state<DisconnectingState>))
    return ClientState::disconnecting;
  if (lifecycle_machine_.is(sml::state<StoppingState>))
    return ClientState::stopping;
  if (lifecycle_machine_.is(sml::state<ClosedState>))
    return ClientState::closed;
  return ClientState::failed;
}

ClientResult<void> Client::drain_commands() {
  TakionCommand command;
  for (std::size_t count = 0; count < config_.limits.maximum_commands_per_pump;
       ++count) {
    if (!commands_.try_pop(command)) break;
    auto result = process_command(std::move(command));
    if (!result) return result;
    if (lifecycle_state() == ClientState::closed) return {};
  }
  return {};
}

ClientResult<void> Client::fail_terminal(ClientError error) {
  process(TerminalFailure{error});
  static_cast<void>(run_effects());
  return unexpected(error);
}

ClientResult<void> Client::run_effects() {
  std::optional<ClientError> refusal;
  while (!lifecycle_context_.effects.empty()) {
    auto effect = std::move(lifecycle_context_.effects.front());
    lifecycle_context_.effects.pop_front();
    if (auto outcome = std::visit(
            [this](auto&& value) { return run_effect(std::move(value)); },
            std::move(effect)))
      refusal = outcome;
  }
  if (refusal) return unexpected(*refusal);
  return {};
}

std::optional<ClientError> Client::run_effect(GenerateStartMaterial) {
  std::array<std::byte, sizeof(std::uint32_t)> entropy{};
  if (!random_.fill_bytes(entropy)) {
    process(StartMaterialFailed{ClientError::random_failure});
    return std::nullopt;
  }
  const auto local_tag =
      std::max(bytes::load_be<std::uint32_t>(entropy), std::uint32_t{1});
  process(StartMaterialReady{
      VerificationTag{local_tag},
      InitialSequenceNumber{local_tag},
      clock_.now_microseconds(),
  });
  return std::nullopt;
}

std::optional<ClientError> Client::run_effect(BeginHandshake handshake) {
  const auto action = handshake_.handle(Start{
      handshake.local_tag,
      handshake.local_initial_sequence_number,
      handshake.now_microseconds,
  });
  auto result = handle_handshake_action(action);
  if (result) return std::nullopt;
  if (std::holds_alternative<SendInit>(action))
    process(OpeningSendFailed{result.error()});
  else
    process(TerminalFailure{result.error()});
  return std::nullopt;
}

std::optional<ClientError> Client::run_effect(ActivateSession activation) {
  if (auto result = activate_session(activation.handshake); !result)
    process(TerminalFailure{result.error()});
  return std::nullopt;
}

std::optional<ClientError> Client::run_effect(TakionCommand command) {
  return std::visit([this](auto&& value) { return execute(std::move(value)); },
                    std::move(command));
}

std::optional<ClientError> Client::run_effect(CloseTransport close) {
  if (transport_ && !transport_->close()) {
    process(TerminalFailure{ClientError::transport_failure});
    return std::nullopt;
  }
  process(CloseCompleted{close.emit_closed});
  return std::nullopt;
}

std::optional<ClientError> Client::run_effect(PublishTerminalFailure failure) {
  events_.on_event(client_event::TerminalFailure{failure.error});
  if (!lifecycle_context_.close_attempted) {
    lifecycle_context_.close_attempted = true;
    lifecycle_context_.effects.emplace_back(CloseTransport{false});
  }
  return std::nullopt;
}

std::optional<ClientError> Client::run_effect(PublishClosed) {
  events_.on_event(client_event::Closed{});
  return std::nullopt;
}

std::optional<ClientError> Client::run_effect(RefuseRequest refusal) {
  return refusal.error;
}

ClientResult<void> Client::activate_session(
    const EstablishedHandshake& handshake) {
  established_ = handshake;
  const auto active =
      profile_ ? client_detail::ClientLimits::active_profile_control_capacity(
                     config_.limits, *profile_, config_.envelope)
               : std::optional<std::size_t>{};
  if (!active) return unexpected(ClientError::invalid_configuration);
  ReliableChannelConfig channel_config{
      .initial_local_sequence = handshake.local_initial_sequence_number.value,
      .initial_remote_sequence = handshake.remote_initial_sequence_number.value,
      .maximum_message_size = config_.limits.maximum_message_size,
      .maximum_fragment_size = *active,
      .maximum_fragment_count = config_.limits.maximum_fragment_count,
      .retransmission_window_size = config_.limits.retransmission_window_size,
      .receive_reorder_size = config_.limits.receive_reorder_size,
      .retransmission_timeout_us = config_.limits.retransmission_timeout_us,
      .maximum_retries = config_.limits.maximum_retries,
      .minimum_retransmission_timeout_us =
          config_.limits.minimum_retransmission_timeout_us,
      .maximum_retransmission_timeout_us =
          config_.limits.maximum_retransmission_timeout_us,
      .initial_peer_receive_window =
          handshake.remote_advertised_receive_window.value,
  };
  const auto reliable_bound =
      client_detail::ClientLimits::reliable_control_capacity(channel_config);
  const auto codec_limit =
      reliable_bound
          ? client_detail::ClientLimits::effective_control_message_limit(
                config_.limits.maximum_message_size, *reliable_bound,
                ControlCodec{}.effective_message_size_limit())
          : std::optional<std::size_t>{};
  if (!reliable_bound || !codec_limit)
    return unexpected(ClientError::invalid_configuration);
  auto reliable = ReliableChannel::create(channel_config);
  if (!reliable) return unexpected(ClientError::invalid_configuration);
  reliable_ = std::make_unique<ReliableChannel>(std::move(reliable).value());
  control_codec_ = ControlCodec{*codec_limit};
  events_.on_event(client_event::Connected{handshake});
  if (const auto* fixed =
          std::get_if<FixedTakionProfile>(&config_.profile_selection))
    return commit_profile(fixed->version);
  const auto& versions =
      std::get<NegotiatedTakionProfiles>(config_.profile_selection)
          .supported_versions;
  inbound_.note_version_request_sent(clock_.now_microseconds());
  return send_control(ProtocolRequest{versions});
}

ClientResult<void> Client::install_crypto(InstallCrypto command) {
  if (!command.provider) return unexpected(ClientError::crypto_failure);
  if (auto result =
          crypto_.install_local(*command.provider, std::move(command.local));
      !result)
    return unexpected(ClientError::crypto_failure);
  if (config_.source_tap && profile_) {
    if (auto keys = command.provider->export_receive_keys(command.remote)) {
      config_.source_tap->on_source_keys(profile_->version(), keys.value());
    }
  }
  if (auto result =
          crypto_.install_remote(*command.provider, std::move(command.remote));
      !result)
    return unexpected(ClientError::crypto_failure);
  if (auto result = crypto_.set_local_key_position(command.local_key_position);
      !result)
    return unexpected(ClientError::crypto_failure);
  crypto_.set_remote_key_position(command.remote_key_position);
  lifecycle_context_.crypto_installed = true;
  outbound_.set_crypto_installed(true);
  return flush_deferred_acks();
}

ClientResult<void> Client::process_command(TakionCommand command) {
  if (std::holds_alternative<Stop>(command)) {
    process(StopRequested{true});
    return run_effects();
  }
  const auto state = lifecycle_state();
  const auto handshaking = state == ClientState::handshaking;
  const auto connected = state == ClientState::connected;
  if (handshaking || connected) {
    const auto* install = std::get_if<InstallCrypto>(&command);
    const bool needs_profile =
        !install && !std::holds_alternative<SendCongestion>(command);
    if ((install && !install->provider) ||
        (needs_profile &&
         (!connected || !lifecycle_context_.profile_committed))) {
      process(TerminalFailure{install ? ClientError::crypto_failure
                                      : ClientError::invalid_state});
      return run_effects();
    }
    lifecycle_context_.effects.emplace_back(std::move(command));
    return run_effects();
  }
  ++lifecycle_context_.unexpected_events;
  if (state == ClientState::closed || state == ClientState::failed) {
    lifecycle_context_.record_terminal_failure(ClientError::invalid_state);
    return run_effects();
  }
  process(TerminalFailure{ClientError::invalid_state});
  return run_effects();
}

std::optional<ClientError> Client::execute(SendControl command) {
  outbound_.set_send_deadline(command.send_deadline_us);
  auto result = send_control(command.message, command.operation_id);
  outbound_.set_send_deadline(std::nullopt);
  return settle_send(std::move(result), {ClientError::control_codec_failure,
                                         ClientError::crypto_not_installed});
}

std::optional<ClientError> Client::execute(SendProbe command) {
  auto result = outbound_.send_probe(command);
  const auto sent_at = outbound_.last_send_attempt_us();
  if (!result && is_transient_send_error(result.error())) {
    reject(result.error());
    if (command.operation_id)
      events_.on_event(
          client_event::ProbeSent{*command.operation_id, sent_at, false,
                                  result.error() == ClientError::send_blocked});
    return std::nullopt;
  }
  if (result && command.operation_id)
    events_.on_event(client_event::ProbeSent{*command.operation_id, sent_at});
  return settle_send(std::move(result), {ClientError::packet_encode_failure});
}

std::optional<ClientError> Client::execute(SendCongestion command) {
  return settle_send(outbound_.send_congestion(command),
                     {ClientError::packet_encode_failure});
}

std::optional<ClientError> Client::execute(SendFeedbackState command) {
  return settle_send(outbound_.send_feedback_state(command),
                     {ClientError::packet_encode_failure});
}

std::optional<ClientError> Client::execute(SendFeedbackHistory command) {
  return settle_send(outbound_.send_feedback_history(command),
                     {ClientError::packet_encode_failure});
}

std::optional<ClientError> Client::execute(SendMicrophone command) {
  return settle_send(outbound_.send_microphone(command),
                     {ClientError::packet_encode_failure});
}

std::optional<ClientError> Client::execute(SendLengthPrefixed command) {
  return settle_send(
      outbound_.send_length_prefixed(command.payload),
      {ClientError::packet_encode_failure, ClientError::crypto_not_installed});
}

std::optional<ClientError> Client::execute(SendTrace command) {
  return settle_send(
      outbound_.send_trace(command.sequence),
      {ClientError::packet_encode_failure, ClientError::crypto_not_installed});
}

std::optional<ClientError> Client::execute(SendGenericData command) {
  return settle_send(
      submit_generic_data(std::move(command)),
      {ClientError::packet_encode_failure, ClientError::crypto_not_installed});
}

std::optional<ClientError> Client::execute(SendExtendedMessage command) {
  return settle_send(
      send_extended_message(command.message),
      {ClientError::packet_encode_failure, ClientError::unsupported_version});
}

std::optional<ClientError> Client::execute(InstallCrypto command) {
  return settle_send(install_crypto(std::move(command)), {});
}

std::optional<ClientError> Client::execute(Stop) { return std::nullopt; }

ClientResult<void> Client::handle_handshake_action(
    const HandshakeAction& action) {
  return std::visit(
      [this](const auto& value) -> ClientResult<void> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, SendInit>) {
          const ControlHeader header{VerificationTag{0}, {}, 0, 0};
          return outbound_.send_chunk(InitChunk{header, value.values}, true);
        } else if constexpr (std::is_same_v<T, SendCookie>) {
          const ControlHeader header{value.remote_tag, {}, 0, 0};
          return outbound_.send_chunk(CookieChunk{header, value.cookie}, true);
        } else if constexpr (std::is_same_v<T, Connected>) {
          process(HandshakeConnected{value.handshake});
          return {};
        } else if constexpr (std::is_same_v<T, Failed>) {
          return unexpected(ClientError::handshake_failure);
        } else if constexpr (std::is_same_v<T, Rejected>) {
          reject(ClientError::handshake_failure);
          return {};
        } else {
          return {};
        }
      },
      action);
}

ClientResult<void> Client::send_fragment(const OutboundFragment& fragment,
                                         std::size_t encrypted_tail) {
  if (!established_) return unexpected(ClientError::invalid_state);
  if (!fragment.datagram.empty())
    return outbound_.resend_datagram(fragment.datagram.bytes());
  auto datagram =
      outbound_.send_fragment(fragment, *established_, encrypted_tail);
  if (!datagram) return unexpected(datagram.error());
  if (reliable_)
    reliable_->record_datagram(fragment.sequence, datagram->bytes());
  return {};
}

ClientResult<void> Client::send_ack(
    const DataAcknowledgement& acknowledgement) {
  if (!established_) return unexpected(ClientError::invalid_state);
  if (acks_await_crypto()) {
    if (deferred_acks_.size() >= config_.limits.receive_reorder_size)
      deferred_acks_.pop_front();
    deferred_acks_.push_back(acknowledgement);
    return {};
  }
  return tolerate_transient_send(
      outbound_.send_ack(acknowledgement, *established_));
}

bool Client::acks_await_crypto() const noexcept {
  return config_.crypto_policy == CryptoPolicy::required &&
         config_.allow_plain_control_until_crypto &&
         lifecycle_context_.profile_committed &&
         !lifecycle_context_.crypto_installed;
}

ClientResult<void> Client::flush_deferred_acks() {
  while (!deferred_acks_.empty()) {
    const auto acknowledgement = deferred_acks_.front();
    deferred_acks_.pop_front();
    if (auto result = send_ack(acknowledgement); !result) {
      deferred_acks_.clear();
      return result;
    }
  }
  return {};
}

ClientResult<void> Client::tolerate_transient_send(ClientResult<void> result) {
  if (!result && is_transient_send_error(result.error())) {
    reject(result.error());
    return {};
  }
  return result;
}

std::optional<ClientError> Client::settle_send(
    ClientResult<void> result, std::initializer_list<ClientError> recoverable) {
  if (result) return std::nullopt;
  if (is_transient_send_error(result.error())) {
    reject(result.error());
    return std::nullopt;
  }
  if (std::ranges::contains(recoverable, result.error())) return result.error();
  process(TerminalFailure{result.error()});
  return std::nullopt;
}

ClientResult<void> Client::submit_generic_data(SendGenericData command) {
  if (!profile_ || !TypedMessageCodec::generic_data_enabled(*profile_))
    return unexpected(ClientError::packet_encode_failure);
  auto submitted =
      generic_data_.submit(std::move(command.data), std::move(command.metadata),
                           clock_.now_microseconds());
  if (!submitted) return unexpected(ClientError::packet_encode_failure);
  events_.on_event(client_event::GenericDataSubmitted{submitted.value()});
  return run_generic_data_actions();
}

ClientResult<void> Client::run_generic_data_actions() {
  for (auto& action : generic_data_.take_actions()) {
    auto result = std::visit(
        [this](auto& value) -> ClientResult<void> {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, GenericDataSendControl>) {
            if (config_.crypto_policy == CryptoPolicy::required &&
                !lifecycle_context_.crypto_installed)
              return unexpected(ClientError::crypto_not_installed);
            auto encoded =
                TypedMessageCodec::encode_generic_data_direct_message(
                    value.control);
            if (!encoded) return unexpected(ClientError::control_codec_failure);
            return send_protobuf(encoded.value(), config_.control_channel,
                                 std::nullopt,
                                 MessageFragmentation::single_datagram);
          } else if constexpr (std::is_same_v<T, GenericDataSendChunk>) {
            return outbound_.send_generic_data_chunk(value.chunk);
          } else if constexpr (std::is_same_v<T, GenericDataDelivered>) {
            events_.on_event(client_event::GenericDataReceived{
                value.transfer_id, std::move(value.metadata),
                std::move(value.data)});
            return {};
          } else {
            events_.on_event(client_event::GenericDataTransferEnded{
                value.transfer_id, value.outcome});
            return {};
          }
        },
        action);
    if (result) continue;
    if (is_transient_send_error(result.error()) ||
        result.error() == ClientError::packet_encode_failure ||
        result.error() == ClientError::control_codec_failure ||
        result.error() == ClientError::crypto_not_installed) {
      reject(result.error());
      continue;
    }
    return result;
  }
  return {};
}

ClientResult<void> Client::send_control(
    const ControlMessage& message,
    std::optional<ControlOperationId> operation_id) {
  if (!reliable_) return unexpected(ClientError::invalid_state);
  if (config_.crypto_policy == CryptoPolicy::required &&
      !lifecycle_context_.crypto_installed &&
      !(config_.allow_plain_control_until_crypto &&
        (std::holds_alternative<Big>(message) ||
         std::holds_alternative<ProtocolRequest>(message) ||
         std::holds_alternative<Disconnect>(message))))
    return unexpected(ClientError::crypto_not_installed);
  auto encoded = control_codec_.encode(message);
  if (!encoded) return unexpected(ClientError::control_codec_failure);
  const auto* big = std::get_if<Big>(&message);
  if (!big && encoded.value().size() >
                  std::numeric_limits<std::uint16_t>::max() -
                      control_length_bias - first_fragment_overhead)
    return unexpected(ClientError::control_codec_failure);
  const auto channel =
      std::holds_alternative<Senkusha>(message)
          ? config_.senkusha_control_channel
      : std::holds_alternative<StreamInfoAcknowledgement>(message)
          ? config_.stream_info_ack_channel
      : (std::holds_alternative<CorruptFrame>(message) ||
         std::holds_alternative<IdrRequest>(message))
          ? feedback_channel_id
          : config_.control_channel;
  return send_protobuf(encoded.value().bytes(), channel, operation_id,
                       big ? MessageFragmentation::split_to_fragment_size
                           : MessageFragmentation::single_datagram);
}

ClientResult<void> Client::send_protobuf(
    std::span<const std::byte> encoded, std::uint16_t channel,
    std::optional<ControlOperationId> operation_id,
    MessageFragmentation fragmentation) {
  if (!reliable_) return unexpected(ClientError::invalid_state);
  auto sent = reliable_->send(
      channel,
      MessageDataType{static_cast<std::uint8_t>(MessageDataKind::protobuf)},
      encoded, clock_.now_microseconds(),
      operation_id ? std::optional<std::uint64_t>{operation_id->value}
                   : std::nullopt,
      fragmentation);
  if (!sent) return unexpected(ClientError::reliable_channel_failure);
  const auto encrypted_tail =
      sent->fragments.size() == 1 && profile_ &&
              profile_->features().contains(Feature::direct_message_encryption)
          ? TypedMessageCodec::direct_message_data_size(encoded)
          : std::size_t{};
  const auto sent_at = clock_.now_microseconds();
  bool delivered = true;
  untracked_fragments_ += sent->untracked_fragments;
  for (const auto& fragment : sent->fragments) {
    auto result = send_fragment(fragment, encrypted_tail);
    if (!result && is_transient_send_error(result.error())) {
      events_.on_event(
          client_event::PacketRejected{result.error(), operation_id});
      delivered = false;
      continue;
    }
    if (!result) return result;
  }
  if (operation_id && delivered)
    events_.on_event(client_event::ControlSent{*operation_id, sent_at});
  return {};
}

ClientResult<void> Client::inbound_handle_init_ack(
    const InitAcknowledgementChunk& chunk) {
  const auto snapshot = handshake_.snapshot();
  auto result =
      handle_handshake_action(handshake_.handle(InitAcknowledgementReceived{
          {snapshot.generation, snapshot.attempt},
          chunk.header.verification_tag,
          chunk.values,
          clock_.now_microseconds(),
      }));
  if (!result) return result;
  static_cast<void>(run_effects());
  return {};
}

ClientResult<void> Client::inbound_handle_cookie_ack(
    const CookieAcknowledgementChunk& chunk) {
  const auto snapshot = handshake_.snapshot();
  auto result =
      handle_handshake_action(handshake_.handle(CookieAcknowledgementReceived{
          {snapshot.generation, snapshot.attempt},
          chunk.header.verification_tag,
          clock_.now_microseconds(),
      }));
  if (!result) return result;
  static_cast<void>(run_effects());
  return {};
}

ClientResult<void> Client::inbound_send_ack(
    const DataAcknowledgement& acknowledgement) {
  return send_ack(acknowledgement);
}

ClientResult<void> Client::inbound_send_control(const ControlMessage& message) {
  return send_control(message);
}

ClientResult<void> Client::inbound_send_fragment(
    const OutboundFragment& fragment) {
  return tolerate_transient_send(send_fragment(fragment));
}

void Client::enable_received_tos_metrics(std::uint64_t now) {
  auto& metrics = inbound_.tos_metrics();
  if (metrics.started() || tos_enable_attempted_ ||
      lifecycle_state() != ClientState::connected || !profile_ ||
      !lifecycle_context_.profile_committed ||
      !lifecycle_context_.crypto_installed ||
      !profile_->features().contains(Feature::received_tos_metrics))
    return;
  tos_enable_attempted_ = true;
  if (transport_ && transport_->enable_traffic_class()) metrics.start(now);
}

void Client::account_received_tos(std::span<const std::byte> datagram) {
  auto& metrics = inbound_.tos_metrics();
  if (!metrics.started() || datagram.empty() || !transport_) return;
  const auto traffic_class = transport_->last_traffic_class();
  if (!traffic_class) return;
  const auto report =
      metrics.account(std::to_integer<std::uint8_t>(datagram.front()),
                      *traffic_class, clock_.now_microseconds());
  if (!report) return;
  if (const auto encoded =
          ExtMessageCodec::encode_received_tos_metrics(*report))
    static_cast<void>(send_extended_message(*encoded));
}

void Client::run_video_frame_reports(std::uint64_t now) {
  if (lifecycle_state() != ClientState::connected || !profile_ ||
      !lifecycle_context_.profile_committed ||
      !lifecycle_context_.crypto_installed ||
      !profile_->features().contains(Feature::video_frame_metrics))
    return;
  auto& collector = inbound_.video_metrics();
  if (!video_report_worker_) {
    video_report_worker_.emplace(now);
    return;
  }
  if (!video_report_worker_->due(now, collector.take_signal())) return;
  auto snapshot = collector.snapshot();
  snapshot.loss_ratio =
      std::min(snapshot.loss_ratio, config_.video_frame_loss_max);
  if (const auto encoded = ExtMessageCodec::encode_video_frame_metrics(
          video_report_worker_->report(snapshot, now)))
    static_cast<void>(send_extended_message(*encoded));
  video_report_worker_->complete(snapshot, now);
}

ClientResult<void> Client::send_extended_message(
    std::span<const std::byte> message) {
  if (!profile_ || !profile_->features().contains(Feature::extended_messages))
    return unexpected(ClientError::unsupported_version);
  return outbound_.send_extended_message(message);
}

ClientResult<void> Client::inbound_send_extended_message(
    std::span<const std::byte> message) {
  return tolerate_transient_send(outbound_.send_extended_message(message));
}

ClientResult<void> Client::inbound_generic_data_control(
    const GenericDataControl& control) {
  if (!generic_data_.receive_control(control, clock_.now_microseconds()))
    reject(ClientError::packet_decode_failure);
  return run_generic_data_actions();
}

ClientResult<void> Client::inbound_generic_data_chunk(GenericDataChunk chunk) {
  if (!generic_data_.receive_chunk(chunk))
    reject(ClientError::packet_decode_failure);
  return run_generic_data_actions();
}

void Client::inbound_fail_terminal(ClientError error) {
  static_cast<void>(fail_terminal(error));
}

bool Client::inbound_connected() const {
  return lifecycle_state() == ClientState::connected;
}

ClientResult<void> Client::commit_profile(std::uint32_t version) {
  const auto resolved = resolve_takion_wire_profile(version);
  if (!resolved) return unexpected(ClientError::unsupported_version);

  const bool retain_bootstrap =
      config_.negotiation_wire_policy ==
          NegotiationWirePolicy::retain_bootstrap_profile &&
      std::holds_alternative<NegotiatedTakionProfiles>(
          config_.profile_selection);
  const auto* wire_profile =
      retain_bootstrap && profile_ ? &profile_.value() : &resolved.value();

  const auto active =
      client_detail::ClientLimits::active_profile_control_capacity(
          config_.limits, *wire_profile, config_.envelope);
  if (!active || !reliable_)
    return unexpected(ClientError::invalid_configuration);
  auto channel_config = reliable_->config();
  if (channel_config.maximum_fragment_size != *active)
    return unexpected(ClientError::invalid_configuration);
  const auto reliable_bound =
      client_detail::ClientLimits::reliable_control_capacity(channel_config);
  const auto codec_limit =
      reliable_bound
          ? client_detail::ClientLimits::effective_control_message_limit(
                config_.limits.maximum_message_size, *reliable_bound,
                ControlCodec{}.effective_message_size_limit())
          : std::optional<std::size_t>{};
  if (!reliable_bound || !codec_limit)
    return unexpected(ClientError::invalid_configuration);

  if (!retain_bootstrap) {
    profile_ = *resolved;
    outbound_.set_profile(&profile_.value());
    control_codec_ = ControlCodec{*codec_limit};
  }
  lifecycle_context_.profile_committed = true;
  events_.on_event(client_event::ProfileCommitted{resolved->version(),
                                                  resolved->features()});
  return {};
}

void Client::reject(ClientError error) {
  events_.on_event(client_event::PacketRejected{error});
}

bool Client::valid_configuration(
    const net::transport::datagram::DatagramTransport* transport,
    const ClientConfig& config) {
  return transport &&
         client_detail::ClientLimits::validate_client_config(config) &&
         config.control_channel != config.senkusha_control_channel &&
         config.control_channel != config.stream_info_ack_channel &&
         config.senkusha_control_channel != config.stream_info_ack_channel;
}

}  // namespace libchiaking::net::protocol::takion
