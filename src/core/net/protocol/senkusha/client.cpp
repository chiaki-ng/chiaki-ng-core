#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/protocol/senkusha/senkusha.hpp"

#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/net/protocol/senkusha/bandwidth_test.hpp"
#include "src/core/net/protocol/senkusha/lifecycle.hpp"
#include "src/core/net/protocol/senkusha/mtu_search.hpp"
#include "src/core/net/protocol/senkusha/probe.hpp"
#include "src/core/net/protocol/senkusha/retained_transport.hpp"
#include "src/core/net/protocol/senkusha/rtt.hpp"
#include "src/core/net/protocol/senkusha/validator.hpp"
#include "src/core/net/protocol/takion/client.hpp"
#include "src/core/net/protocol/takion/control_codec.hpp"
#include "src/core/net/protocol/takion/limits.hpp"
#include "src/core/utils/bytes/endian.hpp"
#include "src/core/utils/numeric/checked.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::net::protocol::senkusha {
namespace lifecycle = lifecycle_detail;

class Client::Impl {
  class QueuedTakionSink final : public takion::EventSink {
   public:
    explicit QueuedTakionSink(const platform::ClockProvider& clock) noexcept
        : clock_(clock) {}

    void on_event(takion::Event event) override {
      events_.push_back({std::move(event), clock_.now_microseconds()});
    }

    bool empty() const noexcept { return events_.empty(); }

    std::pair<takion::Event, std::uint64_t> pop() {
      auto event = std::move(events_.front());
      events_.pop_front();
      return event;
    }

   private:
    const platform::ClockProvider& clock_;
    std::deque<std::pair<takion::Event, std::uint64_t>> events_;
  };

  struct ActiveProbe {
    std::uint16_t unit{};
    std::uint32_t tag{};
    takion::ProbeOperationId operation_id{};
    std::optional<std::uint64_t> sent_at_us{};
  };

  enum class PongKind : std::uint8_t {
    rtt,
    outbound_mtu,
  };

  struct AwaitedPong {
    PongKind kind{PongKind::rtt};
    std::uint16_t unit{};
    std::uint32_t tag{};
  };

  class MtuRun {
   public:
    [[nodiscard]] static auto create(const MtuPolicy& policy,
                                     const RttStatistics& rtt,
                                     std::uint32_t initial_candidate)
        -> Expected<MtuRun, Error> {
      const auto timeout = RttSampler::mtu_timeout(rtt.adjusted_mean, policy);
      if (!timeout) return unexpected(timeout.error());
      const auto timeout_us =
          checked_milliseconds_to_microseconds(timeout.value());
      if (!timeout_us) return unexpected(Error::arithmetic_overflow);
      auto search = ConservativeMtuSearch::create(
          policy.lower_bound, policy.upper_bound, policy.attempts_per_candidate,
          initial_candidate);
      if (!search) return unexpected(search.error());
      return MtuRun{std::move(search).value(), *timeout_us};
    }

    [[nodiscard]] auto begin_attempt() -> Expected<MtuSearchAttempt, Error> {
      auto attempt = search_.next_attempt();
      if (attempt) attempt_ = attempt.value();
      return attempt;
    }

    [[nodiscard]] auto finish(bool success)
        -> Expected<MtuSearchSnapshot, Error> {
      const auto id = attempt_->id;
      attempt_.reset();
      probe_.reset();
      const auto recorded =
          success ? search_.record_success(id) : search_.record_failure(id);
      if (!recorded) return unexpected(recorded.error());
      return search_.snapshot();
    }

    auto attempt() const noexcept -> const std::optional<MtuSearchAttempt>& {
      return attempt_;
    }
    auto probe() noexcept -> std::optional<ActiveProbe>& { return probe_; }
    auto probe() const noexcept -> const std::optional<ActiveProbe>& {
      return probe_;
    }
    auto timeout_us() const noexcept -> std::uint64_t { return timeout_us_; }

   private:
    MtuRun(ConservativeMtuSearch search, std::uint64_t timeout_us) noexcept
        : search_(std::move(search)), timeout_us_(timeout_us) {}

    ConservativeMtuSearch search_;
    std::uint64_t timeout_us_{};
    std::optional<MtuSearchAttempt> attempt_{};
    std::optional<ActiveProbe> probe_{};
  };

  struct Measurement {
    std::optional<ActiveProbe> rtt_probe{};
    std::optional<MtuRun> inbound_mtu{};
    std::optional<MtuRun> outbound_mtu{};
    std::optional<takion::ControlOperationId> echo_enable_ack{};
    std::optional<takion::ControlOperationId> echo_disable_ack{};
    std::optional<takion::ControlOperationId> outbound_mtu_disable_ack{};
  };

 public:
  Impl(std::unique_ptr<net::transport::datagram::DatagramTransport> transport,
       platform::ClockProvider& clock, platform::ThreadingProvider& threading,
       platform::SecureRandomProvider& random, EventSink& events,
       ClientConfig config)
      : config_(std::move(config)),
        clock_(clock),
        random_(random),
        events_(events),
        takion_events_(clock),
        lifecycle_(lifecycle_context_),
        takion_(retain_transport(std::move(transport)), clock, threading,
                random, takion_events_, make_takion_config(config_)) {}

  [[nodiscard]] OperationResult start() {
    if (lifecycle_.started()) return unexpected(Error::invalid_state);
    const auto validated =
        Validator::validate_configuration(config_.profile, config_.policy);
    if (!validated) return unexpected(validated.error());
    connect_timeout_us_ =
        checked_milliseconds_to_microseconds(config_.policy.connect_timeout);
    response_timeout_us_ =
        checked_milliseconds_to_microseconds(config_.policy.response_timeout);
    probe_timeout_us_ =
        checked_milliseconds_to_microseconds(config_.policy.probe.timeout);
    if (!connect_timeout_us_ || !response_timeout_us_ || !probe_timeout_us_)
      return unexpected(Error::arithmetic_overflow);
    bootstrap_profile_ =
        takion::resolve_takion_wire_profile(config_.profile.bootstrap_version);
    if (!bootstrap_profile_) return unexpected(Error::unsupported_version);

    ++last_attempt_id_;
    if (last_attempt_id_ == 0) return unexpected(Error::arithmetic_overflow);
    process(lifecycle::StartRequested{
        last_attempt_id_, clock_.now_microseconds(), *connect_timeout_us_,
        fixed_version(), config_.profile.big_response,
        config_.policy.bandwidth.has_value()});
    run_effects();
    if (lifecycle_context_.terminal_error)
      return unexpected(*lifecycle_context_.terminal_error);
    return {};
  }

