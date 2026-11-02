#include "src/core/kakusan/adapters/shared_session/shared_session_core.hpp"

#include "src/core/kakusan/adapters/shared_session/invited_player.hpp"
#include "src/core/kakusan/adapters/shared_session/packet_verdict.hpp"
#include "src/core/kakusan/adapters/shared_setup.hpp"
#include "src/core/net/kakusan/identity_proof.hpp"
#include "src/core/net/kakusan/media_sender.hpp"
#include "src/core/net/kakusan/short_block.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace libchiaking::kakusan {

static_assert(std::is_same_v<GuestId::Bytes, net::kakusan::InvitationId>);
static_assert(std::is_same_v<IdentityKey::Bytes, net::kakusan::IdentityKey>);
static_assert(std::is_same_v<Fingerprint::Bytes, net::kakusan::Digest256>);
static_assert(std::is_same_v<IdentityHint::Bytes, net::kakusan::IdentityHint>);
static_assert(VerificationCode::size == net::kakusan::code_size);
static_assert(Invitation::most_text ==
              net::kakusan::ShortBlockCodec::max_text_size);
static_assert(all_buttons == net::kakusan::button_mask);
static_assert(required_buttons == net::kakusan::required_buttons);
static_assert(SharingRules::most_seats == controllers::SeatNumber::count);

struct SharedSession::Core::Spoke {
  Spoke(platform::CryptoProvider& crypto,
        platform::SecureRandomProvider& random,
        const net::kakusan::InvitationId& id,
        const net::kakusan::BlockSeed& block_seed,
        std::unique_ptr<net::kakusan::Link> transport_link,
        std::uint64_t now_us) noexcept
      : guest(id),
        seed(block_seed),
        link(std::move(transport_link)),
        machine(crypto, random),
        created_us(now_us) {}

  net::kakusan::InvitationId guest{};
  net::kakusan::BlockSeed seed{};
  std::unique_ptr<net::kakusan::Link> link;
  InvitedPlayer machine;
  EffectLoop<InvitedPlayer> loop;
  std::uint64_t created_us{};
  std::optional<std::uint64_t> issued_us;
  std::uint64_t expires_at{};
  std::optional<net::kakusan::Transcript> transcript;
  std::optional<net::kakusan::SeatBinding> joining;
  bool join_resolved{};
  std::optional<controllers::SeatNumber> console_seat;
  std::optional<std::uint64_t> setup_generation;
  std::unique_ptr<net::kakusan::MediaSender> sender;
  bool media_reported{};
  bool code_drawn{};
};

SharedSession::Core::Core(Dependencies dependencies, DeviceIdentity identity,
                          const SharingRules& rules, SeatTable seats,
                          const net::kakusan::SessionId& session_id)
    : crypto_(dependencies.crypto),
      random_(dependencies.random),
      transports_(dependencies.transports),
      identity_(std::move(identity)),
      identity_hint_(
          net::kakusan::ShortBlockCodec::hint_of(identity_.key().bytes())),
      allowed_buttons_(rules.allowed_buttons()),
      seats_(std::move(seats)),
      session_id_(session_id),
      capture_(dependencies.threading),
      reported_seats_(seats_.summary()) {}

SharedSession::Core::~Core() {
  for (auto& spoke : spokes_) spoke->link->close();
}

auto SharedSession::Core::create(Dependencies dependencies,
                                 DeviceIdentity identity,
                                 const SharingRules& rules)
    -> net::kakusan::Result<std::unique_ptr<Core>> {
  auto seats = SeatTable::create(rules.capacity(), rules.local_seats());
  if (!seats) return net::kakusan::fail(seats.error());
  net::kakusan::SessionId session_id{};
  if (!dependencies.random.fill_bytes(session_id))
    return net::kakusan::fail(net::kakusan::Error::crypto_unavailable);
  return std::unique_ptr<Core>{new Core{dependencies, std::move(identity),
                                        rules, std::move(seats).value(),
                                        session_id}};
}

void SharedSession::Core::source_keys(std::uint32_t protocol_version,
                                      play::ReceiveKeys keys) noexcept {
  capture_.keys(protocol_version, std::move(keys));
}

void SharedSession::Core::source_datagram(
    const play::SharedDatagram& datagram) noexcept {
  capture_.datagram(datagram);
}

