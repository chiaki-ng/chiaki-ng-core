#include <libchiaking/platform/runtime/threading.hpp>
#include "src/core/play/adapters/links/scripted_link_core.hpp"

#include "src/core/net/protocol/takion/detail/crypto_context.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/platform_crypto_provider.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/play/adapters/links/link_evidence.hpp"
#include "src/core/play/adapters/media/media_units.hpp"
#include "src/core/play/adapters/session/controllers_core.hpp"
#include "src/core/play/adapters/session/seats_core.hpp"
#include "src/core/utils/text/hex.hpp"

#include <algorithm>
#include <memory>
#include <utility>
#include <variant>

namespace libchiaking::play::testing {

void ScriptedLink::Core::Script::enqueue(Action action) {
  {
    const platform::Lock lock(mutex_);
    actions_.push_back(std::move(action));
  }
  changed_.notify_all();
}

auto ScriptedLink::Core::Script::take() -> std::deque<Action> {
  const platform::Lock lock(mutex_);
  woken_ = false;
  return std::exchange(actions_, {});
}

void ScriptedLink::Core::Script::wait(std::chrono::milliseconds timeout) {
  platform::Lock lock(mutex_);
  if (stepped_) {
    parked_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this] { return turns_ != 0 || !stepped_ || over_; });
    if (turns_ != 0) --turns_;
    parked_ = false;
    woken_ = false;
    return;
  }
  static_cast<void>(changed_.wait_for(
      lock, timeout, [this] { return woken_ || !actions_.empty(); }));
  woken_ = false;
}

void ScriptedLink::Core::Script::wake(bool leaving) noexcept {
  {
    const platform::Lock lock(mutex_);
    woken_ = true;
    if (leaving) stepped_ = false;
  }
  changed_.notify_all();
}

void ScriptedLink::Core::Script::step_by_hand() {
  const platform::Lock lock(mutex_);
  stepped_ = true;
}

auto ScriptedLink::Core::Script::turn() -> bool {
  platform::Lock lock(mutex_);
  if (!stepped_) return false;
  changed_.wait(lock, [this] { return parked_ || over_ || !stepped_; });
  if (over_ || !stepped_) return false;
  parked_ = false;
  ++turns_;
  changed_.notify_all();
  changed_.wait(lock, [this] { return parked_ || over_ || !stepped_; });
  return !over_ && stepped_;
}

void ScriptedLink::Core::Script::settle() {
  platform::Lock lock(mutex_);
  changed_.wait(lock, [this] { return parked_ || over_ || !stepped_; });
}

void ScriptedLink::Core::Script::over(bool finished) noexcept {
  {
    const platform::Lock lock(mutex_);
    over_ = finished;
  }
  changed_.notify_all();
}

void ScriptedLink::Core::Script::report_launch_to(
    SessionEvents* events) noexcept {
  launch_events_.store(events);
}

auto ScriptedLink::Core::Script::launch_events() const noexcept
    -> SessionEvents* {
  return launch_events_.load();
}

void ScriptedLink::Core::Script::share_through(
    net::protocol::takion::SourceTap* tap,
    platform::CryptoProvider* crypto) noexcept {
  tap_.store(tap);
  crypto_.store(crypto);
  const platform::Lock lock(mutex_);
  parked_ = false;
  over_ = false;
  turns_ = 0;
}

auto ScriptedLink::Core::Script::tap() const noexcept
    -> net::protocol::takion::SourceTap* {
  return tap_.load();
}

auto ScriptedLink::Core::Script::crypto() const noexcept
    -> platform::CryptoProvider* {
  return crypto_.load();
}

auto ScriptedLink::Core::Script::fresh_keys()
    -> net::protocol::takion::SourceReceiveKeys {
  const platform::Lock lock(mutex_);
  SecretArray<net::protocol::takion::source_key_size> key;
  SecretArray<net::protocol::takion::source_key_size> iv;
  const auto fill =
      [this](SecretArray<net::protocol::takion::source_key_size>& output) {
        for (auto& byte : output) {
          key_state_ += 0x9e3779b97f4a7c15ULL;
          auto mixed = key_state_;
          mixed = (mixed ^ (mixed >> 30U)) * 0xbf58476d1ce4e5b9ULL;
          mixed = (mixed ^ (mixed >> 27U)) * 0x94d049bb133111ebULL;
          byte = static_cast<std::byte>((mixed ^ (mixed >> 31U)) & 0xffU);
        }
      };
  fill(key);
  fill(iv);
  keys_.emplace(std::move(key), std::move(iv));
  return keys_->clone();
}

auto ScriptedLink::Core::Script::keys() const
    -> std::optional<net::protocol::takion::SourceReceiveKeys> {
  const platform::Lock lock(mutex_);
  if (!keys_) return std::nullopt;
  return keys_->clone();
}