  [[nodiscard]] OperationResult pump_once() {
    if (!lifecycle_.started()) return unexpected(Error::invalid_state);

    pump_readable_at_us_ = std::exchange(readable_at_us_, std::nullopt);
    const auto takion_state = takion_.snapshot().state;
    if (takion_state == takion::ClientState::handshaking ||
        takion_state == takion::ClientState::connected) {
      if (!takion_.pump_once()) takion_lost();
    }
    drain_takion_events();
    pump_readable_at_us_.reset();
    tick_bandwidth();

    if (lifecycle_context_.active_attempt_id &&
        lifecycle_context_.deadline_us &&
        clock_.now_microseconds() >= *lifecycle_context_.deadline_us) {
      const auto& outbound = measurement_.outbound_mtu;
      if (lifecycle_.state() == State::measuring_rtt &&
          sent(measurement_.rtt_probe)) {
        finish_rtt_attempt();
      } else if (lifecycle_.state() == State::measuring_inbound_mtu &&
                 measurement_.inbound_mtu &&
                 measurement_.inbound_mtu->attempt()) {
        finish_inbound_mtu_attempt(false);
      } else if (lifecycle_.state() == State::measuring_outbound_mtu &&
                 outbound && outbound->attempt() && sent(outbound->probe())) {
        finish_outbound_mtu_attempt(false);
      } else {
        process(
            lifecycle::DeadlineExpired{*lifecycle_context_.active_attempt_id});
      }
    }
    run_effects();
    if (lifecycle_context_.terminal_error)
      return unexpected(*lifecycle_context_.terminal_error);
    return {};
  }

  void mark_readable(std::uint64_t ready_at_us) noexcept {
    if (!readable_at_us_ || ready_at_us < *readable_at_us_)
      readable_at_us_ = ready_at_us;
  }

  [[nodiscard]] OperationResult cancel() {
    if (!lifecycle_context_.active_attempt_id) {
      if (retained_transport_) (void)retained_transport_->close_retained();
      return {};
    }
    process(lifecycle::CancelRequested{*lifecycle_context_.active_attempt_id});
    run_effects();
    if (retained_transport_) (void)retained_transport_->close_retained();
    return {};
  }

  Snapshot snapshot() const {
    return Snapshot{
        lifecycle_.state(),
        lifecycle_context_.active_attempt_id,
        lifecycle_context_.deadline_us,
        lifecycle_context_.rtt_statistics,
        lifecycle_context_.inbound_mtu,
        lifecycle_context_.outbound_mtu,
        result_,
        lifecycle_context_.terminal_error,
    };
  }

  [[nodiscard]] std::unique_ptr<net::transport::datagram::DatagramTransport>
  take_route() {
    if (route_metadata_ || lifecycle_.state() != State::completed) return {};
    return std::move(returned_route_);
  }

  void set_route_metadata(net::protocol::remote_play::RouteCandidate route) {
    route_metadata_ = std::move(route);
  }

  [[nodiscard]] std::optional<net::protocol::remote_play::EstablishedRoute>
  take_established_route() {
    if (lifecycle_.state() != State::completed || !result_ ||
        !route_metadata_ || !returned_route_)
      return std::nullopt;
    return net::protocol::remote_play::EstablishedRoute{
        {std::move(returned_route_), route_metadata_->local_endpoint,
         route_metadata_->remote_endpoint, route_metadata_->kind,
         route_metadata_->dont_fragment},
        *result_};
  }

  [[nodiscard]] std::optional<net::protocol::remote_play::RouteCandidate>
  take_failed_route() {
    if (lifecycle_.state() != State::failed ||
        lifecycle_context_.terminal_error == Error::cancelled ||
        !route_metadata_ || !retained_transport_)
      return std::nullopt;
    auto transport = retained_transport_->release();
    if (!transport) return std::nullopt;
    auto route = std::move(*route_metadata_);
    route_metadata_.reset();
    route.transport = std::move(transport);
    return route;
  }

 private:
  auto retain_transport(
      std::unique_ptr<net::transport::datagram::DatagramTransport> transport)
      -> std::unique_ptr<net::transport::datagram::DatagramTransport> {
    if (!transport ||
        config_.failure_handoff != TransportHandoffPolicy::return_route)
      return transport;
    auto retained = std::make_unique<RetainedTransport>(std::move(transport));
    retained_transport_ = retained.get();
    return retained;
  }
  std::optional<std::uint32_t> fixed_version() const {
    if (const auto* fixed = std::get_if<takion::FixedTakionProfile>(
            &config_.profile.profile_selection))
      return fixed->version;
    return std::nullopt;
  }

  std::uint64_t big_response_timeout_us() const noexcept {
    return config_.profile.big_response ==
                   BigResponsePolicy::fragmented_big_echo
               ? fragmented_big_response_timeout_us
               : *response_timeout_us_;
  }

  template <class Input>
  void process(const Input& event) {
    const auto before = lifecycle_.state();
    static_cast<void>(lifecycle_.process(event));
    const auto after = lifecycle_.state();
    if (before != after) events_.on_event(Event{event::StateChanged{after}});
  }

  void fail_current(Error error) {
    if (!lifecycle_context_.active_attempt_id) return;
    if (error == Error::takion_failure) {
      process(lifecycle::TakionFailed{*lifecycle_context_.active_attempt_id});
    } else {
      process(lifecycle::BangRejected{*lifecycle_context_.active_attempt_id,
                                      error});
    }
  }