auto SharedSession::Core::source_started(
    const media::StreamDescription& description, std::uint32_t video_fps,
    std::uint64_t now_us) -> net::kakusan::Result<void> {
  advance(now_us);
  const auto keys = capture_.clone_keys();
  if (!keys) return net::kakusan::fail(net::kakusan::Error::source_format);
  SourceState next{
      description, video_fps, keys->version(), keys->generation(), {}};
  if (!random_.fill_bytes(next.source_session_id))
    return net::kakusan::fail(net::kakusan::Error::crypto_unavailable);
  const auto probe =
      SharedSetup::build(*keys, description, video_fps, next.session_id());
  if (!probe) return net::kakusan::fail(probe.error());
  if (source_ && source_->generation == next.generation &&
      source_->video_fps == next.video_fps &&
      source_->description.audio_header == description.audio_header &&
      source_->description.profiles == description.profiles)
    return {};
  end_source_spokes(source_ &&
                    source_->protocol_version != next.protocol_version);
  source_ = std::move(next);
  settle();
  return {};
}

void SharedSession::Core::source_ended(std::uint64_t now_us) {
  advance(now_us);
  for (auto& spoke : spokes_) feed(*spoke, SourceEnded{now_});
  capture_.reset();
  forwarding_ = false;
  source_.reset();
  settle();
}

auto SharedSession::Core::source_joined(const net::kakusan::SeatBinding& seat,
                                        controllers::SeatNumber console_seat,
                                        std::uint64_t now_us)
    -> net::kakusan::Result<void> {
  advance(now_us);
  auto* spoke = find_joining(seat);
  if (spoke == nullptr) {
    const bool abandoned = resolve_abandoned(seat, console_seat);
    settle();
    return net::kakusan::fail(abandoned
                                  ? net::kakusan::Error::source_join_failed
                                  : net::kakusan::Error::internal);
  }
  spoke->join_resolved = true;
  if (find_console_seat(console_seat) != nullptr) {
    feed(*spoke, SourceJoinFailed{now_});
    settle();
    return net::kakusan::fail(net::kakusan::Error::source_join_failed);
  }
  spoke->console_seat = console_seat;

  const auto keys = capture_.clone_keys();
  if (!source_ || !keys || keys->generation() != source_->generation) {
    feed(*spoke, LocalFailure{now_, net::kakusan::Error::source_ended});
    settle();
    return net::kakusan::fail(net::kakusan::Error::source_ended);
  }
  auto setup = SharedSetup::build(*keys, source_->description,
                                  source_->video_fps, source_->session_id());
  if (!setup) {
    feed(*spoke, LocalFailure{now_, setup.error()});
    settle();
    return net::kakusan::fail(setup.error());
  }
  spoke->setup_generation = source_->generation;
  feed(*spoke, SourceJoined{now_, std::move(setup).value(), seats_.capacity(),
                            seats_.local_seats(), allowed_buttons_});
  settle();
  return {};
}

auto SharedSession::Core::source_join_failed(
    const net::kakusan::SeatBinding& seat, std::uint64_t now_us)
    -> net::kakusan::Result<void> {
  advance(now_us);
  auto* spoke = find_joining(seat);
  if (spoke == nullptr)
    return resolve_abandoned(seat, std::nullopt)
               ? net::kakusan::Result<void>{}
               : net::kakusan::fail(net::kakusan::Error::internal);
  spoke->join_resolved = true;
  feed(*spoke, SourceJoinFailed{now_});
  settle();
  return {};
}

void SharedSession::Core::source_rumble(const SourceRumble& rumble,
                                        std::uint64_t now_us) {
  advance(now_us);
  auto* spoke = find_console_seat(rumble.seat);
  if (spoke == nullptr) return;
  feed(*spoke,
       RumbleSampled{now_, rumble.low_frequency, rumble.high_frequency});
  settle();
}

void SharedSession::Core::source_unseated(controllers::SeatNumber console_seat,
                                          std::uint64_t now_us) {
  advance(now_us);
  auto* spoke = find_console_seat(console_seat);
  if (spoke == nullptr) return;
  feed(*spoke, SourceEnded{now_});
  settle();
}

