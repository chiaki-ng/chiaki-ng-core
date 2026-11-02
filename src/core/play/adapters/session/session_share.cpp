#include <libchiaking/platform/runtime/threading.hpp>
#include "src/core/play/adapters/session/session_share_core.hpp"

#include "src/core/net/protocol/remote_play/stream/forwarded_source.hpp"
#include "src/core/play/adapters/media/media_units.hpp"
#include "src/core/play/adapters/session/session_core.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace libchiaking::play {

static_assert(ReceiveKeys::size == net::protocol::takion::source_key_size);

SessionShare::Core::Deliveries::Turn::Turn(Deliveries& deliveries) noexcept
    : deliveries_(deliveries),
      outermost_(deliveries.thread_.load() !=
                 platform::ThreadIdentity::current()) {
  if (outermost_)
    deliveries_.thread_.store(platform::ThreadIdentity::current());
}

SessionShare::Core::Deliveries::Turn::~Turn() {
  if (!outermost_) return;
  deliveries_.thread_.store(platform::ThreadIdentity{});
  if (deliveries_.settling_.load() == 0) return;
  {
    const platform::Lock lock(deliveries_.mutex_);
  }
  deliveries_.finished_.notify_all();
}

void SessionShare::Core::Deliveries::settle() const noexcept {
  const auto here = platform::ThreadIdentity::current();
  const auto settled = [this, here] {
    const auto busy = thread_.load();
    return !busy || busy == here;
  };
  if (settled()) return;
  settling_.fetch_add(1);
  {
    platform::Lock lock(mutex_);
    finished_.wait(lock, settled);
  }
  settling_.fetch_sub(1);
}

auto SessionShare::Core::create(platform::ThreadingProvider& threading,
                                const platform::ClockProvider& clock,
                                Source source) -> std::shared_ptr<Core> {
  return std::shared_ptr<Core>{new Core{threading, clock, source}};
}

SessionShare::Core::Core(platform::ThreadingProvider& threading,
                         const platform::ClockProvider& clock, Source source)
    : where_(source.where),
      requested_fps_(source.requested_fps),
      face_(*this),
      deliveries_(threading, clock),
      operations_(threading),
      mutex_(threading) {}

SessionShare::Core::~Core() = default;

auto SessionShare::Core::shared(
    const net::protocol::takion::SourceDatagram& wired) noexcept
    -> SharedDatagram {
  SharedDatagram shared;
  shared.media = wired.media == net::protocol::takion::SourceMedia::video
                     ? SharedDatagram::Media::video
                     : SharedDatagram::Media::audio;
  shared.key_position = wired.key_position;
  shared.bytes = wired.bytes;
  shared.sealed_size = wired.sealed_size;
  shared.unit_index = wired.unit_index;
  shared.unit_total = wired.unit_total;
  shared.unit_redundant = wired.unit_redundant;
  shared.unit_size = wired.unit_size;
  shared.codec = wired.codec;
  shared.stream_index = wired.stream_index;
  shared.auxiliary = wired.auxiliary;
  return shared;
}

auto SessionShare::Core::copied(
    const net::protocol::takion::SourceReceiveKeys& wired) noexcept
    -> ReceiveKeys {
  SecretArray<ReceiveKeys::size> key;
  SecretArray<ReceiveKeys::size> iv;
  std::ranges::copy(wired.key(), key.begin());
  std::ranges::copy(wired.iv(), iv.begin());
  return ReceiveKeys{std::move(key), std::move(iv)};
}

auto SessionShare::Core::watch(media::MediaSink& next) noexcept
    -> media::MediaSink& {
  next_ = &next;
  return *this;
}

void SessionShare::Core::bind(Session::Core& session) {
  const platform::Lock operations(operations_);
  session_ = &session;
}

void SessionShare::Core::close() {
  {
    const platform::Lock operations(operations_);
    session_ = nullptr;
  }
  session_over();
}

void SessionShare::Core::on_source_keys(
    std::uint32_t takion_version,
    const net::protocol::takion::SourceReceiveKeys& keys) noexcept {
  const platform::Lock lock(mutex_);
  if (over_) return;
  version_ = takion_version;
  keys_.emplace(copied(keys));
  stream_generation_.fetch_add(1);
}

auto SessionShare::Core::told(const Registration& listener) const noexcept
    -> bool {
  return listener.ticket == told_ticket_.load() &&
         stream_generation_.load() == told_generation_.load();
}