  void fail_measurement(Error error) {
    measurement_ = {};
    lifecycle_context_.deadline_us.reset();
    if (lifecycle_context_.active_attempt_id) {
      process(lifecycle::MeasurementFailed{
          *lifecycle_context_.active_attempt_id, error});
    }
  }

  void drain_takion_events() {
    while (!takion_events_.empty()) {
      auto [value, received_at_us] = takion_events_.pop();
      event_received_at_us_ = received_at_us;
      std::visit(
          [this](auto&& event_value) { handle_takion_event(event_value); },
          std::move(value));
    }
  }

  void handle_takion_event(const takion::client_event::Connected&) {
    if (lifecycle_context_.active_attempt_id)
      process(lifecycle::TakionConnected{*lifecycle_context_.active_attempt_id,
                                         clock_.now_microseconds(),
                                         big_response_timeout_us()});
  }

  void handle_takion_event(
      const takion::client_event::ProfileCommitted& value) {
    if (lifecycle_context_.active_attempt_id)
      process(lifecycle::ProfileCommitted{
          *lifecycle_context_.active_attempt_id, value.version,
          clock_.now_microseconds(), big_response_timeout_us()});
  }

  void handle_takion_event(const takion::client_event::ControlReceived& value) {
    if (lifecycle_.state() == State::measuring_bandwidth) {
      const auto* senkusha = std::get_if<takion::Senkusha>(&value.message);
      const auto* bandwidth =
          senkusha ? std::get_if<takion::SenkushaBandwidthCommand>(
                         &senkusha->command)
                   : nullptr;
      if (bandwidth && bandwidth->total_sent && bandwidth_test_) {
        if (auto step =
                bandwidth_test_->reply(bandwidth->id, *bandwidth->total_sent,
                                       clock_.now_microseconds()))
          follow_bandwidth(*step);
      }
      return;
    }
    if (lifecycle_.state() == State::starting_outbound_mtu &&
        lifecycle_context_.active_attempt_id) {
      const auto* senkusha = std::get_if<takion::Senkusha>(&value.message);
      const auto* client_mtu =
          senkusha ? std::get_if<takion::SenkushaClientMtuCommand>(
                         &senkusha->command)
                   : nullptr;
      if (client_mtu && client_mtu->id == outbound_mtu_start_command_id) {
        process(
            lifecycle::OutboundMtuReady{*lifecycle_context_.active_attempt_id});
      }
      return;
    }
    if (lifecycle_.state() == State::awaiting_big_echo &&
        lifecycle_context_.active_attempt_id) {
      return;
    }
    const auto* bang = std::get_if<takion::Bang>(&value.message);
    if (!bang || !lifecycle_context_.active_attempt_id ||
        !lifecycle_context_.committed_version)
      return;
    const auto validated = Validator::validate_bang(
        config_.profile, *bang, *lifecycle_context_.committed_version);
    if (validated) {
      process(lifecycle::BangAccepted{*lifecycle_context_.active_attempt_id});
    } else {
      process(lifecycle::BangRejected{*lifecycle_context_.active_attempt_id,
                                      validated.error()});
    }
  }

  void handle_takion_event(
      const takion::client_event::ControlPayloadReceived& value) {
    if (lifecycle_.state() != State::awaiting_big_echo ||
        !lifecycle_context_.active_attempt_id || !echo_expected_size_)
      return;
    echo_received_size_ +=
        std::min(value.size, echo_expected_size_ - echo_received_size_);
    if (echo_received_size_ == echo_expected_size_)
      process(
          lifecycle::BigEchoReceived{*lifecycle_context_.active_attempt_id});
  }

  void handle_takion_event(const takion::client_event::ReliableAcked& value) {
    if (!value.operation_complete || !value.operation_id ||
        !lifecycle_context_.active_attempt_id)
      return;

    const auto attempt_id = *lifecycle_context_.active_attempt_id;
    const auto settle = [this, &value](
                            std::optional<takion::ControlOperationId>& expected,
                            State state) {
      if (expected != value.operation_id || lifecycle_.state() != state)
        return false;
      expected.reset();
      lifecycle_context_.deadline_us.reset();
      return true;
    };
    if (settle(measurement_.echo_enable_ack, State::enabling_echo))
      process(lifecycle::EchoEnabled{attempt_id});
    else if (settle(measurement_.echo_disable_ack, State::disabling_echo))
      process(lifecycle::EchoDisabled{attempt_id});
    else if (settle(measurement_.outbound_mtu_disable_ack,
                    State::finishing_outbound_mtu))
      process(lifecycle::OutboundMtuDisabled{attempt_id});
  }

  void handle_takion_event(const takion::client_event::ProbeSent& value) {
    auto& outbound = measurement_.outbound_mtu;
    if (outbound && outbound->probe() &&
        value.operation_id == outbound->probe()->operation_id &&
        !outbound->probe()->sent_at_us) {
      if (!value.delivered) {
        finish_outbound_mtu_attempt(false);
        return;
      }
      const auto deadline =
          numeric::checked_add(value.now_microseconds, outbound->timeout_us());
      if (!deadline) {
        fail_measurement(Error::arithmetic_overflow);
        return;
      }
      outbound->probe()->sent_at_us = value.now_microseconds;
      lifecycle_context_.deadline_us = *deadline;
      return;
    }
    auto& probe = measurement_.rtt_probe;
    if (!probe || value.operation_id != probe->operation_id ||
        probe->sent_at_us)
      return;
    if (!value.delivered) {
      if (value.blocked)
        finish_rtt_attempt();
      else
        fail_measurement(Error::takion_failure);
      return;
    }
    const auto deadline =
        numeric::checked_add(value.now_microseconds, *probe_timeout_us_);
    if (!deadline) {
      fail_measurement(Error::arithmetic_overflow);
      return;
    }
    probe->sent_at_us = value.now_microseconds;
    lifecycle_context_.deadline_us = *deadline;
  }