auto SharedSession::Core::invite(Clock clock)
    -> net::kakusan::Result<net::kakusan::InvitationId> {
  advance(clock);
  if (!source_) return net::kakusan::fail(net::kakusan::Error::source_ended);
  book_.expire(now_);
  const auto gathering = static_cast<std::size_t>(std::ranges::count_if(
      spokes_, [](const auto& spoke) { return !spoke->issued_us; }));
  if (book_.tracked_count() + gathering >= max_tracked_invitations)
    return net::kakusan::fail(net::kakusan::Error::no_capacity);

  net::kakusan::BlockSeed seed{};
  net::kakusan::InvitationId guest{};
  bool drawn = false;
  for (std::size_t attempt = 0; attempt < id_draw_attempts && !drawn;
       ++attempt) {
    if (!random_.fill_bytes(seed))
      return net::kakusan::fail(net::kakusan::Error::crypto_unavailable);
    const auto named =
        net::kakusan::ShortBlockCodec::invitation_id(crypto_, seed);
    if (!named) return net::kakusan::fail(named.error());
    guest = named.value();
    drawn = guest != net::kakusan::InvitationId{} && find(guest) == nullptr &&
            book_.find(guest) == nullptr;
  }
  if (!drawn) return net::kakusan::fail(net::kakusan::Error::internal);

  auto created = transports_.create(net::kakusan::TransportRole::host);
  if (!created) return net::kakusan::fail(created.error());
  auto link = std::make_unique<net::kakusan::Link>(
      std::move(created).value(), net::kakusan::TransportRole::host);
  auto credentials = net::kakusan::ShortBlockCodec::credentials(
      crypto_, seed, net::kakusan::BlockKind::invitation);
  if (!credentials) return net::kakusan::fail(credentials.error());
  if (const auto gathered = link->gather(now_, std::move(credentials).value());
      !gathered)
    return net::kakusan::fail(gathered.error());
  spokes_.push_back(std::make_unique<Spoke>(crypto_, random_, guest, seed,
                                            std::move(link), now_));
  return guest;
}

auto SharedSession::Core::import_reply(std::string_view block, Clock clock)
    -> net::kakusan::Result<net::kakusan::InvitationId> {
  advance(clock);
  const auto reply = net::kakusan::ShortBlockCodec::parse_reply(crypto_, block);
  if (!reply) return net::kakusan::fail(reply.error());
  const auto& answered = reply.value().body;
  const auto answering = book_.answered_by(reply.value());
  auto* spoke = answering ? find(*answering) : nullptr;
  if (spoke == nullptr)
    return net::kakusan::fail(net::kakusan::Error::invalid_block);
  const auto guest = spoke->guest;
  const auto imported = book_.import_reply(reply.value(), wall_, now_);
  if (!imported) return net::kakusan::fail(imported.error());
  if (imported.value() == ImportOutcome::already_bound) return guest;

  const auto* invitation = book_.find(guest);
  const auto transcript =
      invitation == nullptr
          ? net::kakusan::Result<net::kakusan::Transcript>{net::kakusan::fail(
                net::kakusan::Error::internal)}
          : net::kakusan::Exchange::transcript(crypto_, invitation->hash,
                                               reply.value().hash);
  if (!transcript) {
    feed(*spoke, LocalFailure{now_, transcript.error()});
    settle();
    return net::kakusan::fail(transcript.error());
  }
  spoke->transcript = transcript.value();

  PairingContext context;
  context.transcript = transcript.value();
  context.parties.host_identity = identity_.key().bytes();
  context.invitation_id = guest;
  context.peer_hint = answered.identity_hint;
  feed(*spoke, ReplyImported{now_, context});
  const auto connected = spoke->link->connect(
      net::kakusan::TransportDescription{answered.ice.clone(),
                                         answered.dtls_sha256},
      transcript.value(), now_);
  if (!connected)
    feed(*spoke, LocalFailure{now_, net::kakusan::Error::no_path});
  settle();
  return guest;
}

auto SharedSession::Core::enter_code(const net::kakusan::InvitationId& guest,
                                     std::string_view code,
                                     std::uint64_t now_us)
    -> net::kakusan::Result<void> {
  advance(now_us);
  auto* spoke = find(guest);
  if (spoke == nullptr || spoke->machine.state() != Progress::verifying)
    return net::kakusan::fail(net::kakusan::Error::internal);
  if (!book_.pairing_allowed(now_))
    return net::kakusan::fail(net::kakusan::Error::pairing_failed);
  auto parsed = net::kakusan::Code::parse(code);
  if (!parsed) return net::kakusan::fail(parsed.error());
  feed(*spoke, PeerCodeEntered{now_, std::move(parsed).value()});
  settle();
  return {};
}