void ScriptedLink::Core::Script::pumping(Wire* wire) noexcept {
  const platform::Lock lock(mutex_);
  wire_ = wire;
  pump_thread_ = wire != nullptr ? platform::ThreadIdentity::current()
                                 : platform::ThreadIdentity{};
}

auto ScriptedLink::Core::Script::pumped_here() const noexcept -> Wire* {
  const platform::Lock lock(mutex_);
  return pump_thread_ == platform::ThreadIdentity::current() ? wire_ : nullptr;
}

void ScriptedLink::Core::Script::note(std::string call) {
  const platform::Lock lock(mutex_);
  calls_.push_back(std::move(call));
}

void ScriptedLink::Core::Script::sample(
    const net::protocol::remote_play::ControllerState& state) {
  const platform::Lock lock(mutex_);
  calls_.emplace_back("controller");
  samples_[controllers::SeatNumber::masked(state.controller_id).index()]
      .push_back(Controllers::Core::typed(state));
}

void ScriptedLink::Core::Script::seat_request(SeatRequest request) {
  const platform::Lock lock(mutex_);
  seat_requests_.push_back(request);
}

void ScriptedLink::Core::Script::keyframe() {
  const platform::Lock lock(mutex_);
  calls_.emplace_back("keyframe");
  ++keyframes_;
}

void ScriptedLink::Core::Script::pin(std::span<const std::byte> digits) {
  const platform::Lock lock(mutex_);
  calls_.emplace_back("login_pin");
  pin_.assign(digits);
}

void ScriptedLink::Core::Script::refuse_start(LinkFailure detail) {
  const platform::Lock lock(mutex_);
  start_refusal_ = std::move(detail);
}

void ScriptedLink::Core::Script::refuse_seat_requests(
    net::protocol::remote_play::StreamError error) {
  const platform::Lock lock(mutex_);
  seat_refusal_ = error;
}

void ScriptedLink::Core::Script::refuse_login_pin(
    net::protocol::remote_play::StreamError error) {
  const platform::Lock lock(mutex_);
  pin_refusal_ = error;
}

void ScriptedLink::Core::Script::report_protocol_version(
    std::uint32_t version) {
  const platform::Lock lock(mutex_);
  protocol_version_ = version;
}

auto ScriptedLink::Core::Script::start_refusal() const
    -> std::optional<LinkFailure> {
  const platform::Lock lock(mutex_);
  return start_refusal_;
}

auto ScriptedLink::Core::Script::seat_refusal() const
    -> std::optional<net::protocol::remote_play::StreamError> {
  const platform::Lock lock(mutex_);
  return seat_refusal_;
}

auto ScriptedLink::Core::Script::pin_refusal() const
    -> std::optional<net::protocol::remote_play::StreamError> {
  const platform::Lock lock(mutex_);
  return pin_refusal_;
}

auto ScriptedLink::Core::Script::protocol_version() const
    -> std::optional<std::uint32_t> {
  const platform::Lock lock(mutex_);
  return protocol_version_;
}

auto ScriptedLink::Core::Script::calls() const -> std::vector<std::string> {
  const platform::Lock lock(mutex_);
  return calls_;
}

auto ScriptedLink::Core::Script::controller_samples(
    controllers::SeatNumber seat) const
    -> std::vector<controllers::ControllerState> {
  const platform::Lock lock(mutex_);
  return samples_[seat.index()];
}

auto ScriptedLink::Core::Script::seat_requests() const
    -> std::vector<SeatRequest> {
  const platform::Lock lock(mutex_);
  return seat_requests_;
}

auto ScriptedLink::Core::Script::keyframe_requests() const -> std::size_t {
  const platform::Lock lock(mutex_);
  return keyframes_;
}

auto ScriptedLink::Core::Script::login_pin() const -> SecretString {
  const platform::Lock lock(mutex_);
  return pin_.clone();
}

ScriptedLink::Core::Wire::Wire(std::shared_ptr<Script> script,
                               platform::StopSource stop,
                               std::optional<Venue> venue)
    : Link(venue), script_(std::move(script)), stop_(std::move(stop)) {}

auto ScriptedLink::Core::Wire::running() const noexcept -> bool {
  return state_ ==
             net::protocol::remote_play::StreamState::establishing_route ||
         state_ ==
             net::protocol::remote_play::StreamState::negotiating_stream ||
         state_ == net::protocol::remote_play::StreamState::streaming ||
         state_ == net::protocol::remote_play::StreamState::stopping;
}

auto ScriptedLink::Core::Wire::bytes(std::string_view hex)
    -> std::vector<std::byte> {
  std::vector<std::byte> decoded(hex.size() / 2U);
  static_cast<void>(text::decode_hex(hex, decoded));
  return decoded;
}

