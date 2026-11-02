#include "src/core/utils/containers/bounded_queue.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string>

namespace containers_test {
namespace {

class BoundedQueue : public ::testing::Test {
 protected:
  template <class T>
  using Queue = libchiaking::containers::BoundedQueue<T>;
  using Frames = Queue<std::string>;
  using Numbers = Queue<int>;

  static auto frame_bytes() -> Frames::Weigh {
    return [](const std::string& frame) { return frame.size(); };
  }
};

TEST_F(BoundedQueue, HandsItemsOutInTheOrderTheyArrived) {
  Frames frames{4};
  EXPECT_TRUE(frames.push("first"));
  EXPECT_TRUE(frames.push("second"));
  EXPECT_EQ(frames.size(), 2U);
  EXPECT_FALSE(frames.empty());
  EXPECT_EQ(frames.take(), "first");
  EXPECT_EQ(frames.take(), "second");
  EXPECT_FALSE(frames.take());
  EXPECT_TRUE(frames.empty());
}

TEST_F(BoundedQueue, RefusesTheNewestItemWhenFull) {
  Frames frames{2};
  EXPECT_TRUE(frames.push("a"));
  EXPECT_TRUE(frames.push("b"));
  EXPECT_FALSE(frames.push("c"));
  EXPECT_EQ(frames.size(), 2U);
  EXPECT_EQ(frames.shed(), 0U);
  EXPECT_EQ(frames.take(), "a");
  EXPECT_TRUE(frames.push("c"));
}

TEST_F(BoundedQueue, DropsTheOldestItemWhenSheddingAndFull) {
  Frames frames{2, Frames::WhenFull::shed_oldest};
  EXPECT_TRUE(frames.push("a"));
  EXPECT_TRUE(frames.push("b"));
  EXPECT_TRUE(frames.push("c"));
  EXPECT_EQ(frames.size(), 2U);
  EXPECT_EQ(frames.shed(), 1U);
  EXPECT_EQ(frames.take(), "b");
  EXPECT_EQ(frames.take(), "c");
}

TEST_F(BoundedQueue, BoundsWhatItHoldsByTheByteBudget) {
  Frames frames{8, 6, frame_bytes()};
  EXPECT_TRUE(frames.push("abc"));
  EXPECT_TRUE(frames.push("de"));
  EXPECT_EQ(frames.bytes(), 5U);
  EXPECT_FALSE(frames.push("fg"));
  EXPECT_TRUE(frames.push("f"));
  EXPECT_EQ(frames.bytes(), 6U);
  EXPECT_EQ(frames.take(), "abc");
  EXPECT_EQ(frames.bytes(), 3U);
  EXPECT_FALSE(frames.push("toolong"));
  frames.clear();
  EXPECT_TRUE(frames.empty());
  EXPECT_EQ(frames.bytes(), 0U);
}

TEST_F(BoundedQueue, ShedsOldItemsToFitTheNewestInTheByteBudget) {
  Frames frames{8, 6, frame_bytes(), Frames::WhenFull::shed_oldest};
  EXPECT_TRUE(frames.push("abc"));
  EXPECT_TRUE(frames.push("de"));
  EXPECT_TRUE(frames.push("fgh"));
  EXPECT_EQ(frames.shed(), 1U);
  EXPECT_EQ(frames.bytes(), 5U);
  EXPECT_FALSE(frames.push("toolong"));
  EXPECT_EQ(frames.take(), "de");
}

TEST_F(BoundedQueue, HoldsNothingWithoutCapacity) {
  Frames frames{0, Frames::WhenFull::shed_oldest};
  EXPECT_FALSE(frames.push("a"));
  EXPECT_TRUE(frames.empty());
}

TEST_F(BoundedQueue, CarriesMoveOnlyItems) {
  Queue<std::unique_ptr<int>> owned{1};
  EXPECT_TRUE(owned.push(std::make_unique<int>(5)));
  const auto taken = owned.take();
  ASSERT_TRUE(taken);
  ASSERT_TRUE(*taken);
  EXPECT_EQ(**taken, 5);
}

TEST_F(BoundedQueue, KeepsOrderAsTheRingWrapsGrowsAndClears) {
  Numbers numbers{1000};
  int next_in = 0;
  int next_out = 0;
  for (int round = 0; round < 50; ++round) {
    for (int pushed = 0; pushed < round % 7 + 3; ++pushed)
      EXPECT_TRUE(numbers.push(next_in++)) << "round " << round;
    for (int taken = 0; taken < round % 5 + 1; ++taken)
      EXPECT_EQ(numbers.take(), next_out++) << "round " << round;
  }
  EXPECT_EQ(numbers.size(), static_cast<std::size_t>(next_in - next_out));
  while (const auto value = numbers.take()) EXPECT_EQ(*value, next_out++);
  EXPECT_EQ(next_out, next_in);
  EXPECT_TRUE(numbers.empty());
  EXPECT_TRUE(numbers.push(7));
  EXPECT_TRUE(numbers.push(8));
  numbers.clear();
  EXPECT_TRUE(numbers.empty());
  EXPECT_FALSE(numbers.take());
  EXPECT_TRUE(numbers.push(9));
  EXPECT_EQ(numbers.take(), 9);
}

TEST_F(BoundedQueue, KeepsTheNewestItemsInOrderWhenASheddingRingWraps) {
  Numbers numbers{5, Numbers::WhenFull::shed_oldest};
  for (int value = 0; value < 23; ++value)
    EXPECT_TRUE(numbers.push(value)) << "pushing " << value;
  EXPECT_EQ(numbers.size(), 5U);
  EXPECT_EQ(numbers.shed(), 18U);
  for (int value = 18; value < 23; ++value) EXPECT_EQ(numbers.take(), value);
  EXPECT_TRUE(numbers.empty());
}

}  // namespace
}  // namespace containers_test
