#include "libchiaking/detail/catalog/release_date.hpp"

#include "src/core/utils/timing/instant.hpp"

namespace libchiaking::catalog {

auto ReleaseDate::parse_time(std::string_view ps_date) -> std::optional<Time> {
  return timing::Instant::from_text(ps_date).transform(&timing::Instant::time);
}

auto ReleaseDate::of_record(std::optional<std::string_view> active_date,
                            bool preorder,
                            std::optional<std::string_view> playable_from)
    -> ReleaseDate {
  if (playable_from && !playable_from->empty())
    if (const auto playable = parse_time(*playable_from);
        playable && *playable > Time{})
      return at(*playable);
  if (active_date && !active_date->empty()) {
    const auto parsed = parse_time(*active_date);
    return parsed && *parsed > Time{} ? at(*parsed) : unannounced();
  }
  return preorder ? unannounced() : unrestricted();
}

auto ReleaseDate::earliest(ReleaseDate other) const noexcept -> ReleaseDate {
  return other.comes_before(*this) ? other : *this;
}

auto ReleaseDate::latest(ReleaseDate other) const noexcept -> ReleaseDate {
  return comes_before(other) ? other : *this;
}

auto ReleaseDate::state(std::optional<Time> trusted_now) const noexcept
    -> State {
  if (kind_ == Kind::unrestricted) return State::released;
  if (kind_ == Kind::unannounced) return State::unannounced;
  if (!trusted_now) return State::no_trusted_clock;
  return time_ >= *trusted_now ? State::not_released : State::released;
}

auto ReleaseDate::blocks_launch(std::optional<Time> trusted_now) const noexcept
    -> bool {
  return state(trusted_now) != State::released;
}

auto ReleaseDate::countdown(Time trusted_now) const noexcept
    -> std::optional<Countdown> {
  if (state(trusted_now) != State::not_released) return std::nullopt;
  const auto left =
      std::chrono::ceil<std::chrono::seconds>(time_ - trusted_now);
  const auto days = std::chrono::floor<std::chrono::days>(left);
  const std::chrono::hh_mm_ss clock{left - days};
  return Countdown{days.count(), static_cast<int>(clock.hours().count()),
                   static_cast<int>(clock.minutes().count()),
                   static_cast<int>(clock.seconds().count())};
}

auto ReleaseDate::comes_before(ReleaseDate other) const noexcept -> bool {
  if (kind_ != other.kind_) return kind_ < other.kind_;
  return kind_ == Kind::scheduled && time_ < other.time_;
}

}  // namespace libchiaking::catalog