void SessionShare::Core::on_source_datagram(
    const net::protocol::takion::SourceDatagram& datagram) noexcept {
  const Deliveries::Turn turn{deliveries_};
  const auto* listener = listener_.load();
  if (listener == nullptr || !told(*listener)) return;
  listener->audience.encrypted_datagram(shared(datagram));
}

auto SessionShare::Core::announce(const Registration& listener) -> bool {
  std::optional<SharedStream> stream;
  std::optional<ReceiveKeys> keys;
  std::uint64_t generation = 0;
  {
    const platform::Lock lock(mutex_);
    if (!keys_ || !version_ || !description_ ||
        !SessionShare::shares_protocol_version(*version_))
      return false;
    generation = stream_generation_.load();
    const auto& picture = description_->picture;
    stream = SharedStream{
        *version_, where_, *description_,
        picture && picture->codec != media::VideoCodec::h264,
        picture && picture->max_fps != 0 ? picture->max_fps : requested_fps_};
    keys.emplace(keys_->clone());
  }
  told_ticket_.store(listener.ticket);
  told_generation_.store(generation);
  listener.audience.stream_started(*stream, std::move(*keys));
  listener.audience.feedback(controllers::ControllerFeedback{
      controllers::SeatNumber::first(),
      controllers::HapticIntensity{haptic_intensity_.load()}});
  return true;
}

void SessionShare::Core::on_stream_description(
    const media::StreamDescription& told) {
  next_->on_stream_description(told);
  {
    auto description = told;
    const platform::Lock lock(mutex_);
    if (over_) return;
    description_ = std::move(description);
    stream_generation_.fetch_add(1);
  }
  const Deliveries::Turn turn{deliveries_};
  if (const auto* listener = listener_.load()) announce(*listener);
}

auto SessionShare::Core::on_video(const media::VideoUnit& unit)
    -> media::VideoResult {
  return next_->on_video(unit);
}

void SessionShare::Core::on_video_gap() { next_->on_video_gap(); }

auto SessionShare::Core::on_audio(const media::AudioUnit& unit)
    -> media::AudioResult {
  return next_->on_audio(unit);
}

void SessionShare::Core::on_feedback(
    const controllers::ControllerFeedback& feedback) {
  next_->on_feedback(feedback);
  const auto* intensity = feedback.get<controllers::HapticIntensity>();
  if (intensity != nullptr) haptic_intensity_.store(intensity->level);
  if (intensity == nullptr && held_seats_.load() == 0) return;
  const Deliveries::Turn turn{deliveries_};
  const auto* listener = listener_.load();
  if (listener == nullptr) return;
  listener->audience.feedback(feedback);
}

auto SessionShare::Core::service(std::chrono::milliseconds wait)
    -> std::chrono::milliseconds {
  const Deliveries::Turn turn{deliveries_};
  const auto* listener = listener_.load();
  if (listener == nullptr) return wait;
  if (!told(*listener) && !announce(*listener)) refuse(*listener);
  if (listener_.load() != listener) return wait;
  return listener->audience.service(wait);
}

void SessionShare::Core::refuse(const Registration& listener) {
  {
    const platform::Lock lock(mutex_);
    if (!version_ || SessionShare::shares_protocol_version(*version_) ||
        refused_ticket_ == listener.ticket)
      return;
    refused_ticket_ = listener.ticket;
  }
  listener.audience.became_unshareable(NotShareable::protocol_version);
}

void SessionShare::Core::session_over() {
  const Deliveries::Turn turn{deliveries_};
  std::unique_ptr<Registration> last;
  {
    const platform::Lock lock(mutex_);
    over_ = true;
    keys_.reset();
    description_.reset();
    guests_ = {};
    held_seats_.store(0);
    listener_.store(nullptr);
    last = std::move(registration_);
  }
  if (last) last->audience.session_ended();
}

void SessionShare::Core::seat_joined(controllers::SeatNumber seat) {
  {
    const platform::Lock lock(mutex_);
    if (!guests_[seat.index()]) return;
  }
  const Deliveries::Turn turn{deliveries_};
  if (const auto* listener = listener_.load())
    listener->audience.guest_seated(seat);
}

void SessionShare::Core::seat_refused(const SeatRefusal& refusal) {
  const auto seat = refusal.seat();
  if (!seat) return;
  {
    const platform::Lock lock(mutex_);
    auto& guest = guests_[seat->index()];
    if (!guest) return;
    guest.reset();
    held_seats_.fetch_sub(1);
  }
  const Deliveries::Turn turn{deliveries_};
  if (const auto* listener = listener_.load())
    listener->audience.guest_seat_refused(refusal);
}