void ScriptedLink::Core::Wire::start_shared_stream() {
  auto* tap = script_->tap();
  if (tap == nullptr) {
    script_->note("shared_stream_untapped");
    return;
  }
  script_->report_protocol_version(shared_protocol_version);
  tap->on_source_keys(shared_protocol_version, script_->fresh_keys());
  script_->note("shared_stream");
}

void ScriptedLink::Core::Wire::send_shared_media() {
  namespace takion = net::protocol::takion;
  auto* tap = script_->tap();
  auto* crypto = script_->crypto();
  const auto keys = script_->keys();
  const auto profile =
      takion::resolve_takion_wire_profile(shared_protocol_version);
  if (tap == nullptr || crypto == nullptr || !keys || !profile) {
    script_->note("shared_media_unsealed");
    return;
  }
  SecretArray<takion::source_key_size> old_key;
  SecretArray<takion::source_key_size> old_iv;
  static_cast<void>(text::decode_hex(recorded_key, old_key));
  static_cast<void>(text::decode_hex(recorded_iv, old_iv));
  takion::PlatformTakionCryptoProvider provider{*crypto, script_->threading()};
  const auto opened = provider.create_receive_only(
      takion::SourceReceiveKeys{std::move(old_key), std::move(old_iv)});
  const auto sealed = provider.create_receive_only(*keys);
  if (!opened || !sealed) {
    script_->note("shared_media_unsealed");
    return;
  }
  for (const auto& unit : recorded) {
    auto datagram = bytes(unit.datagram);
    const takion::PacketType type{
        std::to_integer<std::uint8_t>(datagram.front())};
    const auto offsets =
        takion::CryptoContext::packet_offsets(*type.family(), *profile);
    const auto packet =
        takion::PacketCodec::decode_plain_packet(datagram, *profile);
    const auto described =
        packet ? takion::PacketCodec::decode_av_packet(packet.value(), *profile)
               : Expected<takion::AvPacketValues, takion::ParseFailure>{
                     unexpected(packet.error())};
    if (!offsets || !described) {
      script_->note("shared_media_unsealed");
      return;
    }
    const auto body = std::span<std::byte>{datagram}.subspan(
        datagram.size() - described.value().data.size());
    const auto position =
        unit.key_position + offsets.value().cipher_key_position_addition;
    const auto mac =
        datagram.begin() + static_cast<std::ptrdiff_t>(offsets.value().mac);
    std::fill_n(mac, takion::takion_mac_size, std::byte{});
    if (!opened.value()->apply_keystream(position, body, body) ||
        !sealed.value()->apply_keystream(position, body, body)) {
      script_->note("shared_media_unsealed");
      return;
    }
    const auto tag = sealed.value()->authenticate(unit.key_position, datagram);
    if (!tag) {
      script_->note("shared_media_unsealed");
      return;
    }
    std::ranges::copy(tag.value(), mac);
    tap->on_source_datagram(takion::PacketCodec::describe_source_datagram(
        described.value(), *profile, unit.key_position, datagram));
  }
  script_->note("shared_media");
}

auto ScriptedLink::Core::Wire::start() -> Result {
  script_->note("start");
  script_->over(false);
  if (stop_.stop_requested()) {
    ending().record_failure(LinkFailure::abandoned(
        net::protocol::remote_play::FailureStage::unknown,
        net::protocol::remote_play::StreamError::route_failed));
    state_ = net::protocol::remote_play::StreamState::failed;
    script_->over(true);
    sink_->on_event(net::protocol::remote_play::event::Failed{
        net::protocol::remote_play::StreamFailure::cancelled});
    return unexpected(net::protocol::remote_play::StreamError::route_failed);
  }
  if (const auto refusal = script_->start_refusal()) {
    ending().record_failure(*refusal);
    state_ = net::protocol::remote_play::StreamState::failed;
    script_->over(true);
    sink_->on_event(net::protocol::remote_play::event::Failed{
        net::protocol::remote_play::StreamFailure::route_establishment_failed});
    return unexpected(refusal->result());
  }
  become(net::protocol::remote_play::StreamState::establishing_route);
  return {};
}

auto ScriptedLink::Core::Wire::pump_once() -> Result {
  if (stop_.stop_requested() && running()) {
    finish();
    return {};
  }
  auto actions = script_->take();
  script_->pumping(this);
  for (auto& action : actions) action(*this);
  script_->pumping(nullptr);
  return {};
}

auto ScriptedLink::Core::Wire::wait(std::chrono::milliseconds timeout)
    -> Result {
  script_->wait(timeout);
  return {};
}

