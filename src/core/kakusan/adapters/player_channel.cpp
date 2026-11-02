#include "src/core/kakusan/adapters/player_channel.hpp"

#include "src/core/net/kakusan/identity_proof.hpp"
#include "src/core/utils/memory/constant_time.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace libchiaking::kakusan {

auto Ending::quietly(net::kakusan::control::Reason reason) -> Ending {
  return Ending{std::nullopt, std::nullopt, reason, false, false};
}

auto Ending::refused(net::kakusan::Error error) -> Ending {
  return Ending{std::nullopt, error, std::nullopt, false, false};
}

auto Ending::announced(net::kakusan::control::Message notice,
                       net::kakusan::control::Reason reason) -> Ending {
  return Ending{std::move(notice), std::nullopt, reason, false, false};
}

auto Ending::failed(net::kakusan::Error error, bool announce) -> Ending {
  std::optional<net::kakusan::control::Message> notice;
  if (announce) notice = net::kakusan::control::Failure::reporting(error);
  return Ending{std::move(notice), error, std::nullopt, false, false};
}

auto Ending::pairing_failed() -> Ending {
  return Ending{
      net::kakusan::control::Failure{net::kakusan::Error::pairing_failed},
      net::kakusan::Error::pairing_failed, std::nullopt, false, true};
}

auto Ending::ended_by_peer(net::kakusan::control::Reason reason) -> Ending {
  return Ending{std::nullopt, std::nullopt, reason, true, false};
}

auto Ending::failed_by_peer(net::kakusan::Error error, bool guessed) -> Ending {
  return Ending{std::nullopt, error, std::nullopt, true, guessed};
}

template <class Effect>
PlayerChannel<Effect>::PlayerChannel(
    platform::CryptoProvider& crypto, platform::SecureRandomProvider& random,
    net::kakusan::Role role, std::optional<PairingContext> context) noexcept
    : crypto_(&crypto),
      random_(&random),
      role_(role),
      context_(std::move(context)) {}

template <class Effect>
void PlayerChannel<Effect>::advance(std::uint64_t now_us) noexcept {
  now_ = std::max(now_, now_us);
}

template <class Effect>
auto PlayerChannel<Effect>::authenticated() const noexcept -> bool {
  return state_ == Progress::waiting_for_seat ||
         state_ == Progress::getting_ready || state_ == Progress::playing;
}

template <class Effect>
auto PlayerChannel<Effect>::finished() const noexcept -> bool {
  return state_ == Progress::leaving || state_ == Progress::gone;
}

template <class Effect>
auto PlayerChannel<Effect>::next_deadline_us() const noexcept
    -> std::optional<std::uint64_t> {
  if (state_ == Progress::gone) return std::nullopt;
  if (state_ == Progress::leaving) return closing_deadline_;
  auto deadline = timing::Deadline::earliest(dtls_deadline_, hello_deadline_);
  deadline = timing::Deadline::earliest(deadline, pairing_deadline_);
  return timing::Deadline::earliest(deadline, heartbeat_.next_deadline_us());
}