auto SharedSession::Core::approve(const net::kakusan::InvitationId& guest,
                                  std::uint64_t now_us)
    -> net::kakusan::Result<void> {
  advance(now_us);
  auto* spoke = find(guest);
  if (spoke == nullptr)
    return net::kakusan::fail(net::kakusan::Error::internal);
  feed(*spoke, AdmissionApproved{now_});
  settle();
  return {};
}

auto SharedSession::Core::remove(const net::kakusan::InvitationId& guest,
                                 std::uint64_t now_us)
    -> net::kakusan::Result<void> {
  advance(now_us);
  auto* spoke = find(guest);
  if (spoke == nullptr)
    return net::kakusan::fail(net::kakusan::Error::internal);
  feed(*spoke, Cancel{now_, net::kakusan::control::Reason::user});
  settle();
  return {};
}

auto SharedSession::Core::set_local_seats(std::uint8_t local_seats,
                                          std::uint64_t now_us)
    -> net::kakusan::Result<void> {
  advance(now_us);
  return revoke_seats(seats_.set_local_seats(local_seats));
}

auto SharedSession::Core::set_capacity(std::uint8_t capacity,
                                       std::uint64_t now_us)
    -> net::kakusan::Result<void> {
  advance(now_us);
  return revoke_seats(seats_.set_capacity(capacity));
}

auto SharedSession::Core::revoke_seats(
    net::kakusan::Result<std::vector<net::kakusan::SeatBinding>> revoked)
    -> net::kakusan::Result<void> {
  if (!revoked) return net::kakusan::fail(revoked.error());
  for (const auto& binding : revoked.value()) {
    if (auto* spoke = find_binding(binding))
      feed(*spoke, CapacityRevoked{now_});
  }
  settle();
  return {};
}

void SharedSession::Core::pump(Clock clock) {
  advance(clock);
  for (std::size_t index = 0; index < spokes_.size(); ++index) {
    auto& spoke = *spokes_[index];
    auto events = spoke.link->pump(now_);
    deliver(spoke, events);
  }
  for (std::size_t index = 0; index < spokes_.size(); ++index) {
    auto& spoke = *spokes_[index];
    spoke.loop.run(spoke.machine, spoke.machine.on_time(now_),
                   [this, &spoke](InvitedPlayer::Effect& effect) {
                     apply(spoke, effect);
                   });
  }
  expire_invitations();
  forward_capture();
  poll_senders();
  settle();
}

auto SharedSession::Core::next_deadline_us() const noexcept
    -> std::optional<std::uint64_t> {
  std::optional<std::uint64_t> deadline;
  for (const auto& spoke : spokes_) {
    deadline =
        timing::Deadline::earliest(deadline, spoke->link->next_deadline_us());
    deadline =
        timing::Deadline::earliest(deadline, spoke->machine.next_deadline_us());
    if (spoke->sender)
      deadline = timing::Deadline::earliest(deadline,
                                            spoke->sender->next_deadline_us());
    if (spoke->machine.state() != Progress::waiting) continue;
    deadline = timing::Deadline::earliest(
        deadline, spoke->issued_us
                      ? *spoke->issued_us + invitation_lifetime_us
                      : spoke->created_us + net::kakusan::gathering_timeout_us);
  }
  if (idr_pending_ && last_idr_us_)
    deadline = timing::Deadline::earliest(
        deadline, *last_idr_us_ + tuning_.keyframe_interval_us);
  return deadline;
}

auto SharedSession::Core::take_output() -> std::optional<Output> {
  if (outputs_.empty()) return std::nullopt;
  auto output = std::move(outputs_.front());
  outputs_.pop_front();
  return output;
}

auto SharedSession::Core::seats() const noexcept -> SeatSummary {
  return seats_.summary();
}

void SharedSession::Core::advance(std::uint64_t now_us) noexcept {
  now_ = std::max(now_, now_us);
}

void SharedSession::Core::advance(Clock clock) noexcept {
  advance(clock.now_us);
  wall_ = clock.wall_seconds;
}

template <class Matches>
auto SharedSession::Core::find_if(Matches matches) noexcept -> Spoke* {
  const auto found = std::ranges::find_if(
      spokes_, [&matches](const std::unique_ptr<Spoke>& spoke) {
        return matches(*spoke);
      });
  return found == spokes_.end() ? nullptr : found->get();
}

auto SharedSession::Core::find(const net::kakusan::InvitationId& guest) noexcept
    -> Spoke* {
  return find_if([&guest](const Spoke& spoke) { return spoke.guest == guest; });
}

