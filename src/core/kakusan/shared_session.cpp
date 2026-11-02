#include "src/core/kakusan/shared_session_hosting.hpp"

#include <algorithm>

namespace libchiaking::kakusan {

SharedSession::Hosting::Hosting(std::shared_ptr<Surroundings> surroundings,
                                std::unique_ptr<Connections> connections,
                                std::unique_ptr<Core> machine,
                                Observer& observer, Pacing pacing)
    : surroundings_(std::move(surroundings)),
      connections_(std::move(connections)),
      pacing_(pacing),
      gate_(surroundings_->threading()),
      mutex_(surroundings_->threading()),
      machine_(std::move(machine)),
      host_(*this, *surroundings_, *machine_, observer, pacing) {}

SharedSession::Hosting::~Hosting() {
  host_.stop_sharing();
  const platform::Lock turn{gate_.mutex()};
  const platform::Lock lock{mutex_};
  queued_.clear();
}

void SharedSession::Hosting::began(play::Sharing sharing,
                                   play::SessionShare& session) {
  sharing_.emplace(std::move(sharing));
  session_ = &session;
}

void SharedSession::Hosting::never_began() noexcept {
  tap(nullptr);
  live_ = false;
  stopped_ = true;
}

auto SharedSession::Hosting::leave_session() -> std::vector<Seated> {
  session_ = nullptr;
  return std::exchange(seated_, {});
}

auto SharedSession::Hosting::close() -> std::optional<Closed> {
  if (std::exchange(stopped_, true)) return std::nullopt;
  Closed closed;
  closed.streaming = live_ && streaming_;
  live_ = false;
  closed.sharing = std::exchange(sharing_, std::nullopt);
  closed.seated = std::exchange(seated_, {});
  session_ = nullptr;
  tracked_.clear();
  tracked_overflow_ = false;
  return closed;
}

auto SharedSession::Hosting::take_queued() -> std::deque<Step> {
  std::deque<Step> ready;
  ready.swap(queued_);
  return ready;
}

auto SharedSession::Hosting::row(const GuestId& guest) -> Tracked* {
  const auto found =
      std::ranges::find_if(tracked_, [&guest](const Tracked& tracked) {
        return tracked.player.guest == guest;
      });
  return found == tracked_.end() ? nullptr : &*found;
}

void SharedSession::Hosting::seat_as(const GuestId& guest,
                                     const GuestSeating& seating) {
  if (auto* known = row(guest))
    known->seating = seating;
  else if (tracked_.size() < pacing_.tracked_players)
    tracked_.push_back(Tracked{PlayerSummary{guest}, seating});
}

void SharedSession::Hosting::identified(const GuestId& guest,
                                        const IdentityKey& device,
                                        const Fingerprint& fingerprint) {
  if (row(guest) == nullptr && tracked_.size() < pacing_.tracked_players)
    tracked_.push_back(Tracked{PlayerSummary{guest}, {}});
  if (auto* known = row(guest)) {
    known->player.device = device;
    known->player.fingerprint = fingerprint;
  }
}

auto SharedSession::Hosting::changed(PlayerSummary player) -> PlayerSummary {
  if (auto* known = row(player.guest)) {
    player.device = known->player.device;
    player.fingerprint = known->player.fingerprint;
    known->player = player;
  } else if (player.progress != Progress::gone) {
    if (tracked_.size() < pacing_.tracked_players)
      tracked_.push_back(Tracked{player, {}});
    else
      tracked_overflow_ = true;
  }
  if (player.progress == Progress::gone)
    std::erase_if(tracked_, [&player](const Tracked& tracked) {
      return tracked.player.guest == player.guest;
    });
  return shown(std::move(player));
}

auto SharedSession::Hosting::shown(PlayerSummary player) const
    -> PlayerSummary {
  const auto found = std::ranges::find(seated_, player.guest, &Seated::guest);
  if (found == seated_.end()) return player;
  if (found->joined) {
    player.console_seat = found->console.number();
  } else if (player.progress != Progress::gone) {
    player.reserved_seat = found->console.number();
    if (!player.grant) player.grant = found->grant;
  }
  return player;
}

auto SharedSession::Hosting::players() const -> std::vector<PlayerSummary> {
  std::vector<PlayerSummary> players;
  players.reserve(tracked_.size());
  for (const auto& tracked : tracked_) players.push_back(shown(tracked.player));
  return players;
}

auto SharedSession::Hosting::guests_admitted() const noexcept -> bool {
  return tracked_overflow_ ||
         std::ranges::any_of(tracked_, [](const Tracked& tracked) {
           return admitted(tracked.player.progress);
         });
}

auto SharedSession::Hosting::guests_forwarding() const noexcept -> bool {
  return tracked_overflow_ ||
         std::ranges::any_of(tracked_, [](const Tracked& tracked) {
           return forwarding(tracked.player.progress);
         });
}

auto SharedSession::Hosting::verified_device(const GuestId& guest)
    -> Expected<IdentityKey, RememberRefusal> {
  const auto* known = row(guest);
  if (known == nullptr)
    return unexpected(
        RememberRefusal{RememberRefusal::Kind::unknown_device, {}});
  if (!known->player.device || !admitted(known->player.progress))
    return unexpected(RememberRefusal{RememberRefusal::Kind::not_verified, {}});
  return *known->player.device;
}

auto SharedSession::Hosting::invitation(Core::InvitationReady& ready) const
    -> Invitation {
  return Invitation{std::move(ready.block),
                    InvitationSummary{ready.host_hint,
                                      ready.seats,
                                      ready.expires_at,
                                      ready.source_protocol,
                                      {}}};
}

void SharedSession::Hosting::seen(const IdentityKey& device) const {
  surroundings_->remembered().seen(device, surroundings_->wall());
}

void SharedSession::Hosting::reserve(const GuestId& guest,
                                     const SeatGrant& grant,
                                     play::SharedSeat console) {
  seated_.push_back(Seated{guest, grant, std::move(console), false});
}

auto SharedSession::Hosting::awaiting(controllers::SeatNumber seat) -> Seated* {
  const auto found =
      std::ranges::find_if(seated_, [seat](const Seated& candidate) {
        return !candidate.joined && candidate.console.number() == seat;
      });
  return found == seated_.end() ? nullptr : &*found;
}

auto SharedSession::Hosting::seat_of(const GuestId& guest) -> Seated* {
  const auto found = std::ranges::find(seated_, guest, &Seated::guest);
  return found == seated_.end() ? nullptr : &*found;
}

auto SharedSession::Hosting::taken(std::vector<Seated>::iterator found)
    -> std::optional<Seated> {
  if (found == seated_.end()) return std::nullopt;
  std::optional<Seated> gone{std::move(*found)};
  seated_.erase(found);
  return gone;
}

auto SharedSession::Hosting::take_awaiting(controllers::SeatNumber seat)
    -> std::optional<Seated> {
  return taken(std::ranges::find_if(seated_, [seat](const Seated& candidate) {
    return !candidate.joined && candidate.console.number() == seat;
  }));
}

auto SharedSession::Hosting::take(controllers::SeatNumber seat)
    -> std::optional<Seated> {
  return taken(std::ranges::find_if(seated_, [seat](const Seated& candidate) {
    return candidate.console.number() == seat;
  }));
}

auto SharedSession::Hosting::take(const GuestId& guest)
    -> std::optional<Seated> {
  return taken(std::ranges::find(seated_, guest, &Seated::guest));
}

SharedSession::SharedSession(std::unique_ptr<Hosting> hosting) noexcept
    : hosting_(std::move(hosting)) {}

SharedSession::~SharedSession() = default;

auto SharedSession::invite_player() -> Expected<GuestId, Refusal> {
  return hosting_->host().invite_player();
}

auto SharedSession::accept_reply(std::string_view pasted)
    -> Expected<GuestId, Refusal> {
  return hosting_->host().accept_reply(pasted);
}

auto SharedSession::verify(const GuestId& guest, const VerificationCode& theirs)
    -> Expected<void, Refusal> {
  return hosting_->host().verify(guest, theirs);
}

auto SharedSession::give_seat(const GuestId& guest, GuestSeating seating)
    -> Expected<void, Refusal> {
  return hosting_->host().give_seat(guest, seating);
}

auto SharedSession::take_back_seat(const GuestId& guest)
    -> Expected<void, Refusal> {
  return hosting_->host().take_back_seat(guest);
}

auto SharedSession::remember(const GuestId& guest, std::string_view nickname,
                             Replace replace)
    -> Expected<RememberedDevice, RememberRefusal> {
  std::optional<IdentityKey> device;
  {
    const platform::Lock lock{hosting_->mutex()};
    const auto verified = hosting_->verified_device(guest);
    if (!verified) return unexpected(verified.error());
    device = verified.value();
  }
  auto& surroundings = hosting_->surroundings();
  return surroundings.remembered().remember(*device, nickname, replace,
                                            surroundings.wall());
}

auto SharedSession::change(const SharingRules& rules)
    -> Expected<void, Refusal> {
  return hosting_->host().change(rules);
}

auto SharedSession::tune(const Tuning& tuning) -> Expected<void, Refusal> {
  return hosting_->host().tune(tuning);
}

auto SharedSession::players() const -> std::vector<PlayerSummary> {
  const platform::Lock lock{hosting_->mutex()};
  return hosting_->players();
}

auto SharedSession::seats() const -> SeatSummary {
  const platform::Lock lock{hosting_->mutex()};
  return hosting_->machine().seats();
}

void SharedSession::stop_sharing() { hosting_->host().stop_sharing(); }

}  // namespace libchiaking::kakusan