void ScriptedLink::Core::Wire::wake() noexcept {
  script_->wake(stop_.stop_requested());
}

auto ScriptedLink::Core::Wire::stop() -> Result {
  script_->note("stop");
  finish();
  return {};
}

auto ScriptedLink::Core::Wire::stop_for_sleep() -> Result {
  script_->note("stop_for_sleep");
  finish();
  return {};
}

auto ScriptedLink::Core::Wire::send_controller_state(
    const net::protocol::remote_play::ControllerState& state) -> Result {
  script_->sample(state);
  return {};
}

auto ScriptedLink::Core::Wire::submit_pin(std::span<const std::byte> pin)
    -> Result {
  script_->pin(pin);
  if (const auto refusal = script_->pin_refusal()) return unexpected(*refusal);
  return {};
}

auto ScriptedLink::Core::Wire::report_video_loss(std::uint16_t, std::uint16_t)
    -> Result {
  script_->note("video_loss");
  return {};
}

void ScriptedLink::Core::Wire::note(
    const net::protocol::remote_play::command::GotoBed&) {
  script_->note("rest");
}

void ScriptedLink::Core::Wire::note(
    const net::protocol::remote_play::command::GoHome&) {
  script_->note("go_home");
}

void ScriptedLink::Core::Wire::note(
    const net::protocol::remote_play::command::KeyboardSetText&) {
  script_->note("keyboard_text");
}

void ScriptedLink::Core::Wire::note(
    const net::protocol::remote_play::command::KeyboardAccept&) {
  script_->note("keyboard_accept");
}

void ScriptedLink::Core::Wire::note(
    const net::protocol::remote_play::command::KeyboardReject&) {
  script_->note("keyboard_reject");
}

void ScriptedLink::Core::Wire::note(
    const net::protocol::remote_play::command::CouchSetAccountId& command) {
  script_->seat_request({SeatRequest::Kind::choose_account,
                         controllers::SeatNumber::masked(command.pad),
                         command.account_id, std::nullopt});
  script_->note("seat_account");
}

void ScriptedLink::Core::Wire::note(
    const net::protocol::remote_play::command::CouchJoin& command) {
  std::optional<controllers::ControllerKind> controller;
  for (const auto kind : {controllers::ControllerKind::console_default,
                          controllers::ControllerKind::dualshock4,
                          controllers::ControllerKind::dualsense})
    if (Seats::Core::wire(kind) == command.controller_type) controller = kind;
  script_->seat_request({SeatRequest::Kind::join,
                         controllers::SeatNumber::masked(command.pad),
                         std::nullopt, controller});
  script_->note("seat_join");
}

void ScriptedLink::Core::Wire::note(
    const net::protocol::remote_play::command::CouchRemove& command) {
  script_->seat_request({SeatRequest::Kind::remove,
                         controllers::SeatNumber::masked(command.pad),
                         std::nullopt, std::nullopt});
  script_->note("seat_remove");
}

void ScriptedLink::Core::Wire::note(
    const net::protocol::remote_play::command::CouchPasscode& command) {
  script_->seat_request({SeatRequest::Kind::passcode,
                         controllers::SeatNumber::masked(command.pad),
                         std::nullopt, std::nullopt});
  script_->note("seat_passcode");
}

auto ScriptedLink::Core::Wire::execute(
    const net::protocol::remote_play::StreamCommand& command) -> Result {
  std::visit([this](const auto& sent) { note(sent); }, command);
  const bool seat_request =
      std::holds_alternative<
          net::protocol::remote_play::command::CouchSetAccountId>(command) ||
      std::holds_alternative<net::protocol::remote_play::command::CouchJoin>(
          command);
  if (const auto refusal = script_->seat_refusal(); refusal && seat_request)
    return unexpected(*refusal);
  return {};
}

auto ScriptedLink::Core::Wire::send_microphone(std::span<const std::byte>)
    -> Result {
  script_->note("microphone_frame");
  return {};
}

auto ScriptedLink::Core::Wire::connect_microphone() -> Result {
  script_->note("microphone_connect");
  return {};
}

auto ScriptedLink::Core::Wire::set_microphone_muted(bool muted) -> Result {
  script_->note(muted ? "microphone_mute" : "microphone_unmute");
  return {};
}

auto ScriptedLink::Core::Wire::request_idr() -> Result {
  script_->keyframe();
  return {};
}

auto ScriptedLink::Core::Wire::send_audio_state(
    net::protocol::remote_play::AudioStateKind, std::span<const std::byte>)
    -> Result {
  script_->note("audio_state");
  return {};
}

auto ScriptedLink::Core::Wire::send_keyboard(const KeyboardState&) -> Result {
  script_->note("keyboard");
  return {};
}

