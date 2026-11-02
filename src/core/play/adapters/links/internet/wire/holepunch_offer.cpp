#include "src/core/play/adapters/links/internet/wire/holepunch_offer.hpp"

#include <boost/sml.hpp>

namespace libchiaking::play::internet::holepunch {

auto Offer::make_initial_effects(std::uint64_t attempt,
                                 std::uint64_t peer_req_id,
                                 std::uint64_t own_req_id)
    -> Expected<std::array<OfferEffect, 2>, OfferEffectError> {
  if (attempt == 0) return unexpected(OfferEffectError::invalid_attempt);
  if (peer_req_id > UINT16_MAX)
    return unexpected(OfferEffectError::invalid_peer_req_id);
  if (own_req_id == 0) return unexpected(OfferEffectError::invalid_own_req_id);
  return std::array<OfferEffect, 2>{
      OfferEffect{AckPeerOffer{attempt, peer_req_id, 0}},
      OfferEffect{SendOwnOffer{attempt, own_req_id}},
  };
}

namespace offer_wait_detail {
struct Idle {};
struct WaitingOffer {};
struct WaitingResult {};
struct Ready {};
struct Failed {};
struct Begin {};
struct Offer {};
struct Result {};
struct Stop {};
struct Definition {
  auto operator()() const {
    using namespace boost::sml;
    return make_transition_table(
        *state<Idle> + event<Begin> = state<WaitingOffer>,
        state<WaitingOffer> + event<Offer> = state<WaitingResult>,
        state<WaitingResult> + event<Offer> = state<WaitingResult>,
        state<WaitingResult> + event<Result> = state<Ready>,
        state<WaitingOffer> + event<Stop> = state<Failed>,
        state<WaitingResult> + event<Stop> = state<Failed>);
  }
};
}  // namespace offer_wait_detail
class OfferWait::Machine {
  boost::sml::sm<offer_wait_detail::Definition> machine_;

