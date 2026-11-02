#include "libchiaking/detail/kakusan/kakusan.hpp"

#include "src/core/kakusan/adapters/guest_visit/guest_visit_core.hpp"
#include "src/core/kakusan/adapters/protocol_refusal.hpp"
#include "src/core/kakusan/kakusan_core.hpp"
#include "src/core/kakusan/usecases/host.hpp"
#include "src/core/kakusan/usecases/join.hpp"

#include <utility>

namespace libchiaking::kakusan {

Kakusan::Kakusan(std::shared_ptr<Surroundings> surroundings) noexcept
    : surroundings_(std::move(surroundings)) {}

auto Kakusan::create(platform::PlatformFactory& platform, DeviceStore& store,
                     std::span<const std::string> stun_servers)
    -> Expected<Kakusan, Unavailable> {
  auto surroundings = Surroundings::of(platform, store, stun_servers);
  if (!surroundings) return unexpected(surroundings.error());
  return Kakusan{std::move(surroundings).value()};
}

auto Kakusan::on(std::shared_ptr<Surroundings> surroundings) noexcept
    -> Kakusan {
  return Kakusan{std::move(surroundings)};
}

auto Kakusan::use_stun_servers(std::span<const std::string> stun_servers)
    -> Expected<void, Unavailable> {
  return surroundings_->use_stun_servers(stun_servers);
}

void Kakusan::use_port_guessing(PortGuessing guessing) {
  surroundings_->use_port_guessing(guessing);
}

auto Kakusan::this_device() const noexcept -> const DeviceIdentity& {
  return surroundings_->identity();
}

auto Kakusan::remembered() noexcept -> RememberedDevices& {
  return surroundings_->remembered();
}

auto Kakusan::share(play::SessionShare& session, const SharingRules& rules,
                    SharedSessionObserver& observer,
                    SharedSession::Pacing pacing)
    -> Expected<std::unique_ptr<SharedSession>, Refusal> {
  return usecases::Host::open(surroundings_, session, rules, observer, pacing);
}

auto Kakusan::read(std::string_view pasted) const
    -> Expected<Invitation, Refusal> {
  if (pasted.empty())
    return unexpected(Refusal{Refusal::Kind::invalid_argument});
  if (pasted.size() > Invitation::most_text)
    return unexpected(Refusal{Refusal::Kind::invalid_text});
  auto summary = GuestVisit::Core::preview(surroundings_->crypto(), pasted,
                                           surroundings_->now().wall_seconds);
  if (!summary) return unexpected(ProtocolRefusal::of(summary.error()));
  summary.value().recognition =
      surroundings_->remembered().recognise(summary.value().host_hint);
  return Invitation{SecretString{pasted}, std::move(summary).value()};
}

auto Kakusan::join(Invitation invitation, GuestVisit::Ports ports,
                   GuestVisit::Pacing pacing)
    -> Expected<std::unique_ptr<GuestVisit>, Refusal> {
  return usecases::Join::open(surroundings_, invitation, ports, pacing);
}

}  // namespace libchiaking::kakusan