auto ScriptedLink::Core::Wire::takion_version() const noexcept
    -> std::optional<std::uint32_t> {
  return script_->protocol_version();
}

void ScriptedLink::Core::Wire::report(
    net::protocol::remote_play::StreamEvent reported) {
  if (sink_ != nullptr) sink_->on_event(std::move(reported));
}

void ScriptedLink::Core::Wire::become(
    net::protocol::remote_play::StreamState state) {
  state_ = state;
  report(net::protocol::remote_play::event::StateChanged{state});
}

void ScriptedLink::Core::Wire::fail(LinkFailure detail) {
  ending().record_failure(std::move(detail));
  state_ = net::protocol::remote_play::StreamState::failed;
  script_->over(true);
  report(net::protocol::remote_play::event::Failed{
      net::protocol::remote_play::StreamFailure::pipeline_failed});
}

void ScriptedLink::Core::Wire::fail_cleanup(LinkFailure detail) {
  ending().record_cleanup_failure(std::move(detail));
}

void ScriptedLink::Core::Wire::quit(
    net::protocol::remote_play::QuitReason reason, std::string_view text) {
  ending().record_quit(reason, text);
  finish();
}

void ScriptedLink::Core::Wire::finish() {
  if (state_ == net::protocol::remote_play::StreamState::stopped ||
      state_ == net::protocol::remote_play::StreamState::failed)
    return;
  state_ = net::protocol::remote_play::StreamState::stopped;
  script_->over(true);
  report(ending().stopped_event());
}

ScriptedLink::Core::Core(ScriptedLink& face,
                         platform::ThreadingProvider& threading,
                         const platform::ClockProvider& clock)
    : face_(face), script_(std::make_shared<Script>(threading, clock)) {}

auto ScriptedLink::Core::link(platform::StopSource stop, Where where,
                              bool old_service) -> std::unique_ptr<Link> {
  return std::make_unique<Wire>(
      script_, std::move(stop),
      Venue::of(where,
                old_service
                    ? std::optional{catalog::ServiceGeneration::old_service}
                    : std::nullopt));
}

auto ScriptedLink::Core::opener(platform::StopSource stop, Where where)
    -> Opener {
  return [script = script_, stop = std::move(stop),
          route = Venue::of(where, std::nullopt)]()
             -> Expected<std::unique_ptr<Link>,
                         net::protocol::remote_play::StreamError> {
    return std::unique_ptr<Link>{std::make_unique<Wire>(script, stop, route)};
  };
}

auto ScriptedLink::Core::waker() -> std::function<bool()> {
  return [script = script_] {
    script->note("wake_console");
    return true;
  };
}

void ScriptedLink::Core::act(Action action) {
  if (auto* wire = script_->pumped_here()) {
    action(*wire);
    return;
  }
  script_->enqueue(std::move(action));
}

void ScriptedLink::Core::report(
    net::protocol::remote_play::StreamEvent reported) {
  act([held = std::make_shared<net::protocol::remote_play::StreamEvent>(
           std::move(reported))](Wire& wire) {
    wire.report(std::move(*held));
  });
}

void ScriptedLink::Core::report_launch_to(SessionEvents& events) noexcept {
  script_->report_launch_to(&events);
}

void ScriptedLink::Core::share_through(
    net::protocol::takion::SourceTap& tap,
    platform::CryptoProvider* crypto) noexcept {
  script_->share_through(&tap, crypto);
}

void ScriptedLink::Core::step_by_hand() { script_->step_by_hand(); }

void ScriptedLink::Core::settle() { script_->settle(); }

auto ScriptedLink::Core::turn() -> bool { return script_->turn(); }

void ScriptedLink::Core::launch_progress(LaunchProgress progress) {
  act([script = script_, progress](Wire&) {
    if (auto* events = script->launch_events())
      events->on_launch_progress(progress);
  });
}

void ScriptedLink::Core::offer_datacenters(DatacenterOffer offer) {
  act([script = script_,
       held = std::make_shared<DatacenterOffer>(std::move(offer))](Wire&) {
    if (auto* events = script->launch_events())
      events->on_datacenters_offered(*held);
  });
}

void ScriptedLink::Core::choose_datacenter(DatacenterChoice choice) {
  act([script = script_,
       held = std::make_shared<DatacenterChoice>(std::move(choice))](Wire&) {
    if (auto* events = script->launch_events())
      events->on_datacenter_chosen(*held);
  });
}

void ScriptedLink::Core::refuse_start(LinkFailure detail) {
  script_->refuse_start(std::move(detail));
}

void ScriptedLink::Core::refuse_seat_requests(
    net::protocol::remote_play::StreamError error) {
  script_->refuse_seat_requests(error);
}

void ScriptedLink::Core::refuse_login_pin(
    net::protocol::remote_play::StreamError error) {
  script_->refuse_login_pin(error);
}