 public:
  using Begin = offer_wait_detail::Begin;
  using Offer = offer_wait_detail::Offer;
  using Result = offer_wait_detail::Result;
  using Stop = offer_wait_detail::Stop;
  template <class Event>
  void event(Event e) {
    machine_.process_event(e);
  }
  OfferWaitState state() const {
    using boost::sml::state;
    using namespace offer_wait_detail;
    if (machine_.is(state<Idle>)) return OfferWaitState::idle;
    if (machine_.is(state<WaitingOffer>)) return OfferWaitState::waiting_offer;
    if (machine_.is(state<WaitingResult>))
      return OfferWaitState::waiting_result;
    if (machine_.is(state<Ready>)) return OfferWaitState::ready_to_probe;
    return OfferWaitState::failed;
  }
};
OfferWait::OfferWait() : machine_(std::make_unique<Machine>()) {}
OfferWait::~OfferWait() = default;
OfferWait::OfferWait(OfferWait&&) noexcept = default;
OfferWait& OfferWait::operator=(OfferWait&&) noexcept = default;
OfferWaitState OfferWait::state() const noexcept { return machine_->state(); }

bool OfferWait::begin(std::uint64_t attempt, std::uint64_t own_req_id) {
  if (state() != OfferWaitState::idle || attempt == 0 || own_req_id == 0)
    return false;
  attempt_ = attempt;
  own_req_id_ = own_req_id;
  effects_.clear();
  machine_->event(Machine::Begin{});
  return true;
}

void OfferWait::on_console_offer(std::uint64_t attempt,
                                 std::uint64_t peer_req_id) {
  if (attempt != attempt_ || peer_req_id > UINT16_MAX) return;
  if (state() == OfferWaitState::waiting_result) {
    machine_->event(Machine::Offer{});
    effects_.push_back(AckPeerOffer{attempt, peer_req_id, 0});
    return;
  }
  if (state() != OfferWaitState::waiting_offer) return;
  auto result = Offer::make_initial_effects(attempt, peer_req_id, own_req_id_);
  if (!result.has_value()) return;
  effects_.assign(result.value().begin(), result.value().end());
  machine_->event(Machine::Offer{});
}

void OfferWait::on_result(std::uint64_t attempt, std::uint64_t req_id,
                          std::uint32_t) {
  if (state() != OfferWaitState::waiting_result || attempt != attempt_ ||
      req_id != own_req_id_)
    return;
  effects_.clear();
  machine_->event(Machine::Result{});
}

bool OfferWait::timeout(std::uint64_t attempt) {
  if (attempt != attempt_ || (state() != OfferWaitState::waiting_offer &&
                              state() != OfferWaitState::waiting_result))
    return false;
  effects_.clear();
  machine_->event(Machine::Stop{});
  return true;
}

bool OfferWait::cancel(std::uint64_t attempt) { return timeout(attempt); }

void OfferWait::on_message(std::uint64_t attempt,
                           const net::nat::holepunch::SessionMessage& message) {
  switch (message.action) {
    case net::nat::holepunch::Action::offer:
      if (message.conn_request.has_value())
        on_console_offer(attempt, message.req_id);
      break;
    case net::nat::holepunch::Action::result:
      on_result(attempt, message.req_id, message.error);
      break;
    case net::nat::holepunch::Action::terminate:
      static_cast<void>(cancel(attempt));
      break;
    case net::nat::holepunch::Action::accept:
    case net::nat::holepunch::Action::unknown:
      break;
  }
}

std::vector<OfferEffect> OfferWait::take_effects() {
  auto out = std::move(effects_);
  effects_.clear();
  return out;
}

bool OfferInbox::begin(std::uint64_t attempt, std::uint64_t own_req_id) {
  if (!wait_.begin(attempt, own_req_id)) return false;
  attempt_ = attempt;
  peer_request_.reset();
  return true;
}

void OfferInbox::on_message(std::uint64_t attempt,
                            net::nat::holepunch::SessionMessage&& message) {
  const bool accepted_offer =
      message.action == net::nat::holepunch::Action::offer &&
      wait_.state() == OfferWaitState::waiting_offer && attempt == attempt_ &&
      message.conn_request.has_value();
  wait_.on_message(attempt, message);
  if (accepted_offer && wait_.state() == OfferWaitState::waiting_result)
    peer_request_ = std::move(message.conn_request);
  if (wait_.state() == OfferWaitState::failed) peer_request_.reset();
}

bool OfferInbox::timeout(std::uint64_t attempt) {
  const bool changed = wait_.timeout(attempt);
  if (changed) peer_request_.reset();
  return changed;
}

bool OfferInbox::cancel(std::uint64_t attempt) {
  const bool changed = wait_.cancel(attempt);
  if (changed) peer_request_.reset();
  return changed;
}

auto OfferInbox::take_effects() -> std::vector<OfferEffect> {
  return wait_.take_effects();
}

auto OfferInbox::take_peer_request()
    -> std::optional<net::nat::holepunch::ConnRequest> {
  if (wait_.state() != OfferWaitState::ready_to_probe) return std::nullopt;
  auto out = std::move(peer_request_);
  peer_request_.reset();
  return out;
}

OfferDrive::OfferDrive(Receive receive, Send send,
                       const diagnostics::Trace* trace)
    : receive_(std::move(receive)), send_(std::move(send)), trace_(trace) {}

auto OfferDrive::run(std::uint64_t attempt, std::uint64_t own_req_id)
    -> Expected<net::nat::holepunch::ConnRequest, OfferPsnError> {
  if (attempt == 0)
    return unexpected(OfferPsnError{OfferPsnErrorKind::invalid_attempt});
  if (own_req_id == 0 || own_req_id > UINT16_MAX)
    return unexpected(OfferPsnError{OfferPsnErrorKind::invalid_own_req_id});
  if (!inbox_.begin(attempt, own_req_id))
    return unexpected(OfferPsnError{OfferPsnErrorKind::invalid_attempt});

  trace_state();
  for (;;) {
    auto received = receive_();
    if (!received) return unexpected(received.error());
    const auto prior = inbox_.state();
    inbox_.on_message(attempt, std::move(received.value()));
    if (prior != inbox_.state()) trace_state();
    if (inbox_.state() == OfferWaitState::failed)
      return unexpected(OfferPsnError{OfferPsnErrorKind::terminated});
    for (const auto& effect : inbox_.take_effects()) {
      auto sent = send_(effect);
      if (!sent) return unexpected(sent.error());
    }
    if (inbox_.state() == OfferWaitState::ready_to_probe) {
      auto peer = inbox_.take_peer_request();
      if (peer) return std::move(peer.value());
      return unexpected(OfferPsnError{OfferPsnErrorKind::terminated});
    }
  }
}

void OfferDrive::trace_state() const noexcept {
  if (trace_)
    trace_->record_state(diagnostics::Operation::offer_state, inbox_.state());
}

auto Offer::send_effect(
    const OfferEffect& effect, std::uint64_t current_attempt,
    psn::remote_play::SessionClient& client,
    const psn::auth::BearerToken& bearer_token, std::string_view session_id,
    std::int64_t account_id,
    std::span<const std::byte, psn_device_uid_size> console_uid,
    psn::rest::ConsoleFamily family,
    const net::nat::holepunch::ConnRequest& local_offer,
    const platform::StopToken& stop) -> Expected<void, OfferSendError> {
  static_assert(std::variant_size_v<OfferEffect> == 2);
  return std::visit(
      [&](const auto& item) -> Expected<void, OfferSendError> {
        if (item.attempt == 0 || item.attempt != current_attempt)
          return unexpected(OfferSendError::stale_attempt());
        const std::uint64_t req_id = [&]() -> std::uint64_t {
          if constexpr (std::is_same_v<std::decay_t<decltype(item)>,
                                       AckPeerOffer>)
            return item.peer_req_id;
          else
            return item.own_req_id;
        }();
        constexpr bool own_offer =
            std::is_same_v<std::decay_t<decltype(item)>, SendOwnOffer>;
        if ((own_offer && req_id == 0) ||
            req_id > std::numeric_limits<std::uint16_t>::max())
          return unexpected(OfferSendError::invalid_req_id());
        if constexpr (std::is_same_v<std::decay_t<decltype(item)>,
                                     AckPeerOffer>) {
          if (item.error > std::numeric_limits<std::uint16_t>::max())
            return unexpected(OfferSendError::invalid_error());
        }
        auto sent = [&]() -> Expected<void, psn::remote_play::SessionError> {
          if constexpr (std::is_same_v<std::decay_t<decltype(item)>,
                                       AckPeerOffer>) {
            return client.send_short_message(
                bearer_token, session_id, net::nat::holepunch::Action::result,
                static_cast<std::uint16_t>(req_id),
                static_cast<std::uint16_t>(item.error), account_id, console_uid,
                family, stop);
          } else {
            return client.send_full_message(
                bearer_token, session_id, net::nat::holepunch::Action::offer,
                static_cast<std::uint16_t>(req_id), 0, local_offer, account_id,
                console_uid, family, stop);
          }
        }();
        if (!sent) return unexpected(OfferSendError::refused(sent.error()));
        return {};
      },
      effect);
}

auto Offer::build_conn_request(OfferRequestInput input)
    -> Expected<net::nat::holepunch::ConnRequest,
                net::nat::holepunch::ConnRequestError> {
  if (input.candidates.size() < 2U)
    return unexpected(net::nat::holepunch::ConnRequestError::invalid_schema);
  net::nat::holepunch::ConnRequest request{};
  request.sid = input.local_sid;
  request.peer_sid = input.console_sid;
  request.nat_type = 2;
  request.local_hashed_id = std::move(input.local_hashed_id);
  request.candidates = std::move(input.candidates);
  return request;
}

auto Offer::prepare(net::nat::holepunch::GatheredCandidates gathered,
                    std::uint32_t local_sid, std::uint32_t console_sid,
                    SecretArray<net::holepunch_hashed_id_size> local_hashed_id)
    -> Expected<PreparedOffer, net::nat::holepunch::ConnRequestError> {
  OfferRequestInput input{};
  input.local_sid = local_sid;
  input.console_sid = console_sid;
  input.local_hashed_id = std::move(local_hashed_id);
  input.candidates = std::move(gathered.offer_candidates);
  auto request = build_conn_request(std::move(input));
  if (!request) return unexpected(request.error());
  return PreparedOffer{std::move(gathered.local), gathered.allocation,
                       std::move(request.value())};
}

auto Offer::probe_peer(const net::nat::holepunch::ConnRequest& local_offer,
                       const net::nat::holepunch::ConnRequest& peer_offer,
                       net::nat::holepunch::BoundSockets sockets,
                       platform::DnsProvider& dns, platform::Poller& poller,
                       const platform::ClockProvider& clock,
                       platform::SecureRandomProvider& random,
                       const platform::StopToken& stop,
                       net::nat::holepunch::ProbeRetryPolicy retry)
    -> Expected<net::nat::holepunch::SelectedCandidate, OfferProbeError> {
  if (stop.stop_requested()) return unexpected(OfferProbeError::cancelled);
  auto input = net::nat::holepunch::Probe::make_peer_probe_input(local_offer,
                                                                 peer_offer);
  if (!input)
    return unexpected(
        input.error() ==
                net::nat::holepunch::PeerProbeInputError::invalid_local_offer
            ? OfferProbeError::invalid_local_offer
            : OfferProbeError::invalid_peer_offer);
  input.value().retry = retry;
  auto selected = net::nat::holepunch::Probe::probe_peer(
      std::move(sockets), dns, poller, clock, random, stop, input.value());
  if (!selected) {
    switch (selected.error()) {
      case net::nat::holepunch::PeerProbeError::cancelled:
        return unexpected(OfferProbeError::cancelled);
      case net::nat::holepunch::PeerProbeError::no_candidates:
        return unexpected(OfferProbeError::no_candidates);
      case net::nat::holepunch::PeerProbeError::invalid_sid:
        return unexpected(OfferProbeError::invalid_sid);
      case net::nat::holepunch::PeerProbeError::random_failed:
        return unexpected(OfferProbeError::random_failed);
      case net::nat::holepunch::PeerProbeError::wait_timeout:
        return unexpected(OfferProbeError::wait_timeout);
      case net::nat::holepunch::PeerProbeError::wait_net:
        return unexpected(OfferProbeError::wait_net);
      case net::nat::holepunch::PeerProbeError::wait_protocol:
        return unexpected(OfferProbeError::wait_protocol);
      case net::nat::holepunch::PeerProbeError::selection_failed:
        return unexpected(OfferProbeError::selection_failed);
    }
  }
  return std::move(selected.value());
}

auto Offer::run_psn(std::uint64_t attempt, std::uint64_t own_req_id,
                    psn::remote_play::SessionClient& session,
                    psn::remote_play::PushClient& push,
                    platform::WebSocketConnection& connection,
                    const psn::auth::BearerToken& bearer_token,
                    std::string_view session_id, std::int64_t account_id,
                    std::span<const std::byte, psn_device_uid_size> console_uid,
                    psn::rest::ConsoleFamily family,
                    const net::nat::holepunch::ConnRequest& local_offer,
                    platform::StopToken parent_stop,
                    const platform::ClockProvider& clock,
                    net::RemoteWaitDeadline deadline_microseconds,
                    const diagnostics::Trace* trace)
    -> Expected<net::nat::holepunch::ConnRequest, OfferPsnError> {
  bool own_offer_sent = false;
  std::optional<std::uint32_t> pending_console_sid;
  auto send_stop_state = std::make_shared<net::DeadlineStopState>(
      parent_stop, clock, deadline_microseconds.absolute);
  const platform::StopToken send_stop{send_stop_state};
  OfferDrive drive{
      [&]() -> Expected<net::nat::holepunch::SessionMessage, OfferPsnError> {
        for (;;) {
          auto notification = push.receive_bounded(
              connection, parent_stop, clock, deadline_microseconds);
          if (!notification)
            return unexpected(OfferPsnError::received(notification.error()));
          if (notification.value().type() !=
              psn::rest::NotificationType::session_message_created)
            continue;
          auto decoded =
              psn::remote_play::HolepunchMessage::decode(notification.value());
          if (!decoded)
            return unexpected(
                OfferPsnError{OfferPsnErrorKind::invalid_message});
          if (!own_offer_sent &&
              decoded.value().action == net::nat::holepunch::Action::offer &&
              decoded.value().conn_request) {
            auto& sid = decoded.value().conn_request->sid;
            sid = static_cast<std::uint16_t>(sid);
            pending_console_sid = sid;
          }
          return std::move(decoded.value());
        }
      },
      [&](const OfferEffect& effect) -> Expected<void, OfferPsnError> {
        const bool send_own = std::holds_alternative<SendOwnOffer>(effect);
        if (send_own && !pending_console_sid)
          return unexpected(OfferPsnError{OfferPsnErrorKind::send_failure});
        auto sent = send_effect(effect, attempt, session, bearer_token,
                                session_id, account_id, console_uid, family,
                                local_offer, send_stop);
        if (!sent) {
          const auto refusal = sent.error().session();
          const bool cancelled =
              refusal &&
              refusal->kind == psn::remote_play::SessionErrorKind::cancelled;
          const bool rejected =
              send_own && refusal &&
              refusal->kind == psn::remote_play::SessionErrorKind::http_status &&
              refusal->status >= 400 && refusal->status < 500;
          const bool best_effort = send_own || own_offer_sent;
          if (!best_effort || cancelled || rejected || !refusal)
            return unexpected(OfferPsnError::sent(
                send_failure_kind(sent.error(), parent_stop, *send_stop_state),
                sent.error()));
        }
        if (send_own) own_offer_sent = true;
        return {};
      },
      trace};

  auto result = drive.run(attempt, own_req_id);
  if (!result) return unexpected(result.error());
  auto observed = session.observe_session(
      bearer_token, session_id, psn::remote_play::SessionView::detailed,
      send_stop);
  if (!observed)
    return unexpected(OfferPsnError{
        observed.error() == psn::remote_play::CreationWaitError::timed_out
            ? OfferPsnErrorKind::timed_out
            : OfferPsnErrorKind::cancelled});
  return std::move(result.value());
}

auto Offer::send_failure_kind(const OfferSendError& error,
                              const platform::StopToken& parent,
                              const net::DeadlineStopState& send_stop)
    -> OfferPsnErrorKind {
  const auto refusal = error.session();
  if (refusal &&
      refusal->kind == psn::remote_play::SessionErrorKind::cancelled) {
    if (parent.stop_requested() && !parent.deadline_expired())
      return OfferPsnErrorKind::cancelled;
    if (send_stop.timed_out()) return OfferPsnErrorKind::timed_out;
  }
  return OfferPsnErrorKind::send_failure;
}

auto Offer::feed_inbox(OfferInbox& inbox, std::uint64_t attempt,
                       const psn::rest::Notification& notification)
    -> Expected<void, psn::remote_play::HolepunchMessage::Error> {
  auto decoded = psn::remote_play::HolepunchMessage::decode(notification);
  if (!decoded) return unexpected(decoded.error());
  inbox.on_message(attempt, std::move(decoded.value()));
  return {};
}

}  // namespace libchiaking::play::internet::holepunch