void SessionShare::Core::seat_left(controllers::SeatNumber seat) {
  {
    const platform::Lock lock(mutex_);
    auto& guest = guests_[seat.index()];
    if (!guest) return;
    guest.reset();
    held_seats_.fetch_sub(1);
  }
  const Deliveries::Turn turn{deliveries_};
  if (const auto* listener = listener_.load())
    listener->audience.guest_unseated(seat);
}

auto SessionShare::Core::verdict() const -> Expected<void, NotShareable> {
  if (where_ == Where::cloud) return unexpected(NotShareable::cloud_session);
  std::optional<std::uint32_t> version;
  {
    const platform::Lock lock(mutex_);
    if (over_) return unexpected(NotShareable::session_ended);
    version = version_;
  }
  if (session_ == nullptr) return unexpected(NotShareable::session_ended);
  if (!version) version = session_->statistics().protocol_version;
  if (version && !SessionShare::shares_protocol_version(*version))
    return unexpected(NotShareable::protocol_version);
  return {};
}

auto SessionShare::Core::can_be_shared() const -> Expected<void, NotShareable> {
  const platform::Lock operations(operations_);
  return verdict();
}

auto SessionShare::Core::begin(SharedAudience& audience)
    -> Expected<Sharing, NotShareable> {
  const platform::Lock operations(operations_);
  if (const auto shareable = verdict(); !shareable)
    return unexpected(shareable.error());
  std::uint64_t ticket = 0;
  {
    const platform::Lock lock(mutex_);
    if (over_) return unexpected(NotShareable::session_ended);
    if (registration_) return unexpected(NotShareable::already_shared);
    ticket = ++tickets_;
    registration_ =
        std::make_unique<Registration>(Registration{audience, ticket});
    listener_.store(registration_.get());
  }
  session_->wake();
  return Sharing{std::make_unique<Sharing::Core>(weak_from_this(), ticket)};
}

void SessionShare::Core::stop(std::uint64_t ticket) {
  std::unique_ptr<Registration> gone;
  {
    const platform::Lock operations(operations_);
    std::vector<controllers::SeatNumber> held;
    {
      const platform::Lock lock(mutex_);
      if (registration_ && registration_->ticket == ticket) {
        listener_.store(nullptr);
        gone = std::move(registration_);
        for (std::uint8_t index = 0; index < guests_.size(); ++index)
          if (std::exchange(guests_[index], std::nullopt))
            held.push_back(controllers::SeatNumber::masked(index));
        held_seats_.store(0);
      }
    }
    if (session_ != nullptr)
      for (const auto seat : held) session_->seat_rules().release_guest(seat);
  }
  deliveries_.settle();
}

auto SessionShare::Core::reserve(controllers::SeatNumber seat,
                                 const GuestSeat& request)
    -> std::optional<std::uint64_t> {
  const platform::Lock lock(mutex_);
  auto& guest = guests_[seat.index()];
  if (over_ || !registration_ || guest) return std::nullopt;
  guest = Guest{request.guest, ++tickets_};
  held_seats_.fetch_add(1);
  return guest->ticket;
}

void SessionShare::Core::forget(controllers::SeatNumber seat,
                                std::uint64_t ticket) {
  const platform::Lock lock(mutex_);
  auto& guest = guests_[seat.index()];
  if (!guest || guest->ticket != ticket) return;
  guest.reset();
  held_seats_.fetch_sub(1);
}

auto SessionShare::Core::seat_guest(const GuestSeat& request)
    -> Expected<SharedSeat, SeatRefusal> {
  const platform::Lock operations(operations_);
  const auto first =
      request.preferred.value_or(controllers::SeatNumber::masked(1));
  {
    const platform::Lock lock(mutex_);
    if (over_ || !registration_ || session_ == nullptr)
      return unexpected(
          SeatRefusal::not_sent(first, EndReason::Cause::invalid_state));
  }
  const auto last =
      request.preferred
          ? first.index()
          : static_cast<std::uint8_t>(controllers::SeatNumber::count - 1);
  for (auto index = first.index(); index <= last; ++index) {
    const auto seat = controllers::SeatNumber::masked(index);
    if (!seat.is_added() || !session_->seat_rules().seat(seat).is_empty())
      continue;
    const auto ticket = reserve(seat, request);
    if (!ticket) continue;
    const auto seated = session_->seat_rules().seat_guest(
        seat, request.guest, request.controller, request.as);
    if (seated)
      return SharedSeat{std::make_unique<SharedSeat::Core>(
          weak_from_this(), seat, request.guest, *ticket)};
    forget(seat, *ticket);
    if (seated.error().kind() != SeatRefusal::Kind::no_free_seat)
      return unexpected(seated.error());
  }
  return unexpected(SeatRefusal::no_free_seat());
}