void ScriptedLink::Core::report_protocol_version(std::uint32_t version) {
  script_->report_protocol_version(version);
}

auto ScriptedLink::Core::calls() const -> std::vector<std::string> {
  return script_->calls();
}

auto ScriptedLink::Core::controller_samples(controllers::SeatNumber seat) const
    -> std::vector<controllers::ControllerState> {
  return script_->controller_samples(seat);
}

auto ScriptedLink::Core::seat_requests() const -> std::vector<SeatRequest> {
  return script_->seat_requests();
}

auto ScriptedLink::Core::keyframe_requests() const -> std::size_t {
  return script_->keyframe_requests();
}

auto ScriptedLink::Core::login_pin() const -> SecretString {
  return script_->login_pin();
}

ScriptedLink::ScriptedLink(platform::ThreadingProvider& threading,
                           const platform::ClockProvider& clock)
    : core_(std::make_unique<Core>(*this, threading, clock)) {}

ScriptedLink::~ScriptedLink() = default;

void ScriptedLink::refuse_start(EndReason reason) {
  core_->refuse_start(LinkFailure::reported(reason));
}

void ScriptedLink::refuse_seat_requests(EndReason::Cause cause) {
  core_->refuse_seat_requests(LinkEvidence::error(cause));
}

void ScriptedLink::refuse_login_pin(EndReason::Cause cause) {
  core_->refuse_login_pin(LinkEvidence::error(cause));
}

void ScriptedLink::report_protocol_version(std::uint32_t version) {
  core_->report_protocol_version(version);
}

void ScriptedLink::step_by_hand() { core_->step_by_hand(); }

auto ScriptedLink::turn() -> bool { return core_->turn(); }

auto ScriptedLink::shared_media() -> SharedMedia {
  SharedMedia media;
  media.protocol_version = Core::Wire::shared_protocol_version;
  media.frames_per_second = Core::Wire::shared_frames_per_second;
  media.description.audio.channels = 2;
  media.description.audio.bits = 16;
  media.description.audio.rate = 48000;
  media.description.audio.frame_size = 960;
  media.description.audio.streams = 1;
  media.description.audio.coupled_streams = 1;
  media.description.audio.mapping = {0, 1};
  media.description.audio.positions = {
      media::AudioChannelPosition::front_left,
      media::AudioChannelPosition::front_right};
  media.description.profiles.push_back(
      media::VideoProfile{Core::Wire::shared_width, Core::Wire::shared_height,
                          Core::Wire::bytes(Core::Wire::shared_video_header)});
  static_cast<void>(text::decode_hex(Core::Wire::shared_audio_header,
                                     media.description.audio_header));
  media.description.picture = media::GrantedPicture{
      media::VideoCodec::h264, Core::Wire::shared_width,
      Core::Wire::shared_height, Core::Wire::shared_frames_per_second, 10000};
  for (const auto frame : Core::Wire::shared_video_frames)
    media.video_frames.push_back(Core::Wire::bytes(frame));
  for (const auto frame : Core::Wire::shared_audio_frames)
    media.audio_frames.push_back(Core::Wire::bytes(frame));
  return media;
}

void ScriptedLink::start_shared_stream() {
  core_->act([](Core::Wire& wire) { wire.start_shared_stream(); });
  describe(shared_media().description);
}

void ScriptedLink::send_shared_media() {
  core_->act([](Core::Wire& wire) { wire.send_shared_media(); });
}

void ScriptedLink::then(Step step) {
  core_->act([this, step = std::move(step)](Core::Wire&) { step(*this); });
}

void ScriptedLink::progress(ConnectStep step) {
  net::protocol::remote_play::FailureStage stage =
      net::protocol::remote_play::FailureStage::resolve;
  switch (step) {
    case ConnectStep::finding_console:
      stage = net::protocol::remote_play::FailureStage::resolve;
      break;
    case ConnectStep::opening_account_session:
      stage = net::protocol::remote_play::FailureStage::psn_open;
      break;
    case ConnectStep::waking_console:
      stage = net::protocol::remote_play::FailureStage::unknown;
      break;
    case ConnectStep::pairing:
      stage = net::protocol::remote_play::FailureStage::registration;
      break;
    case ConnectStep::reaching_console:
      stage = net::protocol::remote_play::FailureStage::control_holepunch;
      break;
    case ConnectStep::opening_stream_route:
      stage = net::protocol::remote_play::FailureStage::data_holepunch;
      break;
    case ConnectStep::requesting_session:
      stage = net::protocol::remote_play::FailureStage::session_init;
      break;
    case ConnectStep::opening_control:
      stage = net::protocol::remote_play::FailureStage::ctrl_startup;
      break;
    case ConnectStep::measuring_connection:
      stage = net::protocol::remote_play::FailureStage::calibration;
      break;
    case ConnectStep::starting_stream:
      stage = net::protocol::remote_play::FailureStage::active_connect;
      break;
  }
  core_->report(net::protocol::remote_play::event::SetupProgress{stage, 0, {}});
}