  [[nodiscard]] auto awaited_pong() const -> std::optional<AwaitedPong> {
    const auto state = lifecycle_.state();
    if (state == State::measuring_outbound_mtu) {
      const auto& outbound = measurement_.outbound_mtu;
      if (!outbound || !sent(outbound->probe())) return std::nullopt;
      return AwaitedPong{PongKind::outbound_mtu, outbound->probe()->unit,
                         outbound->probe()->tag};
    }
    if (state == State::measuring_rtt && sent(measurement_.rtt_probe))
      return AwaitedPong{PongKind::rtt, measurement_.rtt_probe->unit,
                         measurement_.rtt_probe->tag};
    return std::nullopt;
  }

  void settle_pong(const AwaitedPong& pong) {
    if (pong.kind == PongKind::outbound_mtu)
      finish_outbound_mtu_attempt(true);
    else
      record_rtt_pong();
  }

  static bool sent(const std::optional<ActiveProbe>& probe) noexcept {
    return probe && probe->sent_at_us;
  }

  void handle_takion_event(const takion::client_event::EncodedAudio& value) {
    const auto pong = awaited_pong();
    if (!pong) return;
    const auto classified = ProbeCodec::classify_local_rtt_pong(
        takion::SenkushaProbeMedia::audio, value.bytes,
        value.uses_nalu_info_structs, *bootstrap_profile_, config_.policy.probe,
        0, pong->unit, pong->tag);
    if (classified && classified.value().status == ProbeStatus::matched)
      settle_pong(*pong);
  }

  void record_rtt_pong() {
    const auto sent_at = *measurement_.rtt_probe->sent_at_us;
    const auto now = RttSampler::pong_received_at(sent_at, pump_readable_at_us_,
                                                  event_received_at_us_);
    if (now < sent_at) {
      fail_measurement(Error::arithmetic_overflow);
      return;
    }
    const auto elapsed = now - sent_at;
    if (elapsed >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      fail_measurement(Error::arithmetic_overflow);
      return;
    }
    rtt_.record(std::chrono::microseconds{static_cast<std::int64_t>(elapsed)});
    finish_rtt_attempt();
  }

  void handle_takion_event(const takion::client_event::EncodedVideo& value) {
    const auto& inbound = measurement_.inbound_mtu;
    if (lifecycle_.state() != State::measuring_inbound_mtu || !inbound ||
        !inbound->attempt())
      return;
    const auto classified = ProbeCodec::classify_local_inbound_mtu_response(
        takion::SenkushaProbeMedia::video, value.bytes,
        value.uses_nalu_info_structs, *bootstrap_profile_,
        static_cast<std::uint32_t>(inbound->attempt()->id));
    if (!classified || classified.value().status != ProbeStatus::matched)
      return;
    finish_inbound_mtu_attempt(true);
  }

  void handle_takion_event(const takion::client_event::AvUnitReceived& event) {
    const auto& value = event.values;
    if (lifecycle_.state() == State::measuring_bandwidth) {
      if (bandwidth_test_ && value.type.family() == takion::PacketFamily::video)
        bandwidth_test_->packet(clock_.now_microseconds(), value.data.size());
      return;
    }
    if (value.type.family() == takion::PacketFamily::video) {
      const auto& inbound = measurement_.inbound_mtu;
      if (lifecycle_.state() == State::measuring_inbound_mtu && inbound &&
          inbound->attempt() && value.frame_index == inbound->attempt()->id)
        finish_inbound_mtu_attempt(true);
      return;
    }
    if (value.type.family() != takion::PacketFamily::audio_and_haptics ||
        value.is_haptics || value.frame_index != 0)
      return;
    const auto& policy = config_.policy.probe;
    const auto offset = policy.tag_offset;
    if (value.data.size() < policy.minimum_payload_size ||
        offset > value.data.size() || value.data.size() - offset < 4)
      return;
    const auto tag =
        bytes::load_be<std::uint32_t>(std::span{value.data}.subspan(offset));
    if (const auto pong = awaited_pong();
        pong && tag == pong->tag &&
        (!policy.require_unit_index || value.unit_index == pong->unit))
      settle_pong(*pong);
  }

  void handle_takion_event(const takion::client_event::PacketRejected& value) {
    if (!value.operation_id || !takion::is_transient_send_error(value.error) ||
        value.error == takion::ClientError::send_blocked)
      return;
    const auto state = lifecycle_.state();
    if (expected_big_send_ && *value.operation_id == *expected_big_send_ &&
        (state == State::sending_big || state == State::awaiting_bang ||
         state == State::awaiting_big_echo)) {
      fail_current(Error::takion_failure);
      return;
    }
    const auto awaited =
        [&value, state](
            const std::optional<takion::ControlOperationId>& expected,
            State expected_state) {
          return expected == value.operation_id && state == expected_state;
        };
    if (awaited(measurement_.echo_enable_ack, State::enabling_echo) ||
        awaited(measurement_.echo_disable_ack, State::disabling_echo) ||
        awaited(measurement_.outbound_mtu_disable_ack,
                State::finishing_outbound_mtu) ||
        awaited(expected_inbound_mtu_send_, State::measuring_inbound_mtu) ||
        awaited(expected_outbound_mtu_send_, State::starting_outbound_mtu))
      fail_measurement(Error::takion_failure);
  }

  void takion_lost() {
    if (lifecycle_.state() != State::connecting_takion)
      fail_current(Error::takion_failure);
  }

  void handle_takion_event(const takion::client_event::TerminalFailure&) {
    takion_lost();
  }

  void handle_takion_event(const takion::client_event::Closed&) {
    takion_lost();
  }

  template <class Input>
  void handle_takion_event(const Input&) noexcept {}

