#include "src/core/kakusan/adapters/guest_visit/visiting_player.hpp"

#include "src/core/utils/timing/deadline.hpp"

#include <utility>
#include <variant>

namespace libchiaking::kakusan {

VisitingPlayer::VisitingPlayer(platform::CryptoProvider& crypto,
                               platform::SecureRandomProvider& random,
                               const PairingContext& context,
                               std::uint8_t source_protocol) noexcept
    : channel_(crypto, random, net::kakusan::Role::guest, context),
      source_protocol_(source_protocol),
      progress_(channel_.outbox()) {}

auto VisitingPlayer::handle(Event event) -> Effects {
  std::visit(
      [&](auto& item) {
        advance(item.now_us);
        on(item);
      },
      event);
  return channel_.drain();
}

auto VisitingPlayer::on_time(std::uint64_t now_us) -> Effects {
  advance(now_us);
  return channel_.drain();
}

auto VisitingPlayer::next_deadline_us() const noexcept
    -> std::optional<std::uint64_t> {
  auto deadline = channel_.next_deadline_us();
  if (channel_.finished()) return deadline;
  deadline = timing::Deadline::earliest(deadline, admission_deadline_);
  deadline = timing::Deadline::earliest(deadline, rumble_deadline_);
  if (channel_.state() == Progress::playing)
    deadline = timing::Deadline::earliest(
        deadline,
        input_pacer_.next_send_us(allowed_input() != sent_input_, true));
  return deadline;
}

auto VisitingPlayer::binding() const noexcept
    -> std::optional<net::kakusan::SeatBinding> {
  if (!grant_) return std::nullopt;
  return grant_->binding;
}

void VisitingPlayer::advance(std::uint64_t now_us) {
  channel_.advance(now_us);
  expire();
}

void VisitingPlayer::expire() {
  if (channel_.closing()) return;
  const auto now = channel_.now();
  if (channel_.timed_out() ||
      timing::Deadline::expired(admission_deadline_, now))
    return fail(net::kakusan::Error::timeout);
  if (channel_.silent()) return leave(net::kakusan::control::Reason::timeout);
  if (timing::Deadline::expired(rumble_deadline_, now)) {
    rumble_deadline_.reset();
    channel_.outbox().push(StopRumble{});
  }
  if (auto ending = channel_.keep_alive()) return terminate(std::move(*ending));
  pump_input();
}

void VisitingPlayer::on(IceNominated&) {
  if (channel_.state() == Progress::waiting) connect();
}

void VisitingPlayer::on(DtlsEstablished& event) {
  if (channel_.state() == Progress::waiting) connect();
  if (channel_.state() != Progress::connecting || channel_.open()) return;
  channel_.establish(event);
}

void VisitingPlayer::on(TransportFailed&) {
  conclude(channel_.transport_failed());
}

void VisitingPlayer::on(InvitationExpired&) {
  if (channel_.state() == Progress::waiting)
    terminate(Ending::refused(net::kakusan::Error::expired));
}

void VisitingPlayer::on(ControlReceived& event) {
  if (channel_.finished() || !channel_.open()) return;
  std::visit([&](auto& message) { receive(message); }, event.message);
}

void VisitingPlayer::on(ControlAcked& event) {
  channel_.acknowledged(event.token);
}

void VisitingPlayer::on(MalformedData& event) {
  if (channel_.finished() || !channel_.open()) return;
  fail(event.error == net::kakusan::Error::codec
           ? net::kakusan::Error::codec
           : net::kakusan::Error::protocol);
}

void VisitingPlayer::on(LocalFailure& event) {
  if (!channel_.finished()) fail(event.error);
}

void VisitingPlayer::on(LocalCode& event) {
  if (progress_.in<player_lifecycle::AwaitingShare>() &&
      channel_.show_code(event))
    answer_pake();
}

void VisitingPlayer::on(PeerCodeEntered& event) {
  if (progress_.in<player_lifecycle::AwaitingShare>() &&
      channel_.take_code(event))
    answer_pake();
}

void VisitingPlayer::on(Cancel& event) {
  if (channel_.finished()) return;
  if (channel_.authenticated()) return leave(event.reason);
  terminate(Ending::quietly(event.reason));
}

void VisitingPlayer::on(ReceiverReady&) {
  if (channel_.state() != Progress::waiting_for_seat) return;
  (void)progress_.process(player_lifecycle::ReportSourceReady{setup_hash_});
}

void VisitingPlayer::on(ReceiverFailed&) {
  if (channel_.state() != Progress::waiting_for_seat ||
      !progress_.in<player_lifecycle::ReceiverConfigured>())
    return;
  fail(net::kakusan::Error::codec);
}

void VisitingPlayer::on(ControllerReady&) {
  if (channel_.state() != Progress::getting_ready) return;
  (void)progress_.process(player_lifecycle::ReportReady{grant_->binding});
}

void VisitingPlayer::on(FeedbackReceived& event) {
  if (channel_.state() != Progress::playing) return;
  if (event.frame.binding != grant_->binding)
    return fail(net::kakusan::Error::protocol);
  if (!feedback_freshness_.accept(event.frame.sequence)) return;
  channel_.refresh();
  if (event.frame.low_frequency != 0 || event.frame.high_frequency != 0)
    rumble_deadline_ =
        timing::Deadline::after_us(channel_.now(), rumble_stop_us);
  else
    rumble_deadline_.reset();
  channel_.outbox().push(
      ApplyRumble{event.frame.low_frequency, event.frame.high_frequency});
}

void VisitingPlayer::on(ControllerSampled& event) {
  if (channel_.finished() ||
      !net::kakusan::ControllerFrameCodec::validate_input_state(event.state))
    return;
  current_input_ = event.state;
  pump_input();
}

void VisitingPlayer::on(IdrNeeded&) {
  if (channel_.state() == Progress::playing)
    (void)channel_.send(net::kakusan::control::SourceIdr{});
}

void VisitingPlayer::receive(const net::kakusan::control::Hello& message) {
  conclude(channel_.greet(message));
}

void VisitingPlayer::receive(const net::kakusan::control::Identity& message) {
  conclude(channel_.identify(message));
}

void VisitingPlayer::receive(const net::kakusan::control::PakeA& message) {
  if (channel_.authenticated()) return fail(net::kakusan::Error::protocol);
  if (channel_.state() != Progress::verifying || host_share_ ||
      !progress_.in<player_lifecycle::AwaitingShare>())
    return terminate(Ending::pairing_failed());
  host_share_ = message.point;
  answer_pake();
}

void VisitingPlayer::receive(const net::kakusan::control::ConfirmA& message) {
  if (channel_.authenticated()) return fail(net::kakusan::Error::protocol);
  if (channel_.state() != Progress::verifying ||
      !progress_.in<player_lifecycle::ShareAnswered>() ||
      !channel_.peer_confirmed(message.mac))
    return terminate(Ending::pairing_failed());
  (void)progress_.process(player_lifecycle::ReturnConfirmation{});
  channel_.send_confirmation();
  channel_.drop_confirmations();
}

void VisitingPlayer::receive(
    const net::kakusan::control::Authenticated& message) {
  if (channel_.state() != Progress::verifying ||
      !progress_.in<player_lifecycle::ConfirmationReturned>())
    return fail(net::kakusan::Error::protocol);
  if (auto ending = channel_.check_authenticated(message))
    return terminate(std::move(*ending));
  channel_.send_authenticated();
  channel_.admit();
}

void VisitingPlayer::receive(const net::kakusan::control::AdmissionPending&) {
  if (channel_.state() != Progress::waiting_for_seat ||
      !progress_.process(player_lifecycle::AwaitAdmission{}))
    return fail(net::kakusan::Error::protocol);
  channel_.refresh();
  admission_deadline_ =
      timing::Deadline::after_us(channel_.now(), admission_timeout_us);
}

void VisitingPlayer::receive(net::kakusan::control::SourceSetup& message) {
  if (channel_.state() != Progress::waiting_for_seat ||
      !progress_.in<player_lifecycle::AdmissionAwaited>() ||
      message.source_protocol != source_protocol_)
    return fail(net::kakusan::Error::protocol);
  const auto hash = channel_.setup_hash(message);
  if (!hash) return fail(net::kakusan::Error::codec);
  channel_.refresh();
  (void)progress_.process(player_lifecycle::Configure{});
  admission_deadline_.reset();
  setup_hash_ = hash.value();
  channel_.outbox().push(ConfigureReceiver{std::move(message)});
}

void VisitingPlayer::receive(
    const net::kakusan::control::SeatGranted& message) {
  if (channel_.state() != Progress::waiting_for_seat ||
      !progress_.in<player_lifecycle::SourceReadySent>())
    return fail(net::kakusan::Error::protocol);
  channel_.refresh();
  grant_ = message;
  channel_.enter(Progress::getting_ready);
  (void)progress_.process(player_lifecycle::TakeGrant{message});
}

void VisitingPlayer::receive(const net::kakusan::control::Active& message) {
  if (channel_.state() != Progress::getting_ready ||
      !progress_.in<player_lifecycle::ReadySent>() ||
      message.binding != grant_->binding)
    return fail(net::kakusan::Error::protocol);
  channel_.refresh();
  activate();
}

void VisitingPlayer::receive(
    const net::kakusan::control::SeatRevoked& message) {
  if (!channel_.authenticated() || !grant_ ||
      message.binding != grant_->binding)
    return fail(net::kakusan::Error::protocol);
  terminate(Ending::ended_by_peer(message.reason));
}

void VisitingPlayer::receive(const net::kakusan::control::End& message) {
  if (!channel_.authenticated()) return fail(net::kakusan::Error::protocol);
  terminate(Ending::ended_by_peer(message.reason));
}

void VisitingPlayer::receive(const net::kakusan::control::Ping& message) {
  conclude(channel_.answer_ping(message));
}

void VisitingPlayer::receive(const net::kakusan::control::Pong& message) {
  conclude(channel_.take_pong(message));
}

void VisitingPlayer::receive(const net::kakusan::control::Failure& message) {
  terminate(channel_.peer_failed(message));
}

void VisitingPlayer::receive(const net::kakusan::control::PakeB&) {
  terminate(channel_.unexpected_pake());
}

void VisitingPlayer::receive(const net::kakusan::control::ConfirmB&) {
  terminate(channel_.unexpected_pake());
}

template <class Message>
void VisitingPlayer::receive(const Message&) {
  fail(net::kakusan::Error::protocol);
}

auto VisitingPlayer::allowed_input() const noexcept
    -> net::kakusan::InputState {
  auto input = current_input_;
  input.buttons &= grant_->allowed_buttons;
  return input;
}

void VisitingPlayer::connect() {
  channel_.await_dtls();
  channel_.enter(Progress::connecting);
}

void VisitingPlayer::answer_pake() {
  if (!channel_.codes_ready() || !host_share_ ||
      !progress_.in<player_lifecycle::AwaitingShare>())
    return;
  auto exchange = channel_.exchange();
  if (!exchange) return terminate(std::move(exchange).error());
  const auto share = exchange->share();
  if (auto ending = channel_.confirm(*exchange, *host_share_))
    return terminate(std::move(*ending));
  (void)progress_.process(player_lifecycle::AnswerShare{share});
}

void VisitingPlayer::activate() {
  active_at_ = channel_.now();
  channel_.enter(Progress::playing);
  (void)progress_.process(player_lifecycle::Activate{grant_->binding});
  input_pacer_.start(channel_.now());
  pump_input();
}

void VisitingPlayer::pump_input() {
  if (channel_.state() != Progress::playing) return;
  const auto now = channel_.now();
  const auto input = allowed_input();
  const auto due = input_pacer_.next_send_us(input != sent_input_, true);
  if (!due || now < *due) return;
  net::kakusan::InputFrame frame;
  frame.binding = grant_->binding;
  frame.sequence = input_sequence_++;
  frame.elapsed_ms = channel_.milliseconds_since(active_at_);
  frame.state = input;
  sent_input_ = input;
  input_pacer_.mark(now);
  channel_.outbox().push(SendInput{frame});
}

void VisitingPlayer::fail(net::kakusan::Error error) {
  terminate(channel_.failure(error));
}

void VisitingPlayer::leave(net::kakusan::control::Reason reason) {
  terminate(Ending::announced(net::kakusan::control::Leave{reason}, reason));
}

void VisitingPlayer::conclude(std::optional<Ending> ending) {
  if (ending) terminate(std::move(*ending));
}

void VisitingPlayer::terminate(Ending ending) {
  if (channel_.finished()) return;
  (void)progress_.process(player_lifecycle::Vacate{});
  admission_deadline_.reset();
  rumble_deadline_.reset();
  input_pacer_.stop();
  channel_.end(std::move(ending));
}

}  // namespace libchiaking::kakusan