void ScriptedLink::route(progress::InternetRoute step) {
  using Stage = net::protocol::remote_play::FailureStage;
  using Route = progress::InternetRoute;
  Stage stage = step.purpose == Route::Purpose::stream
                    ? Stage::data_holepunch
                    : Stage::control_holepunch;
  switch (step.step) {
    case Route::Step::opening_account_session:
      stage = Stage::psn_open;
      break;
    case Route::Step::waiting_for_console:
      stage = Stage::psn_start;
      break;
    case Route::Step::switching_control:
      stage = Stage::route_switch;
      break;
    case Route::Step::gathering_candidates:
    case Route::Step::exchanging_offers:
    case Route::Step::testing_candidates:
    case Route::Step::route_chosen:
    case Route::Step::confirming_route:
      break;
  }
  core_->report(
      net::protocol::remote_play::event::SetupProgress{stage, 0, {}, step});
}

void ScriptedLink::launch_progress(LaunchProgress progress) {
  core_->launch_progress(progress);
}

void ScriptedLink::offer_datacenters(DatacenterOffer offer) {
  core_->offer_datacenters(std::move(offer));
}

void ScriptedLink::choose_datacenter(DatacenterChoice choice) {
  core_->choose_datacenter(std::move(choice));
}

void ScriptedLink::connect() {
  core_->act([](Core::Wire& wire) {
    wire.become(net::protocol::remote_play::StreamState::streaming);
  });
}

void ScriptedLink::describe(media::StreamDescription description) {
  net::protocol::remote_play::event::StreamInfoReady info;
  info.audio.channels = description.audio.channels;
  info.audio.bits = description.audio.bits;
  info.audio.rate = description.audio.rate;
  info.audio.frame_size = description.audio.frame_size;
  info.audio.streams = description.audio.streams;
  info.audio.coupled_streams = description.audio.coupled_streams;
  info.audio.mapping = description.audio.mapping;
  std::ranges::transform(
      description.audio.positions, info.audio.positions.begin(),
      [](media::AudioChannelPosition position) {
        return static_cast<net::protocol::remote_play::AudioChannelPosition>(
            position);
      });
  for (auto& profile : description.profiles)
    info.profiles.push_back(
        {profile.width, profile.height, std::move(profile.header)});
  if (description.picture)
    info.launch = net::protocol::remote_play::event::EffectiveLaunch{
        static_cast<net::protocol::remote_play::VideoCodec>(
            description.picture->codec),
        description.picture->width, description.picture->height,
        description.picture->max_fps, description.picture->bitrate_kbps};
  info.audio_header = description.audio_header;
  core_->report(std::move(info));
}

void ScriptedLink::show(media::VideoFrame frame) {
  net::protocol::remote_play::event::EncodedMedia media{
      net::protocol::remote_play::EncodedMediaKind::video,
      std::vector<std::byte>(frame.bytes().begin(), frame.bytes().end())};
  media.frame_index = frame.frame_index();
  media.codec = frame.codec();
  media.adaptive_stream_index = frame.adaptive_stream_index();
  media.frames_lost = frame.frames_lost();
  media.frame_recovered = frame.recovered();
  media.profile_header = frame.profile_header();
  media.dynamic_range_flag = frame.dynamic_range_flag();
  media.suppress_presentation = frame.suppress_presentation();
  core_->report(std::move(media));
}

void ScriptedLink::lose_video() {
  core_->report(net::protocol::remote_play::event::EncodedMedia{
      net::protocol::remote_play::EncodedMediaKind::video_fec_failure, {}});
}

void ScriptedLink::sound(media::AudioUnit unit) {
  using net::protocol::remote_play::EncodedMediaKind;
  const bool format = unit.kind == media::AudioUnit::Kind::format;
  net::protocol::remote_play::event::EncodedMedia media{
      unit.controller_speaker ? EncodedMediaKind::controller_speaker
      : unit.voice            ? (format ? EncodedMediaKind::voice_profile
                                        : EncodedMediaKind::voice_audio)
                              : (format ? EncodedMediaKind::audio_profile
                                        : EncodedMediaKind::audio),
      std::move(unit.bytes)};
  media.pad_index = unit.controller_speaker.value_or(0);
  media.audio.channels = unit.format.channels;
  media.audio.bits = unit.format.bits;
  media.audio.rate = unit.format.rate;
  media.audio.frame_size = unit.format.frame_size;
  media.audio.streams = unit.format.streams;
  media.audio.coupled_streams = unit.format.coupled_streams;
  media.audio.mapping = unit.format.mapping;
  std::ranges::transform(
      unit.format.positions, media.audio.positions.begin(),
      [](media::AudioChannelPosition position) {
        return static_cast<net::protocol::remote_play::AudioChannelPosition>(
            position);
      });
  media.audio_raw_pcm = unit.raw_pcm;
  core_->report(std::move(media));
}