  void run_effects() {
    while (!lifecycle_context_.effects.empty()) {
      auto effect = std::move(lifecycle_context_.effects.front());
      lifecycle_context_.effects.pop_front();
      std::visit(
          [this](auto&& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, lifecycle::StartTakion>) {
              if (!takion_.start()) fail_current(Error::takion_failure);
            } else if constexpr (std::is_same_v<T, lifecycle::SendBig>) {
              const auto& message = config_.profile.big;
              if (config_.profile.big_response ==
                  BigResponsePolicy::fragmented_big_echo) {
                takion::ControlCodec codec{
                    config_.takion_limits.maximum_message_size};
                auto encoded =
                    codec.encode(takion::ControlMessage{message.clone()});
                if (!encoded) {
                  fail_current(Error::malformed_response);
                  return;
                }
                echo_expected_size_ = encoded.value().size();
                echo_received_size_ = 0;
              }
              const auto operation_id =
                  next_operation_id<takion::ControlOperationId>();
              if (!operation_id) {
                fail_current(Error::arithmetic_overflow);
                return;
              }
              expected_big_send_ = *operation_id;
              const auto posted = takion_.post(takion::SendControl{
                  message.clone(), *operation_id,
                  lifecycle_context_.deadline_us
                      ? lifecycle_context_.deadline_us
                      : send_deadline_after(*response_timeout_us_)});
              if (!posted) fail_current(Error::takion_failure);
            } else if constexpr (std::is_same_v<T, lifecycle::BeginRtt>) {
              send_echo_command(true);
            } else if constexpr (std::is_same_v<T, lifecycle::BeginRttProbes>) {
              begin_rtt_probes();
            } else if constexpr (std::is_same_v<T, lifecycle::DisableEcho>) {
              send_echo_command(false);
            } else if constexpr (std::is_same_v<T,
                                                lifecycle::BeginInboundMtu>) {
              begin_inbound_mtu();
            } else if constexpr (std::is_same_v<T,
                                                lifecycle::BeginOutboundMtu>) {
              send_outbound_mtu_command(value.inbound_mtu);
            } else if constexpr (std::is_same_v<
                                     T, lifecycle::BeginOutboundMtuProbes>) {
              begin_outbound_mtu_search();
            } else if constexpr (std::is_same_v<
                                     T, lifecycle::DisableOutboundMtu>) {
              send_outbound_mtu_disable_command(value.inbound_mtu,
                                                value.upper_bound);
            } else if constexpr (std::is_same_v<T, lifecycle::BeginBandwidth>) {
              begin_bandwidth(value.attempt_id, value.inbound_mtu);
            } else if constexpr (std::is_same_v<T,
                                                lifecycle::BeginDisconnect>) {
              complete_senkusha(value.attempt_id);
            } else if constexpr (std::is_same_v<
                                     T, lifecycle::BestEffortDisconnect>) {
              best_effort_disconnect();
            } else if constexpr (std::is_same_v<T, lifecycle::StopTakion>) {
              static_cast<void>(takion_.request_stop());
            } else if constexpr (std::is_same_v<T, lifecycle::PublishFailure>) {
              events_.on_event(Event{event::Failed{value.error}});
            } else if constexpr (std::is_same_v<T, lifecycle::PublishSuccess>) {
              if (result_) events_.on_event(Event{event::Completed{*result_}});
            }
          },
          std::move(effect));
    }
  }

  template <class OperationId>
  [[nodiscard]] auto next_operation_id() -> std::optional<OperationId> {
    if (next_operation_id_ == 0) return std::nullopt;
    const OperationId result{next_operation_id_};
    next_operation_id_ =
        next_operation_id_ == std::numeric_limits<std::uint64_t>::max()
            ? 0
            : next_operation_id_ + 1;
    return result;
  }

  [[nodiscard]] std::optional<std::uint64_t> send_deadline_after(
      std::uint64_t timeout_us) const noexcept {
    return numeric::checked_add(clock_.now_microseconds(), timeout_us);
  }

  [[nodiscard]] bool arm_response_deadline() {
    const auto deadline = send_deadline_after(*response_timeout_us_);
    if (!deadline) {
      fail_measurement(Error::arithmetic_overflow);
      return false;
    }
    lifecycle_context_.deadline_us = *deadline;
    return true;
  }

  void send_echo_command(bool enabled) {
    const auto operation_id = next_operation_id<takion::ControlOperationId>();
    if (!operation_id || !arm_response_deadline()) {
      if (!operation_id) fail_measurement(Error::arithmetic_overflow);
      return;
    }
    (enabled ? measurement_.echo_enable_ack : measurement_.echo_disable_ack) =
        *operation_id;

    const auto posted = takion_.post(takion::SendControl{
        takion::Senkusha{takion::SenkushaEchoCommand{enabled}},
        *operation_id,
        lifecycle_context_.deadline_us,
    });
    if (!posted) fail_measurement(Error::takion_failure);
  }

  void send_outbound_mtu_command(std::uint32_t inbound_mtu) {
    if (!lifecycle_context_.active_attempt_id) return;
    const auto operation_id = next_operation_id<takion::ControlOperationId>();
    if (!operation_id) {
      fail_measurement(Error::arithmetic_overflow);
      return;
    }
    if (!arm_response_deadline()) return;
    expected_outbound_mtu_send_ = *operation_id;
    const auto posted = takion_.post(takion::SendControl{
        takion::Senkusha{takion::SenkushaClientMtuCommand{
            outbound_mtu_start_command_id, inbound_mtu, true, inbound_mtu}},
        *operation_id,
        lifecycle_context_.deadline_us,
    });
    if (!posted) fail_measurement(Error::takion_failure);
  }

  void send_outbound_mtu_disable_command(std::uint32_t inbound_mtu,
                                         std::uint32_t upper_bound) {
    const auto operation_id = next_operation_id<takion::ControlOperationId>();
    if (!operation_id || !arm_response_deadline()) {
      if (!operation_id) fail_measurement(Error::arithmetic_overflow);
      return;
    }
    measurement_.outbound_mtu_disable_ack = *operation_id;
    const auto posted = takion_.post(takion::SendControl{
        takion::Senkusha{takion::SenkushaClientMtuCommand{
            outbound_mtu_stop_command_id, upper_bound, false, inbound_mtu}},
        *operation_id,
        lifecycle_context_.deadline_us,
    });
    if (!posted) fail_measurement(Error::takion_failure);
  }

  void begin_rtt_probes() {
    lifecycle_context_.deadline_us.reset();
    rtt_ = {};
    measurement_.rtt_probe.reset();
    send_next_rtt_probe();
  }

  void send_next_rtt_probe() {
    if (!lifecycle_context_.active_attempt_id) return;
    if (rtt_.attempted() >= config_.policy.probe.count) {
      finish_rtt_measurement();
      return;
    }
    if (rtt_.attempted() > std::numeric_limits<std::uint16_t>::max()) {
      fail_measurement(Error::invalid_configuration);
      return;
    }

    std::array<std::byte, 4> tag_bytes{};
    if (!random_.fill_bytes(tag_bytes)) {
      fail_measurement(Error::takion_failure);
      return;
    }
    const auto tag = bytes::load_be<std::uint32_t>(tag_bytes);
    auto encoded = ProbeCodec::build_local_rtt_probe(
        0, static_cast<std::uint16_t>(rtt_.attempted()), tag,
        *bootstrap_profile_, config_.profile.envelope);
    if (!encoded) {
      fail_measurement(encoded.error());
      return;
    }
    const auto operation_id = next_operation_id<takion::ProbeOperationId>();
    if (!operation_id) {
      fail_measurement(Error::arithmetic_overflow);
      return;
    }

    measurement_.rtt_probe = ActiveProbe{
        static_cast<std::uint16_t>(rtt_.attempted()),
        tag,
        *operation_id,
        std::nullopt,
    };
    lifecycle_context_.deadline_us.reset();
    const auto posted = takion_.post(takion::SendProbe{
        takion::SenkushaProbeMedia::audio,
        std::move(encoded.value().encoded_av_payload),
        encoded.value().datagram_size,
        *operation_id,
        send_deadline_after(*probe_timeout_us_),
    });
    if (!posted) fail_measurement(Error::takion_failure);
  }

  void finish_rtt_attempt() {
    measurement_.rtt_probe.reset();
    lifecycle_context_.deadline_us.reset();
    rtt_.complete_attempt();
    if (rtt_.attempted() < config_.policy.probe.count)
      send_next_rtt_probe();
    else
      finish_rtt_measurement();
  }

  void finish_rtt_measurement() {
    if (!lifecycle_context_.active_attempt_id) return;
    const auto statistics = rtt_.summary(config_.profile.rtt_adjustment);
    if (!statistics && statistics.error() == Error::no_rtt_samples) {
      process(lifecycle::RttUnmeasured{*lifecycle_context_.active_attempt_id,
                                       statistics.error()});
      return;
    }
    if (!statistics) {
      fail_measurement(statistics.error());
      return;
    }
    process(lifecycle::RttMeasured{*lifecycle_context_.active_attempt_id,
                                   statistics.value()});
  }

  void begin_inbound_mtu() {
    if (!lifecycle_context_.active_attempt_id ||
        !lifecycle_context_.rtt_statistics) {
      fail_measurement(Error::invalid_state);
      return;
    }
    auto run =
        MtuRun::create(config_.policy.mtu, *lifecycle_context_.rtt_statistics,
                       config_.policy.mtu.upper_bound);
    if (!run) {
      fail_measurement(run.error());
      return;
    }
    measurement_.inbound_mtu.emplace(std::move(run).value());
    send_next_inbound_mtu_attempt();
  }

  void send_next_inbound_mtu_attempt() {
    auto& inbound = measurement_.inbound_mtu;
    if (!lifecycle_context_.active_attempt_id || !inbound) return;
    auto attempt = inbound->begin_attempt();
    if (!attempt) {
      fail_measurement(attempt.error());
      return;
    }
    if (attempt.value().id > std::numeric_limits<std::uint16_t>::max()) {
      fail_measurement(Error::arithmetic_overflow);
      return;
    }
    const auto operation_id = next_operation_id<takion::ControlOperationId>();
    if (!operation_id) {
      fail_measurement(Error::arithmetic_overflow);
      return;
    }

    const auto now = clock_.now_microseconds();
    const auto deadline =
        numeric::checked_mul(inbound_mtu_deadline_factor, inbound->timeout_us())
            .and_then([now](std::uint64_t window) {
              return numeric::checked_add(now, window);
            });
    if (!deadline) {
      fail_measurement(Error::arithmetic_overflow);
      return;
    }
    lifecycle_context_.deadline_us = *deadline;
    expected_inbound_mtu_send_ = *operation_id;
    const auto posted = takion_.post(takion::SendControl{
        takion::Senkusha{takion::SenkushaMtuCommand{
            static_cast<std::uint32_t>(attempt.value().id),
            attempt.value().candidate,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
        }},
        *operation_id,
        now + inbound->timeout_us(),
    });
    if (!posted) fail_measurement(Error::takion_failure);
  }

  void handle_takion_event(const takion::client_event::ControlSent& value) {
    const auto& inbound = measurement_.inbound_mtu;
    if (lifecycle_.state() != State::measuring_inbound_mtu ||
        expected_inbound_mtu_send_ != value.operation_id || !inbound ||
        !inbound->attempt())
      return;
    const auto deadline =
        numeric::checked_add(value.now_microseconds, inbound->timeout_us());
    if (!deadline) {
      fail_measurement(Error::arithmetic_overflow);
      return;
    }
    lifecycle_context_.deadline_us = *deadline;
  }

  void finish_inbound_mtu_attempt(bool success) {
    auto& inbound = measurement_.inbound_mtu;
    if (!inbound || !inbound->attempt()) return;
    lifecycle_context_.deadline_us.reset();
    const auto snapshot = inbound->finish(success);
    if (!snapshot) {
      fail_measurement(snapshot.error());
      return;
    }
    if (!snapshot->result) {
      send_next_inbound_mtu_attempt();
      return;
    }
    inbound.reset();
    if (lifecycle_context_.active_attempt_id)
      process(lifecycle::InboundMtuMeasured{
          *lifecycle_context_.active_attempt_id, *snapshot->result});
  }

  void begin_outbound_mtu_search() {
    if (!lifecycle_context_.active_attempt_id ||
        !lifecycle_context_.rtt_statistics || !lifecycle_context_.inbound_mtu) {
      fail_measurement(Error::invalid_state);
      return;
    }
    auto run =
        MtuRun::create(config_.policy.mtu, *lifecycle_context_.rtt_statistics,
                       *lifecycle_context_.inbound_mtu);
    if (!run) {
      fail_measurement(run.error());
      return;
    }
    measurement_.outbound_mtu.emplace(std::move(run).value());
    send_next_outbound_mtu_attempt();
  }

  void send_next_outbound_mtu_attempt() {
    auto& outbound = measurement_.outbound_mtu;
    if (!lifecycle_context_.active_attempt_id || !outbound) return;
    auto attempt = outbound->begin_attempt();
    if (!attempt) {
      fail_measurement(attempt.error());
      return;
    }
    if (attempt.value().attempt_number == 0 ||
        attempt.value().attempt_number - 1 > takion::max_unit_index) {
      fail_measurement(Error::arithmetic_overflow);
      return;
    }

    std::array<std::byte, sizeof(std::uint32_t)> tag_bytes{};
    if (!random_.fill_bytes(tag_bytes)) {
      fail_measurement(Error::takion_failure);
      return;
    }
    const auto tag = bytes::load_be<std::uint32_t>(tag_bytes);
    const auto unit =
        static_cast<std::uint16_t>(attempt.value().attempt_number - 1);
    auto encoded = ProbeCodec::build_local_outbound_mtu_probe(
        attempt.value().candidate, unit, tag, *bootstrap_profile_,
        config_.profile.envelope, config_.policy.mtu);
    if (!encoded) {
      fail_measurement(encoded.error());
      return;
    }
    const auto operation_id = next_operation_id<takion::ProbeOperationId>();
    if (!operation_id) {
      fail_measurement(Error::arithmetic_overflow);
      return;
    }

    outbound->probe() = ActiveProbe{unit, tag, *operation_id, std::nullopt};
    lifecycle_context_.deadline_us.reset();
    const auto posted = takion_.post(takion::SendProbe{
        takion::SenkushaProbeMedia::audio,
        std::move(encoded.value().encoded_av_payload),
        encoded.value().datagram_size,
        *operation_id,
        send_deadline_after(outbound->timeout_us()),
    });
    if (!posted) fail_measurement(Error::takion_failure);
  }

  void finish_outbound_mtu_attempt(bool success) {
    auto& outbound = measurement_.outbound_mtu;
    if (!outbound || !outbound->attempt()) return;
    lifecycle_context_.deadline_us.reset();
    const auto snapshot = outbound->finish(success);
    if (!snapshot) {
      fail_measurement(snapshot.error());
      return;
    }
    if (!snapshot->result) {
      send_next_outbound_mtu_attempt();
      return;
    }
    outbound.reset();
    if (lifecycle_context_.active_attempt_id)
      process(
          lifecycle::OutboundMtuMeasured{*lifecycle_context_.active_attempt_id,
                                         *snapshot->result, snapshot->upper});
  }

  void best_effort_disconnect() {
    if (takion_.snapshot().state != takion::ClientState::connected) return;
    const auto posted = takion_.post(takion::SendControl{
        takion::Disconnect{"Client Disconnecting", std::nullopt},
        std::nullopt});
    if (posted) static_cast<void>(takion_.pump_once());
  }

  void begin_bandwidth(std::uint64_t attempt_id, std::uint32_t inbound_mtu) {
    bandwidth_attempt_id_ = attempt_id;
    bandwidth_mtu_ = inbound_mtu;
    auto created = BandwidthTest::create(*config_.policy.bandwidth);
    if (!created) {
      process(lifecycle::BandwidthMeasured{attempt_id, std::nullopt});
      return;
    }
    bandwidth_test_.emplace(std::move(created).value());
    follow_bandwidth(bandwidth_test_->start(clock_.now_microseconds()));
  }

  void tick_bandwidth() {
    if (!bandwidth_test_ || lifecycle_.state() != State::measuring_bandwidth)
      return;
    if (auto step = bandwidth_test_->tick(clock_.now_microseconds()))
      follow_bandwidth(*step);
  }

  void follow_bandwidth(const BandwidthStep& step) {
    if (const auto* send = std::get_if<bandwidth_step::Send>(&step)) {
      if (!send_bandwidth_round(send->round)) finish_bandwidth(std::nullopt);
      return;
    }
    if (const auto* finished = std::get_if<bandwidth_step::Finished>(&step))
      finish_bandwidth(finished->measurement);
  }

  [[nodiscard]] bool send_bandwidth_round(const BandwidthRound& round) {
    const auto operation_id = next_operation_id<takion::ControlOperationId>();
    const auto deadline = bandwidth_test_->deadline_us();
    if (!operation_id || !deadline) return false;
    takion::SenkushaBandwidthCommand command{};
    command.id = round.id;
    command.time = round.duration_ms;
    command.bandwidth = round.kbps;
    command.fps = round.fps;
    command.mtu = bandwidth_mtu_;
    return static_cast<bool>(takion_.post(takion::SendControl{
        takion::Senkusha{command}, *operation_id, *deadline}));
  }

  void finish_bandwidth(std::optional<BandwidthMeasurement> measurement) {
    bandwidth_test_.reset();
    process(lifecycle::BandwidthMeasured{bandwidth_attempt_id_, measurement});
  }

  void complete_senkusha(std::uint64_t attempt_id) {
    if (!lifecycle_context_.is_current(attempt_id) ||
        !lifecycle_context_.committed_version ||
        !lifecycle_context_.rtt_statistics || !lifecycle_context_.inbound_mtu ||
        !lifecycle_context_.outbound_mtu) {
      fail_measurement(Error::invalid_state);
      return;
    }
    const auto profile = takion::resolve_takion_wire_profile(
        *lifecycle_context_.committed_version);
    if (!profile) {
      fail_measurement(Error::unsupported_version);
      return;
    }
    Result result{
        *lifecycle_context_.committed_version,
        *lifecycle_context_.rtt_statistics,
        *lifecycle_context_.inbound_mtu,
        *lifecycle_context_.outbound_mtu,
        profile.value(),
        lifecycle_context_.bandwidth,
    };

    best_effort_disconnect();
    if (config_.profile.handoff == TransportHandoffPolicy::return_route) {
      auto released = takion_.release_transport();
      if (!released) {
        fail_measurement(Error::takion_failure);
        return;
      }
      if (retained_transport_) {
        returned_route_ = retained_transport_->release();
        retained_transport_ = nullptr;
      } else {
        returned_route_ = std::move(released).value();
      }
    } else {
      const auto stopped = takion_.request_stop();
      if (retained_transport_) (void)retained_transport_->close_retained();
      if (!stopped) {
        fail_measurement(Error::takion_failure);
        return;
      }
    }
    result_ = std::move(result);
    process(lifecycle::DisconnectCompleted{attempt_id});
  }

  ClientConfig config_;
  platform::ClockProvider& clock_;
  platform::SecureRandomProvider& random_;
  EventSink& events_;
  QueuedTakionSink takion_events_;
  lifecycle::LifecycleContext lifecycle_context_;
  lifecycle::Lifecycle lifecycle_;
  RetainedTransport* retained_transport_{};
  takion::Client takion_;
  std::optional<std::uint64_t> connect_timeout_us_{};
  std::optional<std::uint64_t> response_timeout_us_{};
  std::optional<std::uint64_t> probe_timeout_us_{};
  std::optional<takion::WireProfile> bootstrap_profile_{};
  Measurement measurement_{};
  std::optional<takion::ControlOperationId> expected_big_send_{};
  std::optional<takion::ControlOperationId> expected_inbound_mtu_send_{};
  std::optional<takion::ControlOperationId> expected_outbound_mtu_send_{};
  RttSampler rtt_{};
  std::size_t echo_expected_size_{};
  std::size_t echo_received_size_{};
  std::uint64_t next_operation_id_{1};
  std::uint64_t event_received_at_us_{};
  std::optional<std::uint64_t> readable_at_us_{};
  std::optional<std::uint64_t> pump_readable_at_us_{};
  std::uint64_t last_attempt_id_{};
  std::optional<Result> result_{};
  std::optional<BandwidthTest> bandwidth_test_{};
  std::uint64_t bandwidth_attempt_id_{};
  std::uint32_t bandwidth_mtu_{};
  std::unique_ptr<net::transport::datagram::DatagramTransport>
      returned_route_{};
  std::optional<net::protocol::remote_play::RouteCandidate> route_metadata_{};

 private:
  [[nodiscard]] static auto checked_milliseconds_to_microseconds(
      std::chrono::milliseconds value) noexcept
      -> std::optional<std::uint64_t> {
    if (value.count() <= 0) return std::nullopt;
    const auto milliseconds = static_cast<std::uint64_t>(value.count());
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max() /
                             microseconds_per_millisecond;
    if (milliseconds > maximum) return std::nullopt;
    return milliseconds * microseconds_per_millisecond;
  }

  static takion::ClientConfig make_takion_config(const ClientConfig& config) {
    takion::ClientConfig result;
    result.bootstrap_version = config.profile.bootstrap_version;
    result.profile_selection = config.profile.profile_selection;
    result.negotiation_wire_policy =
        takion::NegotiationWirePolicy::retain_bootstrap_profile;
    result.envelope = config.profile.envelope;
    result.receive_envelope = config.profile.receive_envelope;
    result.limits = config.takion_limits;
    result.limits.maximum_fragment_size =
        std::max(result.limits.maximum_fragment_size,
                 static_cast<std::size_t>(config.policy.mtu.upper_bound));
    result.limits.maximum_control_datagram_size = minimum_datagram_size;
    result.handshake_retry = config.handshake_retry;
    result.crypto_policy = takion::CryptoPolicy::disabled;
    result.control_channel = takion::control_channel_id;
    result.senkusha_control_channel = takion::senkusha_control_channel_id;
    result.report_control_payload_lengths =
        config.profile.big_response == BigResponsePolicy::fragmented_big_echo;
    return result;
  }
};

