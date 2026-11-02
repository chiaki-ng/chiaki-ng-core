#include "src/core/kakusan/adapters/shared_session/invited_player.hpp"

#include "src/core/net/kakusan/peer_link.hpp"
#include "src/core/utils/memory/constant_time.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <utility>
#include <variant>

namespace libchiaking::kakusan {

auto InvitedPlayer::valid_offer(
    const net::kakusan::control::SeatGranted& offer) noexcept -> bool {
  return offer.capacity >= 1 && offer.binding.seat >= 1 &&
         offer.binding.seat <= offer.capacity &&
         offer.binding.generation >= 1 && offer.local_seats >= 1 &&
         offer.local_seats <= offer.capacity &&
         (offer.allowed_buttons & ~net::kakusan::button_mask) == 0 &&
         (offer.allowed_buttons & net::kakusan::required_buttons) ==
             net::kakusan::required_buttons;
}

InvitedPlayer::InvitedPlayer(platform::CryptoProvider& crypto,
                             platform::SecureRandomProvider& random) noexcept
    : channel_(crypto, random, net::kakusan::Role::host, std::nullopt),
      progress_(channel_.outbox()) {}

auto InvitedPlayer::handle(Event event) -> Effects {
  std::visit(
      [&](auto& item) {
        advance(item.now_us);
        on(item);
      },
      event);
  return channel_.drain();
}

auto InvitedPlayer::on_time(std::uint64_t now_us) -> Effects {
  advance(now_us);
  return channel_.drain();
}

auto InvitedPlayer::next_deadline_us() const noexcept
    -> std::optional<std::uint64_t> {
  auto deadline = channel_.next_deadline_us();
  if (channel_.finished()) return deadline;
  deadline = timing::Deadline::earliest(deadline, connectivity_deadline_);
  deadline = timing::Deadline::earliest(deadline, admission_deadline_);
  deadline = timing::Deadline::earliest(deadline, source_ready_deadline_);
  deadline = timing::Deadline::earliest(deadline, ready_deadline_);
  if (channel_.state() != Progress::playing) return deadline;
  deadline = timing::Deadline::earliest(
      deadline, timing::Deadline::after_us(last_input_, input_revoke_us));
  if (progress_.in<player_lifecycle::InputFresh>())
    deadline = timing::Deadline::earliest(
        deadline, timing::Deadline::after_us(last_input_, input_neutral_us));
  if (progress_.in<player_lifecycle::Streaming>())
    deadline = timing::Deadline::earliest(
        deadline, feedback_pacer_.next_send_us(rumble_changed(), rumbling()));
  return deadline;
}

auto InvitedPlayer::binding() const noexcept
    -> std::optional<net::kakusan::SeatBinding> {
  return progress_.binding();
}

void InvitedPlayer::advance(std::uint64_t now_us) {
  channel_.advance(now_us);
  expire();
}

void InvitedPlayer::expire() {
  if (channel_.closing()) return;
  const auto now = channel_.now();
  const bool playing = channel_.state() == Progress::playing;
  if (timing::Deadline::expired(connectivity_deadline_, now))
    return fail(net::kakusan::Error::no_path);
  if (channel_.timed_out() ||
      timing::Deadline::expired(admission_deadline_, now) ||
      timing::Deadline::expired(source_ready_deadline_, now))
    return fail(net::kakusan::Error::timeout);
  if (timing::Deadline::expired(ready_deadline_, now) ||
      (playing &&
       timing::Deadline::expired(
           timing::Deadline::after_us(last_input_, input_revoke_us), now)))
    return revoke(net::kakusan::control::Reason::timeout);
  if (channel_.silent()) return end(net::kakusan::control::Reason::timeout);
  if (playing && progress_.in<player_lifecycle::InputFresh>() &&
      timing::Deadline::expired(
          timing::Deadline::after_us(last_input_, input_neutral_us), now))
    (void)progress_.process(player_lifecycle::Neutralize{});
  if (auto ending = channel_.keep_alive()) return terminate(std::move(*ending));
  pump_feedback();
}

void InvitedPlayer::on(ReplyImported& event) {
  if (channel_.state() != Progress::waiting) return;
  channel_.adopt(event.context);
  connectivity_deadline_ = timing::Deadline::after_us(
      channel_.now(), net::kakusan::connectivity_timeout_us);
  channel_.enter(Progress::connecting);
}

void InvitedPlayer::on(IceNominated&) {
  if (channel_.state() != Progress::connecting ||
      !progress_.process(player_lifecycle::Nominate{}))
    return;
  connectivity_deadline_.reset();
  channel_.await_dtls();
}

void InvitedPlayer::on(DtlsEstablished& event) {
  if (channel_.state() != Progress::connecting || channel_.open() ||
      !channel_.has_context())
    return;
  if (!progress_.in<player_lifecycle::Nominated>())
    (void)progress_.process(player_lifecycle::Nominate{});
  connectivity_deadline_.reset();
  channel_.establish(event);
}

void InvitedPlayer::on(TransportFailed&) {
  conclude(channel_.transport_failed());
}

void InvitedPlayer::on(InvitationExpired&) {
  if (channel_.state() == Progress::waiting)
    terminate(Ending::refused(net::kakusan::Error::expired));
}

void InvitedPlayer::on(ControlReceived& event) {
  if (channel_.finished() || !channel_.open()) return;
  std::visit([&](auto& message) { receive(message); }, event.message);
}

void InvitedPlayer::on(ControlAcked& event) {
  if (channel_.acknowledged(event.token)) return;
  const auto now = channel_.now();
  if (event.token == setup_token_) {
    setup_token_ = 0;
    if (progress_.in<player_lifecycle::SetupOffered>())
      source_ready_deadline_ =
          timing::Deadline::after_us(now, source_ready_timeout_us);
  } else if (event.token == grant_token_) {
    grant_token_ = 0;
    if (progress_.in<player_lifecycle::Granted>())
      ready_deadline_ = timing::Deadline::after_us(now, ready_timeout_us);
  } else if (event.token == active_token_) {
    active_token_ = 0;
    if (channel_.state() == Progress::playing) start_media();
  }
}

void InvitedPlayer::on(MalformedData&) {
  if (channel_.finished() || !channel_.open()) return;
  fail(net::kakusan::Error::protocol);
}

void InvitedPlayer::on(LocalFailure& event) {
  if (!channel_.finished()) fail(event.error);
}

void InvitedPlayer::on(LocalCode& event) {
  if (progress_.in<player_lifecycle::AwaitingCodes>() &&
      channel_.show_code(event))
    begin_pairing();
}

void InvitedPlayer::on(PeerCodeEntered& event) {
  if (progress_.in<player_lifecycle::AwaitingCodes>() &&
      channel_.take_code(event))
    begin_pairing();
}

void InvitedPlayer::on(Cancel& event) {
  if (channel_.finished()) return;
  if (channel_.authenticated()) return end(event.reason);
  terminate(Ending::quietly(event.reason));
}

void InvitedPlayer::on(AdmissionApproved&) {
  if (channel_.finished()) return;
  (void)progress_.process(player_lifecycle::Approve{});
  maybe_reserve();
}

void InvitedPlayer::on(SeatReserved& event) {
  if (channel_.state() != Progress::waiting_for_seat ||
      !progress_.process(player_lifecycle::Reserve{event.binding})) {
    const auto binding = progress_.binding();
    if (!binding || *binding != event.binding)
      channel_.outbox().push(ReleaseSeat{event.binding});
    return;
  }
  admission_deadline_ =
      timing::Deadline::after_us(channel_.now(), admission_timeout_us);
}

void InvitedPlayer::on(NoCapacity&) {
  if (channel_.state() != Progress::waiting_for_seat ||
      !progress_.in<player_lifecycle::Requested>())
    return;
  fail(net::kakusan::Error::no_capacity);
}

void InvitedPlayer::on(SourceJoined& event) {
  if (channel_.state() != Progress::waiting_for_seat ||
      !progress_.in<player_lifecycle::Held>())
    return;
  const net::kakusan::control::SeatGranted offer{
      *progress_.binding(), event.capacity, event.local_seats,
      event.allowed_buttons};
  const auto hash = channel_.setup_hash(event.setup);
  if (!hash || !valid_offer(offer))
    return fail(net::kakusan::Error::source_join_failed);
  (void)progress_.process(player_lifecycle::OfferSetup{});
  admission_deadline_.reset();
  offer_ = offer;
  setup_hash_ = hash.value();
  setup_token_ =
      channel_.send(net::kakusan::control::Message{std::move(event.setup)});
}

void InvitedPlayer::on(SourceJoinFailed&) {
  if (channel_.state() != Progress::waiting_for_seat ||
      !progress_.in<player_lifecycle::Held>())
    return;
  fail(net::kakusan::Error::source_join_failed);
}

void InvitedPlayer::on(CapacityRevoked&) {
  if (channel_.finished() || !progress_.binding()) return;
  revoke(net::kakusan::control::Reason::capacity);
}

void InvitedPlayer::on(SourceEnded&) {
  if (channel_.finished()) return;
  if (channel_.authenticated())
    return end(net::kakusan::control::Reason::source);
  fail(net::kakusan::Error::source_ended);
}

void InvitedPlayer::on(InputReceived& event) {
  if (channel_.state() != Progress::playing) return;
  if (event.frame.binding != *progress_.binding())
    return fail(net::kakusan::Error::protocol);
  if (!input_freshness_.accept(event.frame.sequence)) return;
  auto input = event.frame.state;
  input.buttons &= offer_->allowed_buttons;
  if (!net::kakusan::ControllerFrameCodec::validate_input_state(input))
    return fail(net::kakusan::Error::protocol);
  last_input_ = channel_.now();
  (void)progress_.process(player_lifecycle::Touch{});
  channel_.refresh();
  channel_.outbox().push(ApplyInput{*progress_.binding(), input});
}

void InvitedPlayer::on(RumbleSampled& event) {
  if (channel_.finished()) return;
  rumble_low_ = event.low_frequency;
  rumble_high_ = event.high_frequency;
  pump_feedback();
}

void InvitedPlayer::receive(const net::kakusan::control::Hello& message) {
  conclude(channel_.greet(message));
}

void InvitedPlayer::receive(const net::kakusan::control::Identity& message) {
  conclude(channel_.identify(message));
}

void InvitedPlayer::receive(const net::kakusan::control::PakeB& message) {
  if (channel_.authenticated()) return fail(net::kakusan::Error::protocol);
  if (channel_.state() != Progress::verifying ||
      !progress_.in<player_lifecycle::ShareOffered>())
    return terminate(Ending::pairing_failed());
  auto ending = channel_.confirm(*exchange_, message.point);
  exchange_.reset();
  if (ending) return terminate(std::move(*ending));
  (void)progress_.process(player_lifecycle::SendConfirmation{});
  channel_.send_confirmation();
}

void InvitedPlayer::receive(const net::kakusan::control::ConfirmB& message) {
  if (channel_.authenticated()) return fail(net::kakusan::Error::protocol);
  if (channel_.state() != Progress::verifying ||
      !progress_.in<player_lifecycle::ConfirmationSent>())
    return terminate(Ending::pairing_failed());
  const bool verified = channel_.peer_confirmed(message.mac);
  channel_.drop_confirmations();
  if (!verified) return terminate(Ending::pairing_failed());
  (void)progress_.process(player_lifecycle::ConfirmPeer{});
  channel_.send_authenticated();
}

void InvitedPlayer::receive(
    const net::kakusan::control::Authenticated& message) {
  if (channel_.state() != Progress::verifying ||
      !progress_.in<player_lifecycle::PeerConfirmed>())
    return fail(net::kakusan::Error::protocol);
  if (auto ending = channel_.check_authenticated(message))
    return terminate(std::move(*ending));
  channel_.admit();
  maybe_reserve();
}

void InvitedPlayer::receive(const net::kakusan::control::SourceReady& message) {
  if (channel_.state() != Progress::waiting_for_seat ||
      !progress_.in<player_lifecycle::SetupOffered>() ||
      !memory::constant_time_equal(message.setup_hash, setup_hash_))
    return fail(net::kakusan::Error::protocol);
  channel_.refresh();
  source_ready_deadline_.reset();
  setup_token_ = 0;
  (void)progress_.process(player_lifecycle::Grant{});
  grant_token_ = channel_.send(*offer_);
  channel_.enter(Progress::getting_ready);
}

void InvitedPlayer::receive(const net::kakusan::control::Ready& message) {
  if (channel_.state() != Progress::getting_ready ||
      !progress_.in<player_lifecycle::Granted>() ||
      message.binding != *progress_.binding())
    return fail(net::kakusan::Error::protocol);
  channel_.refresh();
  (void)progress_.process(player_lifecycle::TakeReady{});
  ready_deadline_.reset();
  grant_token_ = 0;
  activate();
}

void InvitedPlayer::receive(const net::kakusan::control::SourceIdr&) {
  if (channel_.state() != Progress::playing)
    return fail(net::kakusan::Error::protocol);
  channel_.refresh();
  channel_.outbox().push(RequestIdr{});
}

void InvitedPlayer::receive(const net::kakusan::control::Ping& message) {
  conclude(channel_.answer_ping(message));
}

void InvitedPlayer::receive(const net::kakusan::control::Pong& message) {
  conclude(channel_.take_pong(message));
}

void InvitedPlayer::receive(const net::kakusan::control::Leave& message) {
  if (!channel_.authenticated()) return fail(net::kakusan::Error::protocol);
  terminate(Ending::ended_by_peer(message.reason));
}

void InvitedPlayer::receive(const net::kakusan::control::Failure& message) {
  terminate(channel_.peer_failed(message));
}

void InvitedPlayer::receive(const net::kakusan::control::PakeA&) {
  terminate(channel_.unexpected_pake());
}

void InvitedPlayer::receive(const net::kakusan::control::ConfirmA&) {
  terminate(channel_.unexpected_pake());
}

template <class Message>
void InvitedPlayer::receive(const Message&) {
  fail(net::kakusan::Error::protocol);
}

auto InvitedPlayer::rumble_changed() const noexcept -> bool {
  return rumble_low_ != sent_low_ || rumble_high_ != sent_high_;
}

auto InvitedPlayer::rumbling() const noexcept -> bool {
  return rumble_low_ != 0 || rumble_high_ != 0;
}

void InvitedPlayer::begin_pairing() {
  if (!channel_.codes_ready() ||
      !progress_.in<player_lifecycle::AwaitingCodes>())
    return;
  auto exchange = channel_.exchange();
  if (!exchange) return terminate(std::move(exchange).error());
  exchange_.emplace(std::move(*exchange));
  (void)progress_.process(player_lifecycle::OfferShare{exchange_->share()});
}

void InvitedPlayer::maybe_reserve() {
  if (channel_.state() != Progress::waiting_for_seat ||
      !progress_.in<player_lifecycle::Approved>() ||
      !progress_.in<player_lifecycle::Unseated>())
    return;
  (void)progress_.process(player_lifecycle::RequestSeat{});
}

void InvitedPlayer::activate() {
  active_token_ =
      channel_.send(net::kakusan::control::Active{*progress_.binding()});
  active_at_ = channel_.now();
  last_input_ = channel_.now();
  (void)progress_.process(player_lifecycle::Touch{});
  channel_.enter(Progress::playing);
}

void InvitedPlayer::start_media() {
  if (!progress_.process(player_lifecycle::Stream{})) return;
  feedback_pacer_.start(channel_.now());
  pump_feedback();
}

void InvitedPlayer::pump_feedback() {
  if (channel_.state() != Progress::playing ||
      !progress_.in<player_lifecycle::Streaming>())
    return;
  const auto now = channel_.now();
  const auto due = feedback_pacer_.next_send_us(rumble_changed(), rumbling());
  if (!due || now < *due) return;
  net::kakusan::FeedbackFrame frame;
  frame.binding = *progress_.binding();
  frame.sequence = feedback_sequence_++;
  frame.elapsed_ms = channel_.milliseconds_since(active_at_);
  frame.low_frequency = rumble_low_;
  frame.high_frequency = rumble_high_;
  sent_low_ = rumble_low_;
  sent_high_ = rumble_high_;
  feedback_pacer_.mark(now);
  channel_.outbox().push(SendFeedback{frame});
}

void InvitedPlayer::fail(net::kakusan::Error error) {
  terminate(channel_.failure(error));
}

void InvitedPlayer::end(net::kakusan::control::Reason reason) {
  terminate(Ending::announced(net::kakusan::control::End{reason}, reason));
}

void InvitedPlayer::revoke(net::kakusan::control::Reason reason) {
  if (channel_.finished()) return;
  if (progress_.granted())
    return terminate(Ending::announced(
        net::kakusan::control::SeatRevoked{*progress_.binding(), reason},
        reason));
  fail(reason == net::kakusan::control::Reason::capacity
           ? net::kakusan::Error::no_capacity
           : net::kakusan::Error::timeout);
}

void InvitedPlayer::conclude(std::optional<Ending> ending) {
  if (ending) terminate(std::move(*ending));
}

void InvitedPlayer::terminate(Ending ending) {
  if (channel_.finished()) return;
  if (channel_.state() == Progress::verifying &&
      progress_.in<player_lifecycle::ConfirmationSent>())
    ending.authentication_failure = true;
  (void)progress_.process(player_lifecycle::Vacate{});
  exchange_.reset();
  connectivity_deadline_.reset();
  admission_deadline_.reset();
  source_ready_deadline_.reset();
  ready_deadline_.reset();
  feedback_pacer_.stop();
  channel_.end(std::move(ending));
}

}  // namespace libchiaking::kakusan
