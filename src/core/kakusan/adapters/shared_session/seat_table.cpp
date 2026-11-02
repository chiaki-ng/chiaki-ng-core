#include "src/core/kakusan/adapters/shared_session/seat_table.hpp"

#include <limits>

namespace libchiaking::kakusan {

SeatTable::SeatTable(std::uint8_t capacity, std::uint8_t local_seats,
                     std::uint32_t last_generation) noexcept
    : last_generation_(last_generation),
      capacity_(capacity),
      local_(local_seats) {
  for (std::size_t seat = 1; seat <= local_seats; ++seat)
    slots_[seat].state = SeatState::local;
}

auto SeatTable::create(std::uint8_t capacity, std::uint8_t local_seats,
                       std::uint32_t last_generation)
    -> net::kakusan::Result<SeatTable> {
  if (capacity == 0 || local_seats == 0)
    return net::kakusan::fail(net::kakusan::Error::internal);
  if (local_seats > capacity)
    return net::kakusan::fail(net::kakusan::Error::no_capacity);
  return SeatTable{capacity, local_seats, last_generation};
}

auto SeatTable::find(const net::kakusan::SeatBinding& binding) noexcept
    -> Slot* {
  if (binding.seat == 0 || binding.seat > capacity_) return nullptr;
  auto& slot = slots_[binding.seat];
  if ((slot.state != SeatState::reserved && slot.state != SeatState::granted) ||
      slot.generation != binding.generation)
    return nullptr;
  return &slot;
}

auto SeatTable::find(const net::kakusan::SeatBinding& binding) const noexcept
    -> const Slot* {
  if (binding.seat == 0 || binding.seat > capacity_) return nullptr;
  const auto& slot = slots_[binding.seat];
  if ((slot.state != SeatState::reserved && slot.state != SeatState::granted) ||
      slot.generation != binding.generation)
    return nullptr;
  return &slot;
}

void SeatTable::clear(std::size_t seat) noexcept {
  auto& slot = slots_[seat];
  if (slot.state == SeatState::reserved)
    --reserved_;
  else if (slot.state == SeatState::granted)
    --granted_;
  slot.state = SeatState::free;
}

auto SeatTable::reserve() -> net::kakusan::Result<net::kakusan::SeatBinding> {
  const auto used = static_cast<unsigned>(local_) + granted_ + reserved_;
  if (used >= capacity_)
    return net::kakusan::fail(net::kakusan::Error::no_capacity);
  if (last_generation_ == std::numeric_limits<std::uint32_t>::max())
    return net::kakusan::fail(net::kakusan::Error::no_capacity);
  for (unsigned seat = 1; seat <= capacity_; ++seat) {
    auto& slot = slots_[seat];
    if (slot.state != SeatState::free) continue;
    ++last_generation_;
    slot.state = SeatState::reserved;
    slot.generation = last_generation_;
    ++reserved_;
    return net::kakusan::SeatBinding{static_cast<std::uint8_t>(seat),
                                     slot.generation};
  }
  return net::kakusan::fail(net::kakusan::Error::no_capacity);
}

auto SeatTable::grant(const net::kakusan::SeatBinding& binding)
    -> net::kakusan::Result<void> {
  auto* slot = find(binding);
  if (slot == nullptr || slot->state != SeatState::reserved)
    return net::kakusan::fail(net::kakusan::Error::protocol);
  slot->state = SeatState::granted;
  --reserved_;
  ++granted_;
  return {};
}

auto SeatTable::release(const net::kakusan::SeatBinding& binding) -> bool {
  if (find(binding) == nullptr) return false;
  clear(binding.seat);
  return true;
}

auto SeatTable::state_of(
    const net::kakusan::SeatBinding& binding) const noexcept -> SeatState {
  const auto* slot = find(binding);
  return slot == nullptr ? SeatState::free : slot->state;
}

auto SeatTable::holds(const net::kakusan::SeatBinding& binding) const noexcept
    -> bool {
  return find(binding) != nullptr;
}

auto SeatTable::owns(const net::kakusan::SeatBinding& binding) const noexcept
    -> bool {
  return state_of(binding) == SeatState::granted;
}

auto SeatTable::is_local(std::uint8_t seat) const noexcept -> bool {
  return seat >= 1 && seat <= capacity_ &&
         slots_[seat].state == SeatState::local;
}

auto SeatTable::count_in(unsigned low, unsigned high,
                         SeatState state) const noexcept -> std::size_t {
  std::size_t count = 0;
  for (unsigned seat = low; seat <= high; ++seat) {
    if (slots_[seat].state == state) ++count;
  }
  return count;
}

auto SeatTable::collect(const std::array<Span, 2>& spans) const
    -> std::vector<net::kakusan::SeatBinding> {
  std::vector<net::kakusan::SeatBinding> victims;
  std::array<std::size_t, 2> taken{};
  for (const auto wanted : {SeatState::reserved, SeatState::granted}) {
    for (std::size_t index = 0; index < spans.size(); ++index) {
      const auto& span = spans[index];
      for (unsigned seat = span.high; seat >= span.low && seat != 0; --seat) {
        if (taken[index] >= span.limit) break;
        const auto& slot = slots_[seat];
        if (slot.state != wanted) continue;
        victims.push_back(net::kakusan::SeatBinding{
            static_cast<std::uint8_t>(seat), slot.generation});
        ++taken[index];
      }
    }
  }
  return victims;
}

void SeatTable::convert_lowest_free(std::size_t count) noexcept {
  for (unsigned seat = 1; seat <= capacity_ && count > 0; ++seat) {
    if (slots_[seat].state != SeatState::free) continue;
    slots_[seat].state = SeatState::local;
    --count;
  }
}

auto SeatTable::set_local_seats(std::uint8_t local_seats)
    -> net::kakusan::Result<std::vector<net::kakusan::SeatBinding>> {
  if (local_seats == 0)
    return net::kakusan::fail(net::kakusan::Error::internal);
  if (local_seats > capacity_)
    return net::kakusan::fail(net::kakusan::Error::no_capacity);
  std::vector<net::kakusan::SeatBinding> revoked;
  if (local_seats > local_) {
    const auto wanted = static_cast<std::size_t>(local_seats - local_);
    const auto available = count_in(1, capacity_, SeatState::free);
    const auto needed = wanted > available ? wanted - available : 0;
    revoked = collect({Span{1, capacity_, needed}, Span{1, 0, 0}});
    for (const auto& binding : revoked) clear(binding.seat);
    convert_lowest_free(wanted);
  } else {
    for (unsigned seat = capacity_; seat >= 2 && local_ > local_seats; --seat) {
      if (slots_[seat].state != SeatState::local) continue;
      slots_[seat].state = SeatState::free;
      --local_;
    }
  }
  local_ = local_seats;
  return revoked;
}

auto SeatTable::set_capacity(std::uint8_t capacity)
    -> net::kakusan::Result<std::vector<net::kakusan::SeatBinding>> {
  if (capacity == 0) return net::kakusan::fail(net::kakusan::Error::internal);
  if (capacity < local_)
    return net::kakusan::fail(net::kakusan::Error::no_capacity);
  if (capacity >= capacity_) {
    capacity_ = capacity;
    return std::vector<net::kakusan::SeatBinding>{};
  }
  const auto displaced = count_in(static_cast<unsigned>(capacity) + 1,
                                  capacity_, SeatState::local);
  const auto available = count_in(1, capacity, SeatState::free);
  const auto needed = displaced > available ? displaced - available : 0;
  auto revoked = collect({Span{static_cast<unsigned>(capacity) + 1, capacity_,
                               net::kakusan::max_seat_capacity},
                          Span{1, capacity, needed}});
  for (const auto& binding : revoked) clear(binding.seat);
  for (unsigned seat = static_cast<unsigned>(capacity) + 1; seat <= capacity_;
       ++seat)
    slots_[seat].state = SeatState::free;
  capacity_ = capacity;
  convert_lowest_free(displaced);
  return revoked;
}

auto SeatTable::summary() const noexcept -> SeatSummary {
  return SeatSummary{capacity_, local_, granted_, reserved_};
}

}  // namespace libchiaking::kakusan
