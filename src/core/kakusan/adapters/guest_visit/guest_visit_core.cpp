#include "src/core/kakusan/adapters/guest_visit/guest_visit_core.hpp"

#include "src/core/kakusan/adapters/shared_setup.hpp"
#include "src/core/net/kakusan/identity_proof.hpp"
#include "src/core/net/kakusan/short_block.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace libchiaking::kakusan {
auto GuestVisit::Core::create(Dependencies dependencies,
                              DeviceIdentity identity)
    -> net::kakusan::Result<std::unique_ptr<Core>> {
  if (!dependencies.platform.supplies(platform::PlatformCapability::crypto))
    return net::kakusan::fail(net::kakusan::Error::crypto_unavailable);
  return std::unique_ptr<Core>{new Core{
      dependencies, dependencies.platform.crypto(), std::move(identity)}};
}

GuestVisit::Core::Core(Dependencies dependencies,
                       platform::CryptoProvider& crypto,
                       DeviceIdentity identity) noexcept
    : platform_(&dependencies.platform),
      crypto_(&crypto),
      random_(&dependencies.random),
      transports_(&dependencies.transports),
      sink_(&dependencies.media),
      identity_(std::move(identity)) {}

GuestVisit::Core::~Core() {
  spoke_.reset();
  if (link_) link_->close();
}

auto GuestVisit::Core::forwarded_failure(play::ForwardedStream::Refusal refusal)
    const -> std::optional<net::kakusan::Error> {
  if (refusal == play::ForwardedStream::Refusal::crypto_unavailable)
    return net::kakusan::Error::crypto_unavailable;
  if (refusal == play::ForwardedStream::Refusal::media_failed &&
      source_->fault())
    return std::nullopt;
  return net::kakusan::Error::source_format;
}

auto GuestVisit::Core::read(platform::CryptoProvider& crypto,
                            std::string_view block, std::uint64_t wall_seconds)
    -> net::kakusan::Result<net::kakusan::SealedInvitation> {
  auto invitation =
      net::kakusan::ShortBlockCodec::parse_invitation(crypto, block);
  if (!invitation) return net::kakusan::fail(invitation.error());
  if (const auto fresh =
          InvitationBook::check(invitation.value(), wall_seconds);
      !fresh)
    return net::kakusan::fail(fresh.error());
  return invitation;
}

auto GuestVisit::Core::summarize(
    const net::kakusan::SealedInvitation& invitation) -> InvitationSummary {
  const auto& offered = invitation.body;
  InvitationSummary summary;
  summary.host_hint = IdentityHint{offered.identity_hint};
  summary.seats.capacity = offered.capacity;
  summary.expires_at = offered.invite_expires_at;
  summary.stream_protocol = offered.source_protocol;
  return summary;
}

auto GuestVisit::Core::preview(platform::CryptoProvider& crypto,
                               std::string_view block,
                               std::uint64_t wall_seconds)
    -> net::kakusan::Result<InvitationSummary> {
  return read(crypto, block, wall_seconds).transform(&Core::summarize);
}

auto GuestVisit::Core::accept_invitation(std::string_view block, Clock clock)
    -> net::kakusan::Result<InvitationSummary> {
  if (invitation_ || closed_)
    return net::kakusan::fail(net::kakusan::Error::internal);
  advance(clock.now_us);
  wall_seconds_ = clock.wall_seconds;
  auto invitation = read(*crypto_, block, wall_seconds_);
  if (!invitation) return net::kakusan::fail(invitation.error());
  const auto summary = summarize(invitation.value());
  net::kakusan::BlockSeed seed{};
  if (!random_->fill_bytes(seed))
    return net::kakusan::fail(net::kakusan::Error::crypto_unavailable);
  auto credentials = net::kakusan::ShortBlockCodec::credentials(
      *crypto_, seed, net::kakusan::BlockKind::reply);
  if (!credentials) return net::kakusan::fail(credentials.error());
  auto transport = transports_->create(net::kakusan::TransportRole::guest);
  if (!transport) return net::kakusan::fail(transport.error());
  auto link = std::make_unique<net::kakusan::Link>(
      std::move(transport).value(), net::kakusan::TransportRole::guest);
  if (const auto gathered = link->gather(now_, std::move(credentials).value());
      !gathered)
    return net::kakusan::fail(gathered.error());
  seed_ = seed;

  const auto remaining =
      std::min(summary.expires_at - wall_seconds_, max_waiting_seconds);
  expiry_us_ =
      timing::Deadline::after_us(now_, remaining * microseconds_per_second);
  link_ = std::move(link);
  invitation_ = std::move(invitation).value();
  return summary;
}