Client::Client(
    std::unique_ptr<net::transport::datagram::DatagramTransport> transport,
    platform::ClockProvider& clock, platform::ThreadingProvider& threading,
    platform::SecureRandomProvider& random, EventSink& events,
    ClientConfig config)
    : impl_(std::make_unique<Impl>(std::move(transport), clock, threading,
                                   random, events, std::move(config))) {}

Client::Client(net::protocol::remote_play::RouteCandidate route,
               platform::ClockProvider& clock,
               platform::ThreadingProvider& threading,
               platform::SecureRandomProvider& random, EventSink& events,
               ClientConfig config)
    : impl_(std::make_unique<Impl>(std::move(route.transport), clock, threading,
                                   random, events, std::move(config))) {
  impl_->set_route_metadata(std::move(route));
}

Client::~Client() = default;

auto Client::start() -> OperationResult { return impl_->start(); }

auto Client::pump_once() -> OperationResult { return impl_->pump_once(); }

void Client::mark_readable(std::uint64_t ready_at_us) noexcept {
  impl_->mark_readable(ready_at_us);
}

auto Client::cancel() -> OperationResult { return impl_->cancel(); }

auto Client::snapshot() const -> Snapshot { return impl_->snapshot(); }

auto Client::take_route()
    -> std::unique_ptr<net::transport::datagram::DatagramTransport> {
  return impl_->take_route();
}

auto Client::take_established_route()
    -> std::optional<net::protocol::remote_play::EstablishedRoute> {
  return impl_->take_established_route();
}

auto Client::take_failed_route()
    -> std::optional<net::protocol::remote_play::RouteCandidate> {
  return impl_->take_failed_route();
}

}  // namespace libchiaking::net::protocol::senkusha