auto SharedSession::Core::find_joining(
    const net::kakusan::SeatBinding& seat) noexcept -> Spoke* {
  return find_if([&seat](const Spoke& spoke) {
    return spoke.joining == seat && !spoke.join_resolved;
  });
}

auto SharedSession::Core::find_console_seat(
    controllers::SeatNumber seat) noexcept -> Spoke* {
  return find_if(
      [seat](const Spoke& spoke) { return spoke.console_seat == seat; });
}

auto SharedSession::Core::find_binding(
    const net::kakusan::SeatBinding& seat) noexcept -> Spoke* {
  return find_if(
      [&seat](const Spoke& spoke) { return spoke.machine.binding() == seat; });
}

void SharedSession::Core::feed(Spoke& spoke, InvitedPlayer::Event event) {
  spoke.loop.feed(
      spoke.machine, std::move(event),
      [this, &spoke](InvitedPlayer::Effect& effect) { apply(spoke, effect); });
}

void SharedSession::Core::apply(Spoke& spoke, InvitedPlayer::Effect& effect) {
  if (auto* send = std::get_if<SendControl>(&effect)) {
    const auto sent = spoke.link->send_control(send->message, send->token,
                                               send->terminal, now_);
    if (!sent && !send->terminal) feed(spoke, LocalFailure{now_, sent.error()});
  } else if (const auto* shown = std::get_if<ShowState>(&effect)) {
    show(spoke, *shown);
  } else if (const auto* peer = std::get_if<PeerIdentified>(&effect)) {
    identify(spoke, *peer);
  } else if (auto* display = std::get_if<DisplayCode>(&effect)) {
    emit(CodeReady{spoke.guest, std::move(display->code)});
  } else if (std::holds_alternative<EraseSecrets>(effect)) {
    spoke.sender.reset();
    update_forwarding();
  } else if (std::holds_alternative<CloseTransport>(effect)) {
    stop_media(spoke);
    spoke.link->close();
  } else if (std::holds_alternative<RecordAuthenticationFailure>(effect)) {
    book_.record_authentication_failure(now_);
  } else if (std::holds_alternative<ConsumeInvitation>(effect)) {
    consume_invitation(spoke.guest);
  } else {
    apply_admission(spoke, effect);
  }
}

void SharedSession::Core::apply_admission(Spoke& spoke,
                                          InvitedPlayer::Effect& effect) {
  if (std::holds_alternative<ReserveSeat>(effect)) {
    const auto reserved = seats_.reserve();
    if (reserved)
      feed(spoke, SeatReserved{now_, reserved.value()});
    else
      feed(spoke, NoCapacity{now_});
  } else if (const auto* grant = std::get_if<GrantSeat>(&effect)) {
    (void)seats_.grant(grant->binding);
  } else if (const auto* release = std::get_if<ReleaseSeat>(&effect)) {
    (void)seats_.release(release->binding);
  } else if (const auto* join = std::get_if<RequestSourceJoin>(&effect)) {
    spoke.joining = join->binding;
    spoke.join_resolved = false;
    emit(JoinRequested{spoke.guest, join->binding});
  } else if (const auto* removal = std::get_if<RequestSourceRemoval>(&effect)) {
    remove_source(spoke, removal->binding);
  } else {
    apply_gameplay(spoke, effect);
  }
}

void SharedSession::Core::apply_gameplay(Spoke& spoke,
                                         InvitedPlayer::Effect& effect) {
  if (std::holds_alternative<StartMedia>(effect)) {
    start_media(spoke);
  } else if (std::holds_alternative<StopMedia>(effect)) {
    stop_media(spoke);
  } else if (const auto* input = std::get_if<ApplyInput>(&effect)) {
    if (!spoke.console_seat) return;
    const auto state =
        ControllerMap::to_source_state(input->state, allowed_buttons_);
    if (state) emit(ControllerInput{spoke.guest, state.value()});
  } else if (std::holds_alternative<InjectNeutral>(effect)) {
    if (spoke.console_seat)
      emit(ControllerInput{spoke.guest, controllers::ControllerState{}});
  } else if (const auto* feedback = std::get_if<SendFeedback>(&effect)) {
    (void)spoke.link->send_feedback(feedback->frame);
  } else if (std::holds_alternative<RequestIdr>(effect)) {
    request_idr();
  }
}