auto GuestVisit::Core::enter_code(std::string_view code, std::uint64_t now_us)
    -> net::kakusan::Result<void> {
  if (!spoke_ || spoke_->state() != Progress::verifying || code_entered_)
    return net::kakusan::fail(net::kakusan::Error::internal);
  auto parsed = net::kakusan::Code::parse(code);
  if (!parsed) return net::kakusan::fail(parsed.error());
  advance(now_us);
  code_entered_ = true;
  PeerCodeEntered entered{now_, std::move(parsed).value()};
  feed(std::move(entered));
  return {};
}

auto GuestVisit::Core::controller_ready(std::uint64_t now_us)
    -> net::kakusan::Result<void> {
  if (!spoke_ || spoke_->state() != Progress::getting_ready ||
      !controller_requested_)
    return net::kakusan::fail(net::kakusan::Error::internal);
  advance(now_us);
  controller_requested_ = false;
  feed(ControllerReady{now_});
  return {};
}

auto GuestVisit::Core::set_controller(const net::kakusan::InputState& state,
                                      std::uint64_t now_us)
    -> net::kakusan::Result<void> {
  if (!spoke_ || finished())
    return net::kakusan::fail(net::kakusan::Error::internal);
  if (!net::kakusan::ControllerFrameCodec::validate_input_state(state))
    return net::kakusan::fail(net::kakusan::Error::protocol);
  advance(now_us);
  feed(ControllerSampled{now_, state});
  return {};
}

void GuestVisit::Core::leave(std::uint64_t now_us) {
  advance(now_us);
  if (spoke_) return feed(Cancel{now_, net::kakusan::control::Reason::user});
  if (!closed_) abandon(std::nullopt, net::kakusan::control::Reason::user);
}

void GuestVisit::Core::pump(Clock clock) {
  advance(clock.now_us);
  wall_seconds_ = clock.wall_seconds;
  if (closed_ || !link_) return;
  for (auto& event : link_->pump(now_)) {
    if (closed_) break;
    deliver(event);
  }
  if (closed_) return;
  expire_invitation();
  if (spoke_ && !closed_) run(spoke_->on_time(now_));
  if (!closed_) poll_receivers();
}

auto GuestVisit::Core::next_deadline_us() const noexcept
    -> std::optional<std::uint64_t> {
  if (closed_) return std::nullopt;
  std::optional<std::uint64_t> deadline;
  if (link_) deadline = link_->next_deadline_us();
  if (spoke_)
    deadline = timing::Deadline::earliest(deadline, spoke_->next_deadline_us());
  if (waiting()) deadline = timing::Deadline::earliest(deadline, expiry_us_);
  if (source_)
    deadline = timing::Deadline::earliest(deadline, source_deadline_us_);
  if (media_)
    deadline = timing::Deadline::earliest(deadline, media_->next_deadline_us());
  return deadline;
}

auto GuestVisit::Core::take_output() -> std::optional<Output> {
  if (outputs_.empty()) return std::nullopt;
  auto output = std::move(outputs_.front());
  outputs_.pop_front();
  return output;
}

auto GuestVisit::Core::state() const noexcept -> Progress {
  if (spoke_) return spoke_->state();
  return closed_ ? Progress::gone : Progress::waiting;
}

void GuestVisit::Core::advance(std::uint64_t now_us) noexcept {
  now_ = std::max(now_, now_us);
}

auto GuestVisit::Core::waiting() const noexcept -> bool {
  return !closed_ && invitation_ &&
         (!spoke_ || spoke_->state() == Progress::waiting);
}

