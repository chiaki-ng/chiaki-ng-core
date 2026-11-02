#pragma once

#include "src/core/net/kakusan/protocol_error.hpp"
#include "src/core/kakusan/adapters/shared_session/seat_table.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>

namespace kakusan_test {

class SeatTableTest : public ::testing::Test {
 protected:
  using SeatBinding = libchiaking::net::kakusan::SeatBinding;
  using SeatTable = libchiaking::kakusan::SeatTable;

  static auto table(std::uint8_t capacity, std::uint8_t local,
                    std::uint32_t last_generation = 0) -> SeatTable {
    auto created = SeatTable::create(capacity, local, last_generation);
    EXPECT_TRUE(created);
    return std::move(created.value());
  }

  static auto reserved(SeatTable& seats) -> SeatBinding {
    const auto binding = seats.reserve();
    EXPECT_TRUE(binding);
    return binding.value();
  }

  static auto granted(SeatTable& seats) -> SeatBinding {
    const auto binding = reserved(seats);
    EXPECT_TRUE(seats.grant(binding));
    return binding;
  }

  static void invariant(const SeatTable& seats) {
    const auto snapshot = seats.summary();
    EXPECT_EQ(snapshot.capacity, seats.capacity());
    EXPECT_EQ(snapshot.local_seats, seats.local_seats());
    EXPECT_EQ(snapshot.remote_seats, seats.remote_seats());
    EXPECT_EQ(snapshot.reserved_seats, seats.reserved_seats());
    EXPECT_LE(static_cast<unsigned>(snapshot.local_seats) +
                  snapshot.remote_seats + snapshot.reserved_seats,
              snapshot.capacity);
  }
};

}  // namespace kakusan_test
