#include "libchiaking/detail/catalog/launch_verdict.hpp"

#include <type_traits>
#include <utility>
#include <variant>

namespace libchiaking::catalog {

static_assert(std::variant_size_v<LaunchRefusal::Reason> ==
              std::to_underlying(LaunchRefusal::Kind::choose_a_game) + 1);
static_assert(
    std::is_same_v<LaunchRefusal::Hidden,
                   std::variant_alternative_t<
                       std::to_underlying(LaunchRefusal::Kind::hidden),
                       LaunchRefusal::Reason>>);
static_assert(
    std::is_same_v<LaunchRefusal::NotReleased,
                   std::variant_alternative_t<
                       std::to_underlying(LaunchRefusal::Kind::not_released),
                       LaunchRefusal::Reason>>);

auto LaunchRefusal::not_listed() -> LaunchRefusal {
  return LaunchRefusal{NotListed{}};
}

auto LaunchRefusal::hidden(HideReason reason) -> LaunchRefusal {
  return LaunchRefusal{Hidden{reason}};
}

auto LaunchRefusal::buy_to_stream() -> LaunchRefusal {
  return LaunchRefusal{BuyToStream{}};
}

auto LaunchRefusal::add_to_library(
    std::optional<StorefrontAnswer::LibraryOffer> offer) -> LaunchRefusal {
  return LaunchRefusal{AddToLibrary{std::move(offer)}};
}

auto LaunchRefusal::needs_premium(std::optional<users::Tier> held)
    -> LaunchRefusal {
  return LaunchRefusal{NeedsPremium{held}};
}

auto LaunchRefusal::refused_by_service(Time when) -> LaunchRefusal {
  return LaunchRefusal{RefusedByService{when}};
}

auto LaunchRefusal::not_streamable(std::string notice) -> LaunchRefusal {
  return LaunchRefusal{NotStreamable{std::move(notice)}};
}

auto LaunchRefusal::unannounced() -> LaunchRefusal {
  return LaunchRefusal{Unannounced{}};
}

auto LaunchRefusal::not_released(Time at) -> LaunchRefusal {
  return LaunchRefusal{NotReleased{at}};
}

auto LaunchRefusal::no_trusted_clock() -> LaunchRefusal {
  return LaunchRefusal{NoTrustedClock{}};
}

auto LaunchRefusal::check_unavailable(LaunchCheckFailure cause)
    -> LaunchRefusal {
  return LaunchRefusal{CheckUnavailable{std::move(cause)}};
}

auto LaunchRefusal::invalid_title() -> LaunchRefusal {
  return LaunchRefusal{InvalidTitle{}};
}

auto LaunchRefusal::title_taken_down() -> LaunchRefusal {
  return LaunchRefusal{TitleTakenDown{}};
}

auto LaunchRefusal::streaming_unavailable() -> LaunchRefusal {
  return LaunchRefusal{StreamingUnavailable{}};
}

auto LaunchRefusal::purchase_needed() -> LaunchRefusal {
  return LaunchRefusal{PurchaseNeeded{}};
}

auto LaunchRefusal::unexpected_entitlement_state() -> LaunchRefusal {
  return LaunchRefusal{UnexpectedEntitlementState{}};
}

auto LaunchRefusal::choose_a_game() -> LaunchRefusal {
  return LaunchRefusal{ChooseAGame{}};
}

auto LaunchRefusal::hide_reason() const noexcept -> std::optional<HideReason> {
  if (const auto* hidden = std::get_if<Hidden>(&reason_)) return hidden->reason;
  return std::nullopt;
}

auto LaunchRefusal::library_offer() const
    -> std::optional<StorefrontAnswer::LibraryOffer> {
  if (const auto* add = std::get_if<AddToLibrary>(&reason_)) return add->offer;
  return std::nullopt;
}

auto LaunchRefusal::tier_held() const noexcept -> std::optional<users::Tier> {
  if (const auto* premium = std::get_if<NeedsPremium>(&reason_))
    return premium->held;
  return std::nullopt;
}

auto LaunchRefusal::time() const noexcept -> std::optional<Time> {
  if (const auto* refused = std::get_if<RefusedByService>(&reason_))
    return refused->when;
  if (const auto* ahead = std::get_if<NotReleased>(&reason_)) return ahead->at;
  return std::nullopt;
}

auto LaunchRefusal::notice() const noexcept -> std::string_view {
  if (const auto* unstreamable = std::get_if<NotStreamable>(&reason_))
    return unstreamable->notice;
  return {};
}

auto LaunchRefusal::cause() const -> std::optional<LaunchCheckFailure> {
  if (const auto* unchecked = std::get_if<CheckUnavailable>(&reason_))
    return unchecked->cause;
  return std::nullopt;
}

auto LaunchRefusal::asks_for_ownership_check() const noexcept -> bool {
  const auto* unchecked = std::get_if<CheckUnavailable>(&reason_);
  return unchecked != nullptr &&
         unchecked->cause.reason() == LaunchCheckReason::ownership_unknown;
}

auto LaunchRefusal::ownership_unknown() -> LaunchRefusal {
  return check_unavailable(
      LaunchCheckFailure{LaunchCheckReason::ownership_unknown});
}

auto LaunchRefusal::of_release(ReleaseDate release,
                               std::optional<Time> trusted_now)
    -> std::optional<LaunchRefusal> {
  switch (release.state(trusted_now)) {
    case ReleaseDate::State::released:
      break;
    case ReleaseDate::State::unannounced:
      return unannounced();
    case ReleaseDate::State::no_trusted_clock:
      return no_trusted_clock();
    case ReleaseDate::State::not_released:
      return not_released(*release.time());
  }
  return std::nullopt;
}

LaunchVerdict::LaunchVerdict(Outcome outcome) noexcept
    : outcome_(std::move(outcome)) {}

auto LaunchVerdict::allowed() -> LaunchVerdict {
  return LaunchVerdict{Allowed{}};
}

auto LaunchVerdict::cleared(ClearedLaunch launch) -> LaunchVerdict {
  return LaunchVerdict{std::move(launch)};
}

auto LaunchVerdict::refused(LaunchRefusal refusal) -> LaunchVerdict {
  return LaunchVerdict{std::move(refusal)};
}

auto LaunchVerdict::of(const MyGame* listed,
                       std::optional<LaunchRefusal::Time> trusted_now)
    -> LaunchVerdict {
  if (listed == nullptr) return refused(LaunchRefusal::not_listed());
  if (const auto hidden = listed->visibility().hide_reason())
    return refused(LaunchRefusal::hidden(*hidden));
  if (listed->is_bundle()) return refused(LaunchRefusal::choose_a_game());
  if (listed->needs_ownership_check())
    return refused(LaunchRefusal::ownership_unknown());
  if (listed->play_path() == PlayPath::store_only)
    return refused(LaunchRefusal::buy_to_stream());
  if (listed->needs_library_add())
    return refused(LaunchRefusal::add_to_library());
  if (auto unreleased =
          LaunchRefusal::of_release(listed->release(), trusted_now))
    return refused(std::move(*unreleased));
  if (const auto& remembered = listed->remembered_refusal())
    return refused(LaunchRefusal::refused_by_service(remembered->refused_at()));
  return allowed();
}

auto LaunchVerdict::before_launch(const MyGame* listed,
                                  std::optional<LaunchRefusal::Time> held_now)
    -> LaunchVerdict {
  return of(listed, held_now);
}

}  // namespace libchiaking::catalog