auto GuestVisit::Core::finished() const noexcept -> bool {
  const auto current = state();
  return current == Progress::leaving || current == Progress::gone;
}

void GuestVisit::Core::deliver(net::kakusan::LinkEvent& event) {
  if (auto* gathered = std::get_if<net::kakusan::link_event::Gathered>(&event))
    return on_gathered(gathered->local);
  if (auto* media = std::get_if<net::kakusan::link_event::Media>(&event))
    return on_media(media->datagram);
  if (auto* received = std::get_if<net::kakusan::link_event::Control>(&event))
    return on_control(received->message);
  if (!spoke_) {
    if (std::holds_alternative<net::kakusan::link_event::Failed>(event))
      abandon(net::kakusan::Error::no_path, std::nullopt);
    return;
  }
  if (std::holds_alternative<net::kakusan::link_event::Nominated>(event))
    return feed(IceNominated{now_});
  if (auto* established =
          std::get_if<net::kakusan::link_event::Established>(&event)) {
    auto proof = net::kakusan::IdentityProof::sign(
        *crypto_, net::kakusan::Role::guest, transcript_, established->exporter,
        identity_.seed(), std::nullopt);
    if (!proof) return feed(LocalFailure{now_, proof.error()});
    feed(DtlsEstablished{now_, std::move(established->exporter),
                         std::move(proof).value()});
    return;
  }
  if (const auto* acked = std::get_if<net::kakusan::link_event::Acked>(&event))
    return feed(ControlAcked{now_, acked->token});
  if (const auto* feedback =
          std::get_if<net::kakusan::link_event::Feedback>(&event))
    return feed(FeedbackReceived{now_, feedback->frame});
  if (const auto* malformed =
          std::get_if<net::kakusan::link_event::Malformed>(&event))
    return feed(MalformedData{now_, malformed->error});
  if (std::holds_alternative<net::kakusan::link_event::Failed>(event))
    return feed(TransportFailed{now_});
}

void GuestVisit::Core::on_gathered(net::kakusan::TransportDescription& local) {
  if (spoke_ || closed_ || !invitation_) return;
  if (const auto fresh = InvitationBook::check(*invitation_, wall_seconds_);
      !fresh)
    return abandon(fresh.error(), std::nullopt);
  const auto& offered = invitation_->body;
  net::kakusan::ShortReply reply{
      .seed = seed_,
      .invite_tag = net::kakusan::ShortBlockCodec::tag_of(invitation_->hash),
      .dtls_sha256 = local.certificate_digest,
      .identity_hint =
          net::kakusan::ShortBlockCodec::hint_of(identity_.key().bytes()),
      .ice = std::move(local.ice),
      .reply_nonce = {}};
  const auto sealed = net::kakusan::ShortBlockCodec::seal(*crypto_, reply);
  if (!sealed) return abandon(sealed.error(), std::nullopt);
  const auto transcript = net::kakusan::Exchange::transcript(
      *crypto_, invitation_->hash, sealed.value().hash);
  if (!transcript) return abandon(transcript.error(), std::nullopt);

  PairingContext context;
  context.transcript = transcript.value();
  context.parties.guest_identity = identity_.key().bytes();
  context.invitation_id = offered.invitation_id;
  context.peer_hint = offered.identity_hint;
  transcript_ = context.transcript;
  spoke_ = std::make_unique<VisitingPlayer>(*crypto_, *random_, context,
                                            offered.source_protocol);
  const net::kakusan::TransportDescription remote{offered.ice.clone(),
                                                  offered.dtls_sha256};
  if (const auto connected = link_->connect(remote, transcript_, now_);
      !connected)
    return feed(TransportFailed{now_});
  outputs_.push_back(ReplyReady{sealed.value().text.clone()});
}

void GuestVisit::Core::on_control(net::kakusan::control::Message& message) {
  if (spoke_) feed(ControlReceived{now_, std::move(message)});
}