void SharedSession::Core::identify(Spoke& spoke, const PeerIdentified& peer) {
  const auto print = net::kakusan::Hash::sha256(crypto_, peer.key);
  if (!print) return feed(spoke, LocalFailure{now_, print.error()});
  emit(GuestIdentified{spoke.guest, peer.key, print.value()});
}

void SharedSession::Core::establish(Spoke& spoke,
                                    net::kakusan::Exporter exporter) {
  if (!spoke.transcript)
    return feed(spoke, LocalFailure{now_, net::kakusan::Error::internal});
  const auto seats = seats_.summary();
  auto proof = net::kakusan::IdentityProof::sign(
      crypto_, net::kakusan::Role::host, *spoke.transcript, exporter,
      identity_.seed(),
      net::kakusan::IdentityProof::Hosting{session_id_, seats.local_seats,
                                           seats.remote_seats,
                                           seats.reserved_seats});
  if (!proof) return feed(spoke, LocalFailure{now_, proof.error()});
  feed(spoke,
       DtlsEstablished{now_, std::move(exporter), std::move(proof).value()});
}

void SharedSession::Core::show(Spoke& spoke, const ShowState& shown) {
  emit(GuestState{spoke.guest, shown.state, shown.error, shown.reason,
                  shown.by_peer, spoke.machine.binding()});
  if (shown.state != Progress::verifying || spoke.code_drawn) return;
  spoke.code_drawn = true;
  auto value = net::kakusan::Code::draw(random_);
  if (!value)
    return feed(spoke, LocalFailure{now_, net::kakusan::Error::internal});
  feed(spoke, LocalCode{now_, std::move(*value)});
}

void SharedSession::Core::deliver(Spoke& spoke,
                                  std::span<net::kakusan::LinkEvent> events) {
  namespace link_event = net::kakusan::link_event;
  for (auto& event : events) {
    if (const auto* gathered =
            std::get_if<net::kakusan::link_event::Gathered>(&event)) {
      if (spoke.machine.state() == Progress::waiting && !spoke.issued_us)
        issue_invitation(spoke, gathered->local);
    } else if (std::holds_alternative<net::kakusan::link_event::Nominated>(
                   event)) {
      feed(spoke, IceNominated{now_});
    } else if (auto* established =
                   std::get_if<net::kakusan::link_event::Established>(&event)) {
      establish(spoke, std::move(established->exporter));
    } else if (auto* message =
                   std::get_if<net::kakusan::link_event::Control>(&event)) {
      feed(spoke, ControlReceived{now_, std::move(message->message)});
    } else if (const auto* acked =
                   std::get_if<net::kakusan::link_event::Acked>(&event)) {
      feed(spoke, ControlAcked{now_, acked->token});
    } else if (const auto* input =
                   std::get_if<net::kakusan::link_event::Input>(&event)) {
      feed(spoke, InputReceived{now_, input->frame});
    } else if (const auto* ack =
                   std::get_if<net::kakusan::link_event::MediaAck>(&event)) {
      acknowledge_media(spoke, ack->record);
    } else if (const auto* malformed =
                   std::get_if<net::kakusan::link_event::Malformed>(&event)) {
      feed(spoke, MalformedData{now_, malformed->error});
    } else if (std::holds_alternative<net::kakusan::link_event::Failed>(
                   event)) {
      feed(spoke, TransportFailed{now_});
    }
  }
}

void SharedSession::Core::acknowledge_media(Spoke& spoke,
                                            std::span<const std::byte> record) {
  if (!spoke.sender) return;
  const auto acked = spoke.sender->on_ack(record, now_);
  if (acked) return;
  if (const auto failure = spoke.sender->failure())
    return feed(spoke, LocalFailure{now_, *failure});
  feed(spoke, MalformedData{now_, acked.error()});
}

void SharedSession::Core::issue_invitation(
    Spoke& spoke, const net::kakusan::TransportDescription& local) {
  if (!source_)
    return feed(spoke, LocalFailure{now_, net::kakusan::Error::source_ended});
  const auto seats = seats_.summary();
  const net::kakusan::ShortInvitation invitation{
      .seed = spoke.seed,
      .invite_expires_at = wall_ + net::kakusan::invitation_lifetime_seconds,
      .source_protocol = static_cast<std::uint8_t>(source_->protocol_version),
      .capacity = seats.capacity,
      .dtls_sha256 = local.certificate_digest,
      .identity_hint = identity_hint_,
      .ice = local.ice.clone(),
      .invitation_id = {}};
  const auto sealed = net::kakusan::ShortBlockCodec::seal(crypto_, invitation);
  if (!sealed) return feed(spoke, LocalFailure{now_, sealed.error()});
  if (const auto added = book_.add(sealed.value(), now_); !added)
    return feed(spoke, LocalFailure{now_, added.error()});
  spoke.issued_us = now_;
  spoke.expires_at = invitation.invite_expires_at;
  emit(InvitationReady{spoke.guest, sealed.value().text.clone(),
                       spoke.expires_at, IdentityHint{identity_hint_}, seats,
                       invitation.source_protocol});
}

