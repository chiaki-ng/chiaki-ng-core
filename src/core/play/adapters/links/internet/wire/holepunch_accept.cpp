#include "src/core/play/adapters/links/internet/wire/holepunch_accept.hpp"

#include <boost/sml.hpp>

namespace libchiaking::play::internet::holepunch {

namespace accept_wait_detail {
struct Idle {};
struct Waiting {};
struct Pending {};
struct Ready {};
struct Failed {};
struct Begin {};
struct Accept {};
struct Ack {};
struct Stop {};
struct Definition {
  auto operator()() const {
    using namespace boost::sml;
    return make_transition_table(
        *state<Idle> + event<Begin> = state<Waiting>,
        state<Waiting> + event<Accept> = state<Pending>,
        state<Pending> + event<Ack> = state<Ready>,
        state<Waiting> + event<Stop> = state<Failed>,
        state<Pending> + event<Stop> = state<Failed>);
  }
};
}  // namespace accept_wait_detail
class AcceptWait::Machine {
 public:
  boost::sml::sm<accept_wait_detail::Definition> machine;
};
AcceptWait::AcceptWait() : machine_(std::make_unique<Machine>()) {}
AcceptWait::~AcceptWait() = default;
AcceptWait::AcceptWait(AcceptWait&&) noexcept = default;
AcceptWait& AcceptWait::operator=(AcceptWait&&) noexcept = default;
bool AcceptWait::begin(std::uint64_t attempt) {
  if (state() != AcceptWaitState::idle || attempt == 0) return false;
  attempt_ = attempt;
  machine_->machine.process_event(accept_wait_detail::Begin{});
  return true;
}

void AcceptWait::on_message(
    std::uint64_t attempt, const net::nat::holepunch::SessionMessage& message) {
  if (attempt != attempt_ || state() != AcceptWaitState::waiting_accept) return;
  if (message.action == net::nat::holepunch::Action::terminate) {
    machine_->machine.process_event(accept_wait_detail::Stop{});
    return;
  }
  if (message.action != net::nat::holepunch::Action::accept) return;
  effect_ = AckPeerAccept{attempt, message.req_id};
  machine_->machine.process_event(accept_wait_detail::Accept{});
}

auto AcceptWait::take_ack() -> std::optional<AckPeerAccept> {
  auto out = effect_;
  effect_.reset();
  return out;
}

bool AcceptWait::ack_sent(std::uint64_t attempt) {
  if (attempt != attempt_ || state() != AcceptWaitState::ack_pending)
    return false;
  machine_->machine.process_event(accept_wait_detail::Ack{});
  return true;
}

bool AcceptWait::timeout(std::uint64_t attempt) {
  if (attempt != attempt_ || (state() != AcceptWaitState::waiting_accept &&
                              state() != AcceptWaitState::ack_pending))
    return false;
  effect_.reset();
  machine_->machine.process_event(accept_wait_detail::Stop{});
  return true;
}

bool AcceptWait::cancel(std::uint64_t attempt) { return timeout(attempt); }

auto AcceptWait::state() const noexcept -> AcceptWaitState {
  using namespace boost::sml;
  using namespace accept_wait_detail;
  if (machine_->machine.is(boost::sml::state<Idle>))
    return AcceptWaitState::idle;
  if (machine_->machine.is(boost::sml::state<Waiting>))
    return AcceptWaitState::waiting_accept;
  if (machine_->machine.is(boost::sml::state<Pending>))
    return AcceptWaitState::ack_pending;
  if (machine_->machine.is(boost::sml::state<Ready>))
    return AcceptWaitState::ready;
  return AcceptWaitState::failed;
}

AcceptDrive::AcceptDrive(Receive receive, SendAck send_ack,
                         const diagnostics::Trace* trace)
    : receive_(std::move(receive)),
      send_ack_(std::move(send_ack)),
      trace_(trace) {}

auto AcceptDrive::run(std::uint64_t attempt)
    -> Expected<AcceptWait, AcceptPsnError> {
  if (!wait_.begin(attempt))
    return unexpected(AcceptPsnError{AcceptPsnErrorKind::invalid_attempt});
  trace_state();
  for (;;) {
    auto received = receive_();
    if (!received) return unexpected(received.error());
    const auto prior = wait_.state();
    wait_.on_message(attempt, received.value());
    if (prior != wait_.state()) trace_state();
    if (wait_.state() == AcceptWaitState::failed)
      return unexpected(AcceptPsnError{AcceptPsnErrorKind::terminated});
    if (wait_.state() != AcceptWaitState::ack_pending) continue;
    auto ack = wait_.take_ack();
    if (!ack) return unexpected(AcceptPsnError{AcceptPsnErrorKind::terminated});
    auto sent = send_ack_(*ack);
    if (!sent) return unexpected(sent.error());
    if (!wait_.ack_sent(attempt))
      return unexpected(AcceptPsnError{AcceptPsnErrorKind::send_failure});
    trace_state();
    return std::move(wait_);
  }
}

void AcceptDrive::trace_state() const noexcept {
  if (trace_)
    trace_->record_state(diagnostics::Operation::accept_state, wait_.state());
}

auto Accept::send_ack(
    const AckPeerAccept& ack, std::uint64_t current_attempt,
    psn::remote_play::SessionClient& client,
    const psn::auth::BearerToken& bearer_token, std::string_view session_id,
    std::int64_t account_id,
    std::span<const std::byte, psn_device_uid_size> console_uid,
    psn::rest::ConsoleFamily family, const platform::StopToken& stop)
    -> Expected<void, AcceptAckError> {
  if (ack.attempt == 0 || ack.attempt != current_attempt)
    return unexpected(AcceptAckError::stale_attempt());
  auto sent = client.send_short_message(
      bearer_token, session_id, net::nat::holepunch::Action::result,
      ack.peer_req_id, 0, account_id, console_uid, family, stop);
  if (!sent) return unexpected(AcceptAckError::refused(sent.error()));
  return {};
}

bool Accept::legacy_private_stub(std::string_view address) noexcept {
  constexpr std::string_view stubs[] = {
      "10.",     "192.168.", "172.16.", "172.17.", "172.18.", "172.19.",
      "172.20.", "172.21.",  "172.22.", "172.23.", "172.24.", "172.25.",
      "172.26.", "172.27.",  "172.28.", "172.29.", "172.30.", "172.31.",
      "FC",      "fc",       "FD",      "fd",
  };
  for (const auto stub : stubs) {
    if (address == stub) return true;
  }
  return false;
}

auto Accept::prepare_candidate(const net::nat::holepunch::Candidate& selected,
                               const net::nat::holepunch::Candidate& local,
                               const net::nat::holepunch::Candidate& remote)
    -> net::nat::holepunch::Candidate {
  auto updated = selected;
  const bool use_local =
      selected.type == net::nat::holepunch::CandidateType::local ||
      (selected.type == net::nat::holepunch::CandidateType::derived &&
       legacy_private_stub(selected.addr));
  const auto& mapped = use_local ? local : remote;
  updated.mapped_addr = mapped.addr;
  updated.mapped_port = mapped.port;
  return updated;
}

auto Accept::build_request(std::uint32_t sid, std::uint32_t peer_sid,
                           const net::nat::holepunch::Candidate& selected,
                           const net::nat::holepunch::Candidate& local,
                           const net::nat::holepunch::Candidate& remote)
    -> net::nat::holepunch::ConnRequest {
  net::nat::holepunch::ConnRequest request{};
  request.sid = sid;
  request.peer_sid = peer_sid;
  request.candidates.push_back(prepare_candidate(selected, local, remote));
  return request;
}

auto Accept::send_request(
    std::uint64_t attempt, std::uint64_t current_attempt,
    std::uint64_t local_req_id, const net::nat::holepunch::ConnRequest& request,
    psn::remote_play::SessionClient& client,
    const psn::auth::BearerToken& bearer_token, std::string_view session_id,
    std::int64_t account_id,
    std::span<const std::byte, psn_device_uid_size> console_uid,
    psn::rest::ConsoleFamily family, const platform::StopToken& stop)
    -> Expected<void, AcceptSendError> {
  if (attempt == 0 || attempt != current_attempt)
    return unexpected(AcceptSendError::stale_attempt());
  if (local_req_id == 0 ||
      local_req_id > std::numeric_limits<std::uint16_t>::max())
    return unexpected(AcceptSendError::invalid_req_id());
  auto sent = client.send_full_message(
      bearer_token, session_id, net::nat::holepunch::Action::accept,
      static_cast<std::uint16_t>(local_req_id), 0, request, account_id,
      console_uid, family, stop);
  if (!sent) return unexpected(AcceptSendError::refused(sent.error()));
  return {};
}

auto Accept::send(std::uint64_t attempt, std::uint64_t current_attempt,
                  std::uint64_t local_req_id, std::uint32_t sid,
                  std::uint32_t peer_sid,
                  const net::nat::holepunch::Candidate& selected,
                  const net::nat::holepunch::Candidate& local,
                  const net::nat::holepunch::Candidate& remote,
                  psn::remote_play::SessionClient& client,
                  const psn::auth::BearerToken& bearer_token,
                  std::string_view session_id, std::int64_t account_id,
                  std::span<const std::byte, psn_device_uid_size> console_uid,
                  psn::rest::ConsoleFamily family,
                  const platform::StopToken& stop)
    -> Expected<void, AcceptSendError> {
  if (attempt == 0 || attempt != current_attempt)
    return unexpected(AcceptSendError::stale_attempt());
  if (local_req_id == 0 ||
      local_req_id > std::numeric_limits<std::uint16_t>::max())
    return unexpected(AcceptSendError::invalid_req_id());
  const auto request = build_request(sid, peer_sid, selected, local, remote);
  return send_request(attempt, current_attempt, local_req_id, request, client,
                      bearer_token, session_id, account_id, console_uid, family,
                      stop);
}

auto Accept::run_psn(
    std::uint64_t attempt, psn::remote_play::SessionClient& session,
    psn::remote_play::PushClient& push,
    platform::WebSocketConnection& connection,
    const psn::auth::BearerToken& bearer_token, std::string_view session_id,
    std::int64_t account_id,
    std::span<const std::byte, psn_device_uid_size> console_uid,
    psn::rest::ConsoleFamily family, platform::StopToken parent_stop,
    const platform::ClockProvider& clock,
    net::RemoteWaitDeadline deadline_microseconds,
    const diagnostics::Trace* trace) -> Expected<AcceptWait, AcceptPsnError> {
  auto send_stop_state = std::make_shared<net::DeadlineStopState>(
      parent_stop, clock, deadline_microseconds.absolute);
  const platform::StopToken send_stop{send_stop_state};
  AcceptDrive drive{
      [&]() -> Expected<net::nat::holepunch::SessionMessage, AcceptPsnError> {
        for (;;) {
          auto notification = push.receive_bounded(
              connection, parent_stop, clock, deadline_microseconds);
          if (!notification)
            return unexpected(AcceptPsnError::received(notification.error()));
          if (notification.value().type() !=
              psn::rest::NotificationType::session_message_created)
            continue;
          auto decoded =
              psn::remote_play::HolepunchMessage::decode(notification.value());
          if (!decoded)
            return unexpected(
                AcceptPsnError{AcceptPsnErrorKind::invalid_message});
          return std::move(decoded.value());
        }
      },
      [&](const AckPeerAccept& ack) -> Expected<void, AcceptPsnError> {
        auto sent = send_ack(ack, attempt, session, bearer_token, session_id,
                             account_id, console_uid, family, send_stop);
        if (!sent)
          return unexpected(AcceptPsnError::sent(
              send_failure_kind(sent.error(), parent_stop, *send_stop_state),
              sent.error()));
        return {};
      },
      trace};
  return drive.run(attempt);
}

auto Accept::send_failure_kind(const AcceptAckError& error,
                               const platform::StopToken& parent,
                               const net::DeadlineStopState& send_stop)
    -> AcceptPsnErrorKind {
  const auto refusal = error.session();
  if (refusal &&
      refusal->kind == psn::remote_play::SessionErrorKind::cancelled) {
    if (parent.stop_requested() && !parent.deadline_expired())
      return AcceptPsnErrorKind::cancelled;
    if (send_stop.timed_out()) return AcceptPsnErrorKind::timed_out;
  }
  return AcceptPsnErrorKind::send_failure;
}

auto Accept::feed_wait(AcceptWait& wait, std::uint64_t attempt,
                       const psn::rest::Notification& notification)
    -> Expected<void, psn::remote_play::HolepunchMessage::Error> {
  auto decoded = psn::remote_play::HolepunchMessage::decode(notification);
  if (!decoded) return unexpected(decoded.error());
  wait.on_message(attempt, decoded.value());
  return {};
}

auto Accept::build_selected(
    const PreparedOffer& ours, const net::nat::holepunch::ConnRequest& console,
    const net::nat::holepunch::SelectedCandidate& selected)
    -> Expected<net::nat::holepunch::ConnRequest, SelectedAcceptError> {
  if (!selected.derived &&
      selected.candidate_index >= console.candidates.size())
    return unexpected(SelectedAcceptError::candidate_index_out_of_range);
  if (ours.request.sid > UINT16_MAX || console.sid > UINT16_MAX)
    return unexpected(SelectedAcceptError::sid_out_of_range);
  const auto& candidates = ours.request.candidates;
  if (ours.local.type != net::nat::holepunch::CandidateType::local ||
      candidates.size() < 2U ||
      candidates.back().type != net::nat::holepunch::CandidateType::local ||
      candidates[candidates.size() - 2U].type !=
          net::nat::holepunch::CandidateType::static_)
    return unexpected(SelectedAcceptError::malformed_local_offer);
  net::nat::holepunch::Candidate derived{
      net::nat::holepunch::CandidateType::derived,
      platform::to_string(selected.peer.address),
      selected.peer.address.is_ipv4() ? "0.0.0.0" : "0:0:0:0:0:0:0:0",
      selected.peer.port, 0};
  return build_request(
      ours.request.sid, console.sid,
      selected.derived ? derived : console.candidates[selected.candidate_index],
      ours.local, candidates.front());
}

auto Accept::run_selected_exchange(
    platform::NetworkProvider& network, const PreparedOffer& ours,
    const net::nat::holepunch::ConnRequest& console,
    net::nat::holepunch::SelectedCandidate selected, std::uint64_t attempt,
    std::uint64_t local_req_id, psn::remote_play::SessionClient& session,
    psn::remote_play::PushClient& push,
    platform::WebSocketConnection& connection,
    const psn::auth::BearerToken& bearer_token, std::string_view session_id,
    std::int64_t account_id,
    std::span<const std::byte, psn_device_uid_size> console_uid,
    psn::rest::ConsoleFamily family, platform::StopToken parent_stop,
    const platform::ClockProvider& clock,
    net::RemoteWaitDeadline deadline_microseconds,
    const diagnostics::Trace* trace)
    -> Expected<SelectedAcceptExchangeResult, SelectedAcceptExchangeError> {
  auto request = build_selected(ours, console, selected);
  if (!request) return unexpected(SelectedAcceptExchangeError{request.error()});
  auto send_stop_state = std::make_shared<net::DeadlineStopState>(
      parent_stop, clock, deadline_microseconds.absolute);
  const platform::StopToken send_stop{send_stop_state};
  auto sent = send_request(attempt, attempt, local_req_id, request.value(),
                           session, bearer_token, session_id, account_id,
                           console_uid, family, send_stop);
  if (!sent) return unexpected(SelectedAcceptExchangeError{sent.error()});
  auto ready =
      run_psn(attempt, session, push, connection, bearer_token, session_id,
              account_id, console_uid, family, std::move(parent_stop), clock,
              deadline_microseconds, trace);
  if (!ready) return unexpected(SelectedAcceptExchangeError{ready.error()});
  auto poller = network.pollers().create_poller();
  if (!selected.socket || !poller || !poller.value())
    return unexpected(
        SelectedAcceptExchangeError{net::nat::holepunch::ProbeWaitError::net});
  net::nat::holepunch::CandidateProbe profile{};
  std::copy(ours.request.local_hashed_id.begin(),
            ours.request.local_hashed_id.end(),
            profile.local_hashed_id.begin());
  std::copy(console.local_hashed_id.begin(), console.local_hashed_id.end(),
            profile.console_hashed_id.begin());
  profile.local_sid = static_cast<std::uint16_t>(ours.request.sid);
  profile.console_sid = static_cast<std::uint16_t>(console.sid);
  auto drained =
      diagnostics::observe(trace, diagnostics::Operation::late_probe, [&] {
        return net::nat::holepunch::Probe::answer_candidate_requests(
            *selected.socket, *poller.value(), selected.peer, profile,
            send_stop, true);
      });
  if (!drained) return unexpected(SelectedAcceptExchangeError{drained.error()});
  return SelectedAcceptExchangeResult{std::move(selected),
                                      std::move(ready.value())};
}

auto Accept::handoff_ctrl_rudp(platform::NetworkProvider& network,
                               net::nat::holepunch::SelectedCandidate selected,
                               const AcceptWait& accept_wait)
    -> Expected<CtrlRudpHandoff, CtrlRudpHandoffError> {
  using Error = CtrlRudpHandoffError;
  if (accept_wait.state() != AcceptWaitState::ready)
    return unexpected(Error::accept_not_ready);
  if (!selected.socket) return unexpected(Error::null_socket);
  if (&selected.socket->owner() != &network)
    return unexpected(Error::foreign_provider);
  if (selected.peer.port == 0 || selected.peer.address.is_unspecified())
    return unexpected(Error::invalid_peer);
  const auto local = selected.socket->local_endpoint();
  if (!local || local.value().port == 0)
    return unexpected(Error::unbound_socket);
  if (local.value().address.family() != selected.peer.address.family())
    return unexpected(Error::invalid_peer);
  auto transport =
      net::transport::datagram::ProviderUdpTransport::adopt_connected(
          network, std::move(selected.socket), selected.peer);
  if (!transport) return unexpected(Error::adoption_failed);
  return CtrlRudpHandoff{
      std::move(transport.value()), local.value(), selected.peer, {}};
}

auto Accept::transport_ctrl(
    platform::NetworkProvider& network, const PreparedOffer& ours,
    const net::nat::holepunch::ConnRequest& console,
    net::nat::holepunch::SelectedCandidate selected, std::uint64_t attempt,
    std::uint64_t local_req_id, psn::remote_play::SessionClient& session,
    psn::remote_play::PushClient& push,
    platform::WebSocketConnection& connection,
    const psn::auth::BearerToken& bearer_token, std::string_view session_id,
    std::int64_t account_id,
    std::span<const std::byte, psn_device_uid_size> console_uid,
    psn::rest::ConsoleFamily family, platform::StopToken parent_stop,
    const platform::ClockProvider& clock,
    net::RemoteWaitDeadline deadline_microseconds,
    const diagnostics::Trace* trace)
    -> Expected<CtrlRudpHandoff, CtrlAcceptTransportError> {
  auto exchanged = run_selected_exchange(
      network, ours, console, std::move(selected), attempt, local_req_id,
      session, push, connection, bearer_token, session_id, account_id,
      console_uid, family, std::move(parent_stop), clock, deadline_microseconds,
      trace);
  if (!exchanged)
    return unexpected(CtrlAcceptTransportError{exchanged.error()});
  auto handoff = handoff_ctrl_rudp(
      network, std::move(exchanged.value().selected), exchanged.value().ready);
  if (!handoff) return unexpected(CtrlAcceptTransportError{handoff.error()});
  handoff.value().registration_local_address = ours.local.addr;
  return std::move(handoff.value());
}

}  // namespace libchiaking::play::internet::holepunch