void GuestVisit::Core::on_media(std::span<const std::byte> datagram) {
  if (!media_ || !source_ || !spoke_ || spoke_->state() != Progress::playing)
    return;
  ++media_datagrams_;
  if (!media_first_us_) media_first_us_ = now_;
  auto received = media_->receive(datagram, now_);
  if (!received) return feed(LocalFailure{now_, received.error()});
  if (!received.value()) return;
  ++media_packets_;
  const auto& packet = *received.value();
  play::SharedDatagram sealed;
  sealed.media = packet.kind == net::kakusan::MediaKind::audio
                     ? play::SharedDatagram::Media::audio
                     : play::SharedDatagram::Media::video;
  sealed.key_position = packet.key_position;
  sealed.bytes = packet.bytes;
  if (const auto pushed = source_->receive(sealed); !pushed) {
    if (const auto failure = forwarded_failure(pushed.error()))
      return feed(LocalFailure{now_, *failure});
    return;
  }
  request_idr();
}

void GuestVisit::Core::expire_invitation() {
  if (!waiting() || (wall_seconds_ < invitation_->body.invite_expires_at &&
                     now_ < expiry_us_))
    return;
  if (spoke_) return feed(InvitationExpired{now_});
  abandon(net::kakusan::Error::expired, std::nullopt);
}

void GuestVisit::Core::poll_receivers() {
  if (source_) {
    const auto serviced = source_->service(idle_service);
    if (!serviced) {
      if (const auto failure = forwarded_failure(serviced.error()))
        return feed(LocalFailure{now_, *failure});
    } else {
      source_deadline_us_ =
          serviced.value() < idle_service
              ? std::optional<std::uint64_t>{timing::Deadline::after_us(
                    now_,
                    static_cast<std::uint64_t>(
                        std::chrono::microseconds{serviced.value()}.count()))}
              : std::nullopt;
      request_idr();
    }
  }
  if (!media_ || !link_) return;
  while (const auto ack = media_->poll(now_)) {
    if (link_->send_media_ack(*ack)) ++media_acks_;
  }
}

void GuestVisit::Core::request_idr() {
  if (source_ && source_->wants_keyframe()) feed(IdrNeeded{now_});
}

void GuestVisit::Core::feed(VisitingPlayer::Event event) {
  if (spoke_)
    loop_.feed(*spoke_, std::move(event),
               [this](auto& effect) { apply(effect); });
}

void GuestVisit::Core::run(VisitingPlayer::Effects effects) {
  loop_.run(*spoke_, std::move(effects),
            [this](auto& effect) { apply(effect); });
}

void GuestVisit::Core::apply(VisitingPlayer::Effect& effect) {
  std::visit([this](auto& item) { on(item); }, effect);
}

void GuestVisit::Core::on(SendControl& effect) {
  const bool sent = link_ && link_->send_control(effect.message, effect.token,
                                                 effect.terminal, now_);
  if (!sent) feed(TransportFailed{now_});
}

void GuestVisit::Core::on(ShowState& effect) {
  outputs_.push_back(State{effect.state, effect.error, effect.reason,
                           effect.by_peer, spoke_->binding()});
  if (effect.state == Progress::verifying) present_code();
}

void GuestVisit::Core::on(PeerIdentified& effect) {
  const auto print = net::kakusan::Hash::sha256(*crypto_, effect.key);
  if (!print || !effect.hosting || !invitation_)
    return feed(LocalFailure{
        now_, print ? net::kakusan::Error::internal : print.error()});
  outputs_.push_back(HostIdentified{
      effect.key, print.value(),
      SeatSummary{invitation_->body.capacity, effect.hosting->local_seats,
                  effect.hosting->remote_seats,
                  effect.hosting->reserved_seats}});
}

void GuestVisit::Core::on(DisplayCode& effect) {
  outputs_.push_back(CodeReady{std::move(effect.code)});
}

void GuestVisit::Core::on(EraseSecrets&) {
  forget_secrets();
  discard_codes();
}

void GuestVisit::Core::on(CloseTransport&) {
  closed_ = true;
  loop_.clear();
  stop_media();
  if (link_) link_->close();
}

void GuestVisit::Core::on(RecordAuthenticationFailure&) {}