template <class Effect>
auto PlayerChannel<Effect>::milliseconds_since(
    std::uint64_t since_us) const noexcept -> std::uint32_t {
  const timing::Deadline::Duration elapsed{
      timing::Deadline::elapsed_us(since_us, now_)};
  return static_cast<std::uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

template <class Effect>
auto PlayerChannel<Effect>::setup_hash(
    const net::kakusan::control::SourceSetup& setup) const
    -> net::kakusan::Result<net::kakusan::Digest256> {
  return net::kakusan::control::MessageCodec::setup_hash(*crypto_, setup);
}

template <class Effect>
void PlayerChannel<Effect>::adopt(const PairingContext& context) {
  context_ = context;
}

template <class Effect>
void PlayerChannel<Effect>::enter(Progress state) {
  state_ = state;
  show();
}

template <class Effect>
auto PlayerChannel<Effect>::send(net::kakusan::control::Message message)
    -> net::kakusan::AckToken {
  return lifecycle_.outbox().send(std::move(message));
}

template <class Effect>
void PlayerChannel<Effect>::await_dtls() noexcept {
  dtls_deadline_ = timing::Deadline::after_us(now_, dtls_timeout_us);
}

template <class Effect>
void PlayerChannel<Effect>::establish(DtlsEstablished& event) {
  (void)lifecycle_.open();
  dtls_deadline_.reset();
  const auto transcript =
      std::ranges::copy(context_->transcript, associated_.begin());
  std::ranges::copy(event.exporter, transcript.out);
  hello_deadline_ = timing::Deadline::after_us(now_, hello_timeout_us);
  proof_ = std::move(event.proof);
  (void)send(net::kakusan::control::Hello{context_->transcript});
}

template <class Effect>
auto PlayerChannel<Effect>::greet(const net::kakusan::control::Hello& message)
    -> std::optional<Ending> {
  if (state_ != Progress::connecting ||
      introduction_ != Introduction::awaiting_hello || !proof_)
    return failure(net::kakusan::Error::protocol);
  if (!memory::constant_time_equal(message.transcript, context_->transcript))
    return failure(net::kakusan::Error::transcript);
  introduction_ = Introduction::awaiting_identity;
  (void)send(*proof_);
  proof_.reset();
  return std::nullopt;
}

template <class Effect>
auto PlayerChannel<Effect>::identify(
    const net::kakusan::control::Identity& message) -> std::optional<Ending> {
  if (state_ != Progress::connecting ||
      introduction_ != Introduction::awaiting_identity)
    return failure(net::kakusan::Error::protocol);
  const auto exporter = std::span<const std::byte>{associated_}.last(
      net::kakusan::pairing_exporter_size);
  const auto proven = net::kakusan::IdentityProof::verify(
      *crypto_, peer_role(), context_->transcript, exporter, message,
      context_->peer_hint);
  if (!proven) return failure(proven.error());
  auto& peer = role_ == net::kakusan::Role::host
                   ? context_->parties.guest_identity
                   : context_->parties.host_identity;
  peer = message.key;
  introduction_ = Introduction::complete;
  lifecycle_.outbox().push(PeerIdentified{message.key, message.hosting});
  hello_deadline_.reset();
  pairing_deadline_ = timing::Deadline::after_us(now_, pairing_timeout_us);
  enter(Progress::verifying);
  return std::nullopt;
}

template <class Effect>
auto PlayerChannel<Effect>::show_code(LocalCode& event) -> bool {
  if (state_ != Progress::verifying || local_code_) return false;
  local_code_ = std::move(event.code);
  lifecycle_.outbox().push(DisplayCode{local_code_->clone()});
  return true;
}

template <class Effect>
auto PlayerChannel<Effect>::take_code(PeerCodeEntered& event) -> bool {
  if (state_ != Progress::verifying || peer_code_) return false;
  peer_code_ = std::move(event.code);
  return true;
}

template <class Effect>
auto PlayerChannel<Effect>::codes_ready() const noexcept -> bool {
  return local_code_ && peer_code_;
}

template <class Effect>
auto PlayerChannel<Effect>::exchange()
    -> Expected<net::kakusan::Exchange, Ending> {
  const bool hosting = role_ == net::kakusan::Role::host;
  auto password = net::kakusan::Exchange::password_scalar(
      *crypto_, hosting ? *local_code_ : *peer_code_,
      hosting ? *peer_code_ : *local_code_, context_->invitation_id);
  local_code_.reset();
  peer_code_.reset();
  if (!password) return unexpected(failure(password.error()));
  auto ephemeral = net::kakusan::Exchange::ephemeral(*random_);
  if (!ephemeral) return unexpected(failure(net::kakusan::Error::internal));
  auto begun = net::kakusan::Exchange::begin(
      *crypto_, role_, std::move(password).value(), std::move(*ephemeral),
      context_->parties, associated_);
  if (!begun) return unexpected(failure(begun.error()));
  return std::move(begun).value();
}

template <class Effect>
auto PlayerChannel<Effect>::confirm(net::kakusan::Exchange& exchange,
                                    std::span<const std::byte> peer_share)
    -> std::optional<Ending> {
  auto confirmations = exchange.complete(*crypto_, peer_share);
  if (!confirmations)
    return confirmations.error() == net::kakusan::Error::pairing_failed
               ? Ending::pairing_failed()
               : failure(confirmations.error());
  confirmations_ = std::move(confirmations).value();
  confirmations_->shared_key.clear();
  return std::nullopt;
}

template <class Effect>
auto PlayerChannel<Effect>::peer_confirmed(
    const net::kakusan::Digest512& mac) const noexcept -> bool {
  const auto& expected = role_ == net::kakusan::Role::host
                             ? confirmations_->guest
                             : confirmations_->host;
  return memory::constant_time_equal(mac, expected);
}

template <class Effect>
void PlayerChannel<Effect>::send_confirmation() {
  if (role_ == net::kakusan::Role::host) {
    net::kakusan::control::ConfirmA confirm;
    std::ranges::copy(confirmations_->host, confirm.mac.begin());
    (void)send(confirm);
    return;
  }
  net::kakusan::control::ConfirmB confirm;
  std::ranges::copy(confirmations_->guest, confirm.mac.begin());
  (void)send(confirm);
}

template <class Effect>
void PlayerChannel<Effect>::drop_confirmations() noexcept {
  confirmations_.reset();
}

template <class Effect>
auto PlayerChannel<Effect>::check_authenticated(
    const net::kakusan::control::Authenticated& message) const
    -> std::optional<Ending> {
  if (!memory::constant_time_equal(message.transcript, context_->transcript))
    return failure(net::kakusan::Error::transcript);
  return std::nullopt;
}

template <class Effect>
void PlayerChannel<Effect>::send_authenticated() {
  (void)send(net::kakusan::control::Authenticated{context_->transcript});
}

template <class Effect>
void PlayerChannel<Effect>::admit() {
  pairing_deadline_.reset();
  associated_ = {};
  enter(Progress::waiting_for_seat);
  lifecycle_.spend();
  heartbeat_.start(now_);
}

template <class Effect>
void PlayerChannel<Effect>::refresh() noexcept {
  heartbeat_.refresh(now_);
}

template <class Effect>
auto PlayerChannel<Effect>::answer_ping(
    const net::kakusan::control::Ping& message) -> std::optional<Ending> {
  if (!authenticated()) return failure(net::kakusan::Error::protocol);
  refresh();
  (void)send(net::kakusan::control::Pong{message.nonce});
  return std::nullopt;
}

template <class Effect>
auto PlayerChannel<Effect>::take_pong(
    const net::kakusan::control::Pong& message) -> std::optional<Ending> {
  if (!authenticated()) return failure(net::kakusan::Error::protocol);
  if (heartbeat_.answer(message.nonce)) refresh();
  return std::nullopt;
}

template <class Effect>
auto PlayerChannel<Effect>::keep_alive() -> std::optional<Ending> {
  if (!heartbeat_.ping_due(now_)) return std::nullopt;
  net::kakusan::control::Nonce nonce{};
  if (!random_->fill_bytes(nonce))
    return Ending::refused(net::kakusan::Error::crypto_unavailable);
  heartbeat_.issue(nonce, now_);
  (void)send(net::kakusan::control::Ping{nonce});
  return std::nullopt;
}

template <class Effect>
auto PlayerChannel<Effect>::silent() const noexcept -> bool {
  return heartbeat_.silent(now_);
}

template <class Effect>
auto PlayerChannel<Effect>::timed_out() const noexcept -> bool {
  return timing::Deadline::expired(dtls_deadline_, now_) ||
         timing::Deadline::expired(hello_deadline_, now_) ||
         timing::Deadline::expired(pairing_deadline_, now_);
}

template <class Effect>
auto PlayerChannel<Effect>::failure(net::kakusan::Error error) const -> Ending {
  return Ending::failed(error, open());
}

template <class Effect>
auto PlayerChannel<Effect>::peer_failed(
    const net::kakusan::control::Failure& message) const -> Ending {
  const bool guessed = state_ == Progress::verifying &&
                       message.code == net::kakusan::Error::pairing_failed;
  return Ending::failed_by_peer(message.code, guessed);
}

template <class Effect>
auto PlayerChannel<Effect>::unexpected_pake() const -> Ending {
  if (authenticated()) return failure(net::kakusan::Error::protocol);
  return Ending::pairing_failed();
}

template <class Effect>
auto PlayerChannel<Effect>::transport_failed() -> std::optional<Ending> {
  if (state_ == Progress::leaving) {
    finish();
    return std::nullopt;
  }
  if (finished()) return std::nullopt;
  if (open()) return Ending::quietly(net::kakusan::control::Reason::network);
  return Ending::refused(net::kakusan::Error::no_path);
}

template <class Effect>
auto PlayerChannel<Effect>::acknowledged(net::kakusan::AckToken token) -> bool {
  if (token == 0) return true;
  if (state_ == Progress::leaving) {
    if (token == terminal_token_) finish();
    return true;
  }
  return state_ == Progress::gone;
}

template <class Effect>
auto PlayerChannel<Effect>::closing() -> bool {
  if (state_ == Progress::leaving) {
    if (timing::Deadline::expired(closing_deadline_, now_)) finish();
    return true;
  }
  return state_ == Progress::gone;
}

template <class Effect>
void PlayerChannel<Effect>::end(Ending ending) {
  forget_secrets();
  lifecycle_.outbox().push(EraseSecrets{});
  lifecycle_.spend();
  if (ending.authentication_failure)
    lifecycle_.outbox().push(RecordAuthenticationFailure{});
  stop_timers();
  shown_.error = ending.error;
  shown_.reason = ending.reason;
  shown_.by_peer = ending.by_peer;
  if (!open() || (!ending.notice && !ending.by_peer)) return finish();
  closing_deadline_ = timing::Deadline::after_us(now_, terminal_grace_us);
  enter(Progress::leaving);
  if (ending.notice)
    terminal_token_ = lifecycle_.outbox().send_last(std::move(*ending.notice));
}

template <class Effect>
auto PlayerChannel<Effect>::peer_role() const noexcept -> net::kakusan::Role {
  return role_ == net::kakusan::Role::host ? net::kakusan::Role::guest
                                           : net::kakusan::Role::host;
}

template <class Effect>
void PlayerChannel<Effect>::show() {
  shown_.state = state_;
  lifecycle_.outbox().push(shown_);
}

template <class Effect>
void PlayerChannel<Effect>::finish() {
  stop_timers();
  enter(Progress::gone);
  lifecycle_.outbox().push(CloseTransport{});
}

template <class Effect>
void PlayerChannel<Effect>::stop_timers() noexcept {
  dtls_deadline_.reset();
  hello_deadline_.reset();
  pairing_deadline_.reset();
  closing_deadline_.reset();
  heartbeat_.stop();
}

template <class Effect>
void PlayerChannel<Effect>::forget_secrets() noexcept {
  local_code_.reset();
  peer_code_.reset();
  confirmations_.reset();
  associated_ = {};
}

template class PlayerChannel<VisitingEffect>;
template class PlayerChannel<InvitedEffect>;

}  // namespace libchiaking::kakusan