auto SessionShare::Core::send(controllers::SeatNumber seat,
                              std::uint64_t ticket,
                              const controllers::ControllerState& state)
    -> Expected<void, InputRefusal> {
  const platform::Lock operations(operations_);
  {
    const platform::Lock lock(mutex_);
    if (over_ || session_ == nullptr)
      return unexpected(InputRefusal::not_running);
    const auto& guest = guests_[seat.index()];
    if (!guest || guest->ticket != ticket)
      return unexpected(InputRefusal::not_owner);
  }
  return session_->controller_rules().forward(seat, state);
}

void SessionShare::Core::release(controllers::SeatNumber seat,
                                 std::uint64_t ticket) {
  const platform::Lock operations(operations_);
  {
    const platform::Lock lock(mutex_);
    auto& guest = guests_[seat.index()];
    if (!guest || guest->ticket != ticket) return;
    guest.reset();
    held_seats_.fetch_sub(1);
  }
  if (session_ != nullptr) session_->seat_rules().release_guest(seat);
}

void SessionShare::Core::request_keyframe() {
  const platform::Lock operations(operations_);
  if (session_ != nullptr) static_cast<void>(session_->request_keyframe());
}

void SessionShare::Core::wake() noexcept {
  const platform::Lock operations(operations_);
  if (session_ != nullptr) session_->wake();
}

Sharing::Core::Core(std::weak_ptr<SessionShare::Core> share,
                    std::uint64_t ticket) noexcept
    : share_(std::move(share)), ticket_(ticket) {}

Sharing::Core::~Core() { stop(); }

void Sharing::Core::stop() noexcept {
  if (const auto share = std::exchange(share_, {}).lock()) share->stop(ticket_);
}

SharedSeat::Core::Core(std::weak_ptr<SessionShare::Core> share,
                       controllers::SeatNumber seat, GuestTag guest,
                       std::uint64_t ticket) noexcept
    : share_(std::move(share)), seat_(seat), guest_(guest), ticket_(ticket) {}

SharedSeat::Core::~Core() {
  if (const auto share = share_.lock()) share->release(seat_, ticket_);
}

auto SharedSeat::Core::send(const controllers::ControllerState& state)
    -> Expected<void, InputRefusal> {
  if (const auto share = share_.lock())
    return share->send(seat_, ticket_, state);
  return unexpected(InputRefusal::not_running);
}

Sharing::Sharing(std::unique_ptr<Core> core) noexcept
    : core_(std::move(core)) {}

Sharing::Sharing(Sharing&& other) noexcept = default;

auto Sharing::operator=(Sharing&& other) noexcept -> Sharing& = default;

Sharing::~Sharing() = default;

void Sharing::stop() noexcept {
  if (core_) core_->stop();
}

SharedSeat::SharedSeat(std::unique_ptr<Core> core) noexcept
    : core_(std::move(core)) {}

SharedSeat::SharedSeat(SharedSeat&& other) noexcept = default;

auto SharedSeat::operator=(SharedSeat&& other) noexcept
    -> SharedSeat& = default;

SharedSeat::~SharedSeat() = default;

auto SharedSeat::number() const noexcept -> controllers::SeatNumber {
  return core_->number();
}

auto SharedSeat::guest() const noexcept -> GuestTag { return core_->guest(); }

auto SharedSeat::send(const controllers::ControllerState& state)
    -> Expected<void, InputRefusal> {
  return core_->send(state);
}

auto SessionShare::shares_protocol_version(
    std::uint32_t protocol_version) noexcept -> bool {
  return net::protocol::remote_play::ForwardedSourceReceiver::
      forwardable_takion_version(protocol_version);
}

auto SessionShare::can_be_shared() const -> Expected<void, NotShareable> {
  return core_->can_be_shared();
}

auto SessionShare::begin(SharedAudience& audience)
    -> Expected<Sharing, NotShareable> {
  return core_->begin(audience);
}

auto SessionShare::seat_guest(const GuestSeat& request)
    -> Expected<SharedSeat, SeatRefusal> {
  return core_->seat_guest(request);
}

void SessionShare::request_keyframe() { core_->request_keyframe(); }

void SessionShare::wake() noexcept { core_->wake(); }

}  // namespace libchiaking::play