void SharedSession::Core::consume_invitation(
    const net::kakusan::InvitationId& guest) {
  const auto status = book_.status(guest);
  if (status == InvitationStatus::bound)
    (void)book_.finish(guest);
  else if (status == InvitationStatus::offered)
    (void)book_.fail(guest, now_);
}

void SharedSession::Core::remove_source(Spoke& spoke,
                                        const net::kakusan::SeatBinding& seat) {
  emit(RemovalRequested{spoke.guest, seat, spoke.console_seat});
  if (!spoke.console_seat && spoke.joining && !spoke.join_resolved) {
    abandoned_.push_back(AbandonedJoin{spoke.guest, seat});
    if (abandoned_.size() > abandoned_join_limit) abandoned_.pop_front();
  }
  spoke.console_seat.reset();
  spoke.joining.reset();
  spoke.join_resolved = false;
}

auto SharedSession::Core::resolve_abandoned(
    const net::kakusan::SeatBinding& seat,
    std::optional<controllers::SeatNumber> console_seat) -> bool {
  const auto found = std::ranges::find(abandoned_, seat, &AbandonedJoin::seat);
  if (found == abandoned_.end()) return false;
  const auto guest = found->guest;
  abandoned_.erase(found);
  if (console_seat) emit(RemovalRequested{guest, seat, console_seat});
  return true;
}

void SharedSession::Core::start_media(Spoke& spoke) {
  if (spoke.sender || !spoke.transcript) return;
  auto key = spoke.link->media_key();
  if (!key) return feed(spoke, LocalFailure{now_, key.error()});
  const auto connection = net::kakusan::TransportFrameCodec::connection_id(
      crypto_, *spoke.transcript);
  if (!connection) return feed(spoke, LocalFailure{now_, connection.error()});
  spoke.sender = std::make_unique<net::kakusan::MediaSender>(
      crypto_, std::move(key).value(), connection.value(), now_, tuning_.media);
  update_forwarding();
  request_idr();
}

void SharedSession::Core::set_tuning(const Tuning& tuning) noexcept {
  tuning_.media = tuning.media.bounded();
  tuning_.keyframe_interval_us =
      std::max(tuning.keyframe_interval_us, shortest_keyframe_interval_us);
}

void SharedSession::Core::report_media(Spoke& spoke) {
  if (!spoke.sender || spoke.media_reported) return;
  spoke.media_reported = true;
  const auto report = spoke.sender->report();
  const auto& tuning = spoke.sender->tuning();
  const auto drop_ms = tuning.drop_windows_to_fail *
                       net::kakusan::media_drop_window_us /
                       microseconds_per_millisecond;
  const auto ack_ms = tuning.progress_timeout_us / microseconds_per_millisecond;
  std::string text{"media sender "};
  if (report.stop == net::kakusan::MediaStop::drop_windows)
    text += "stopped after " + std::to_string(drop_ms) +
            " ms of shedding half the stream";
  else if (report.stop == net::kakusan::MediaStop::ack_timeout)
    text += "stopped after " + std::to_string(ack_ms) +
            " ms without an acknowledgement";
  else
    text += "closed";
  if (report.rtt_sampled)
    text +=
        ": round trip " +
        std::to_string(report.smoothed_rtt_us / microseconds_per_millisecond) +
        " ms";
  else
    text += ": no round trip measured";
  text += ", floor " +
          std::to_string(report.floor_rtt_us / microseconds_per_millisecond) +
          " ms";
  text += ", lost " + std::to_string(report.lost_datagrams) + " datagrams";
  text += ", in flight " + std::to_string(report.bytes_in_flight) + " B";
  text += ", queue limit " +
          std::to_string(report.queue_age_us / microseconds_per_millisecond) +
          " ms";
  text += ", sent " + std::to_string(report.datagrams_sent) + " datagrams";
  text += ", " + std::to_string(report.acks) + " acknowledgements";
  text += ", " + std::to_string(report.source_packets) + " source packets";
  text += ", dropped " + std::to_string(report.dropped_packets);
  text += " (" + std::to_string(report.dropped_video) + " video)";
  emit(Diagnostic{spoke.guest, std::move(text)});
}