void ScriptedLink::feed_back(controllers::ControllerFeedback feedback) {
  core_->report(MediaUnits::wire(feedback));
}

void ScriptedLink::ask_for_login_pin(bool previous_was_wrong) {
  core_->report(
      net::protocol::remote_play::event::PinRequired{previous_was_wrong});
}

void ScriptedLink::block_picture(bool blocked) {
  core_->report(net::protocol::remote_play::event::DisplayProhibited{blocked});
}

void ScriptedLink::open_keyboard(std::string_view text) {
  core_->report(
      net::protocol::remote_play::event::KeyboardOpen{std::string{text}});
}

void ScriptedLink::change_keyboard_text(std::string_view text) {
  core_->report(
      net::protocol::remote_play::event::KeyboardTextChange{std::string{text}});
}

void ScriptedLink::close_keyboard() {
  core_->report(net::protocol::remote_play::event::KeyboardRemoteClose{});
}

void ScriptedLink::ask_for_passcode(controllers::SeatNumber seat, bool retry) {
  core_->report(net::protocol::remote_play::event::PadPasscodeRequest{
      seat.index(), retry});
}

void ScriptedLink::join(controllers::SeatNumber seat,
                        controllers::SeatLight light) {
  core_->report(net::protocol::remote_play::event::PadAnnounced{
      seat.index(), {light.red, light.green, light.blue}});
}

void ScriptedLink::drop(controllers::SeatNumber seat) {
  core_->report(net::protocol::remote_play::event::PadDropped{seat.index()});
}

void ScriptedLink::refuse(SeatRefusal refusal) {
  const auto seat = refusal.seat();
  if (!seat) return;
  const auto join_failed = [&](std::uint8_t status) {
    core_->report(net::protocol::remote_play::event::PadJoinFailed{
        seat->index(), status});
  };
  switch (refusal.kind()) {
    case SeatRefusal::Kind::account_not_on_console:
      core_->report(net::protocol::remote_play::event::PadAccountUnresolvable{
          seat->index()});
      return;
    case SeatRefusal::Kind::refused_by_console:
      join_failed(*refusal.console_status());
      return;
    case SeatRefusal::Kind::passcode_attempts_used:
      join_failed(SeatRefusal::passcode_attempts_status);
      return;
    case SeatRefusal::Kind::timed_out:
      join_failed(SeatRefusal::timed_out_status);
      return;
    case SeatRefusal::Kind::not_sent:
    case SeatRefusal::Kind::no_free_seat:
      return;
  }
}

void ScriptedLink::begin_leaving() {
  core_->act([](Core::Wire& wire) {
    wire.become(net::protocol::remote_play::StreamState::stopping);
  });
}

void ScriptedLink::fail(EndReason reason) {
  core_->act([detail = LinkFailure::reported(reason)](Core::Wire& wire) {
    wire.fail(detail);
  });
}

void ScriptedLink::fail_cleanup(EndReason reason) {
  core_->act([detail = LinkFailure::reported(reason)](Core::Wire& wire) {
    wire.fail_cleanup(detail);
  });
}

void ScriptedLink::quit(ConsoleQuit quit) {
  core_->act(
      [reason = LinkEvidence::quit_reason(quit.reason()),
       text = quit.text()](Core::Wire& wire) { wire.quit(reason, text); });
}

void ScriptedLink::finish() {
  core_->act([](Core::Wire& wire) { wire.finish(); });
}

auto ScriptedLink::calls() const -> std::vector<std::string> {
  return core_->calls();
}

auto ScriptedLink::count(std::string_view call) const -> std::size_t {
  const auto all = core_->calls();
  return static_cast<std::size_t>(std::ranges::count(all, call));
}

auto ScriptedLink::controller_samples(controllers::SeatNumber seat) const
    -> std::vector<controllers::ControllerState> {
  return core_->controller_samples(seat);
}

auto ScriptedLink::seat_requests() const -> std::vector<SeatRequest> {
  return core_->seat_requests();
}

auto ScriptedLink::keyframe_requests() const -> std::size_t {
  return core_->keyframe_requests();
}

auto ScriptedLink::login_pin() const -> SecretString {
  return core_->login_pin();
}

}  // namespace libchiaking::play::testing