void GuestVisit::Core::on(ConsumeInvitation&) {}

void GuestVisit::Core::on(ConfigureReceiver& effect) {
  const auto fps = effect.setup.video_fps;
  auto shared = SharedSetup::stream(std::move(effect.setup));
  if (!shared) return feed(ReceiverFailed{now_});
  auto opened =
      play::ForwardedStream::open(*platform_, std::move(shared.value().stream),
                                  std::move(shared.value().keys), *sink_);
  if (!opened) {
    if (opened.error() == play::ForwardedStream::Refusal::crypto_unavailable)
      return feed(LocalFailure{now_, net::kakusan::Error::crypto_unavailable});
    return feed(ReceiverFailed{now_});
  }
  source_ = std::move(opened).value();
  source_deadline_us_.reset();
  stream_announced_ = true;
  outputs_.push_back(StreamReady{fps});
  feed(ReceiverReady{now_});
}

void GuestVisit::Core::on(StartMediaReception&) {
  if (!link_ || media_) return;
  auto key = link_->media_key();
  if (!key) return feed(LocalFailure{now_, key.error()});
  const auto connection =
      net::kakusan::TransportFrameCodec::connection_id(*crypto_, transcript_);
  if (!connection) return feed(LocalFailure{now_, connection.error()});
  media_ = std::make_unique<net::kakusan::MediaReceiver>(
      *crypto_, std::move(key).value(), connection.value());
}

void GuestVisit::Core::on(StopMediaReception&) { stop_media(); }

void GuestVisit::Core::on(PrepareController& effect) {
  controller_requested_ = true;
  outputs_.push_back(ControllerRequested{
      effect.grant.binding, effect.grant.capacity, effect.grant.local_seats,
      effect.grant.allowed_buttons});
}

void GuestVisit::Core::on(SendInput& effect) {
  if (link_ && spoke_->state() == Progress::playing)
    (void)link_->send_input(effect.frame);
}

void GuestVisit::Core::on(ApplyRumble& effect) {
  outputs_.push_back(Rumble{effect.low_frequency, effect.high_frequency});
}

void GuestVisit::Core::on(StopRumble&) { outputs_.push_back(Rumble{0, 0}); }

void GuestVisit::Core::present_code() {
  if (code_drawn_) return;
  code_drawn_ = true;
  auto value = net::kakusan::Code::draw(*random_);
  if (!value) return feed(LocalFailure{now_, net::kakusan::Error::internal});
  feed(LocalCode{now_, std::move(*value)});
}

void GuestVisit::Core::stop_media() {
  if (media_) {
    std::string text{"media receiver closed: "};
    text += std::to_string(media_datagrams_) + " datagrams";
    text += ", " + std::to_string(media_packets_) + " source packets";
    text += ", " + std::to_string(media_acks_) + " acknowledgements sent";
    if (media_first_us_)
      text += ", first datagram " +
              std::to_string((now_ - *media_first_us_) /
                             microseconds_per_millisecond) +
              " ms before the end";
    outputs_.push_back(Diagnostic{std::move(text)});
    media_datagrams_ = 0;
    media_packets_ = 0;
    media_acks_ = 0;
    media_first_us_.reset();
    media_.reset();
  }
  source_.reset();
  source_deadline_us_.reset();
  if (stream_announced_) {
    stream_announced_ = false;
    outputs_.push_back(StreamStopped{});
  }
}

void GuestVisit::Core::abandon(
    std::optional<net::kakusan::Error> error,
    std::optional<net::kakusan::control::Reason> reason) {
  forget_secrets();
  discard_codes();
  if (link_) link_->close();
  closed_ = true;
  outputs_.push_back(State{Progress::gone, error, reason, false, std::nullopt});
}

void GuestVisit::Core::forget_secrets() noexcept {
  media_.reset();
  source_.reset();
  source_deadline_us_.reset();
}

void GuestVisit::Core::discard_codes() noexcept {
  std::erase_if(outputs_, [](const Output& output) {
    return std::holds_alternative<CodeReady>(output);
  });
}

}  // namespace libchiaking::kakusan