void SharedSession::Core::stop_media(Spoke& spoke) {
  report_media(spoke);
  spoke.sender.reset();
  update_forwarding();
}

void SharedSession::Core::update_forwarding() {
  const bool wanted = source_.has_value() &&
                      std::ranges::any_of(spokes_, [](const auto& spoke) {
                        return spoke->sender != nullptr;
                      });
  if (wanted == forwarding_) return;
  forwarding_ = wanted;
  capture_.set_forwarding(wanted);
}

void SharedSession::Core::request_idr() noexcept { idr_pending_ = true; }

void SharedSession::Core::expire_invitations() {
  book_.expire(now_);
  for (std::size_t index = 0; index < spokes_.size(); ++index) {
    auto& spoke = *spokes_[index];
    if (spoke.machine.state() != Progress::waiting) continue;
    if (!spoke.issued_us) {
      if (now_ >= spoke.created_us + net::kakusan::gathering_timeout_us)
        feed(spoke, LocalFailure{now_, net::kakusan::Error::no_path});
      continue;
    }
    if (now_ >= *spoke.issued_us + invitation_lifetime_us ||
        wall_ >= spoke.expires_at)
      feed(spoke, InvitationExpired{now_});
  }
}

void SharedSession::Core::check_source_keys() {
  if (!source_) return;
  auto keys = capture_.clone_keys();
  if (keys && keys->generation() == source_->generation) {
    return;
  }
  const bool version_changed =
      !keys || keys->version() != source_->protocol_version;
  end_source_spokes(version_changed);
  source_.reset();
  update_forwarding();
}

void SharedSession::Core::end_source_spokes(bool every_spoke) {
  for (std::size_t index = 0; index < spokes_.size(); ++index) {
    auto& spoke = *spokes_[index];
    if (every_spoke || spoke.setup_generation) feed(spoke, SourceEnded{now_});
  }
}

void SharedSession::Core::forward_capture() {
  const auto drained = capture_.drain();
  check_source_keys();
  if (!source_ || !forwarding_) return;
  if (drained.dropped.dropped_video > 0) request_idr();
  for (const auto& datagram : drained.datagrams) {
    const auto verdict = PacketVerdict::classify(
        source_->description.profiles.size(), datagram.view());
    if (verdict.admission != Admission::forward) continue;
    for (auto& spoke : spokes_) {
      if (spoke->sender)
        (void)spoke->sender->submit(verdict.kind, datagram.facts.key_position,
                                    datagram.bytes, now_);
    }
  }
}

void SharedSession::Core::poll_senders() {
  for (std::size_t index = 0; index < spokes_.size(); ++index) {
    auto& spoke = *spokes_[index];
    if (!spoke.sender) continue;
    media_.clear();
    const auto polled = spoke.sender->poll(now_, media_);
    for (const auto& datagram : media_) (void)spoke.link->send_media(datagram);
    media_.clear();
    if (spoke.sender->take_idr_request()) request_idr();
    if (!polled) {
      report_media(spoke);
      feed(spoke, LocalFailure{now_, polled.error()});
    } else if (const auto failure = spoke.sender->failure()) {
      report_media(spoke);
      feed(spoke, LocalFailure{now_, *failure});
    }
  }
}

void SharedSession::Core::settle() {
  std::erase_if(spokes_, [](const auto& spoke) {
    if (spoke->machine.state() != Progress::gone) return false;
    spoke->link->close();
    return true;
  });
  update_forwarding();
  if (!forwarding_) idr_pending_ = false;
  if (idr_pending_ &&
      (!last_idr_us_ || now_ >= *last_idr_us_ + tuning_.keyframe_interval_us)) {
    idr_pending_ = false;
    last_idr_us_ = now_;
    emit(IdrRequested{});
  }
  const auto snapshot = seats_.summary();
  if (snapshot != reported_seats_) {
    reported_seats_ = snapshot;
    emit(SeatsChanged{snapshot});
  }
}

void SharedSession::Core::emit(Output output) {
  outputs_.push_back(std::move(output));
}

}  // namespace libchiaking::kakusan
