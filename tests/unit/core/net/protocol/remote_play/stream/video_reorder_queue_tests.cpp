#include "src/core/net/protocol/remote_play/stream/video_reorder_queue.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;

class VideoReorderQueue : public ::testing::Test {
 protected:
  using Indices = std::vector<std::uint16_t>;

  void push(std::uint16_t index, std::uint64_t now_us, std::uint16_t unit = 0) {
    takion::AvPacketValues values{};
    values.type = takion::PacketType{0x02};
    values.packet_index = index;
    values.unit_index = unit;
    queue.push(values, now_us, ready);
  }

  void flush(std::uint64_t now_us) { queue.flush(now_us, ready); }

  auto take() -> Indices {
    Indices out;
    for (const auto& values : ready) out.push_back(values.packet_index);
    ready.clear();
    return out;
  }

  remote_play::VideoReorderQueue queue;
  std::vector<takion::AvPacketValues> ready;
};

TEST_F(VideoReorderQueue, DeliversInOrderPacketsImmediately) {
  push(10, 0);
  push(11, 0);
  push(12, 0);
  EXPECT_EQ(take(), (Indices{10, 11, 12}));
  EXPECT_FALSE(queue.waiting());
}

TEST_F(VideoReorderQueue, ReordersPacketsThatArriveWithinTheTimeout) {
  push(20, 0);
  EXPECT_EQ(take(), Indices{20});
  push(22, 1'000);
  EXPECT_TRUE(ready.empty());
  EXPECT_TRUE(queue.waiting());
  push(21, 6'000);
  EXPECT_EQ(take(), (Indices{21, 22}));
  EXPECT_EQ(queue.skipped(), 0U);
  EXPECT_EQ(queue.dropped(), 0U);
}

TEST_F(VideoReorderQueue, SkipsALostHeadAfterTheTimeoutAndDropsItWhenLate) {
  push(30, 0);
  EXPECT_EQ(take(), Indices{30});
  push(33, 100);
  EXPECT_TRUE(ready.empty());
  flush(16'100);
  EXPECT_TRUE(ready.empty());
  flush(16'101);
  EXPECT_EQ(take(), Indices{33});
  EXPECT_EQ(queue.skipped(), 2U);
  EXPECT_FALSE(queue.waiting());
  push(31, 16'200);
  EXPECT_TRUE(ready.empty());
  EXPECT_EQ(queue.dropped(), 1U);
}

TEST_F(VideoReorderQueue, DropsDuplicates) {
  push(40, 0);
  push(42, 0);
  push(42, 0);
  push(40, 0);
  EXPECT_EQ(take(), Indices{40});
  EXPECT_EQ(queue.dropped(), 2U);
}

TEST_F(VideoReorderQueue, StartsAtTheFirstUnitOfAFrame) {
  push(50, 0, 2);
  EXPECT_TRUE(ready.empty());
  EXPECT_TRUE(queue.waiting());
  push(48, 1'000, 0);
  EXPECT_EQ(take(), Indices{48});
  flush(17'001);
  EXPECT_EQ(take(), Indices{50});
  EXPECT_EQ(queue.skipped(), 1U);
}

TEST_F(VideoReorderQueue, DropsFromTheBeginWhenTheWindowIsExceeded) {
  push(100, 0);
  EXPECT_EQ(take(), Indices{100});
  push(102, 0);
  EXPECT_TRUE(ready.empty());
  push(165, 0);
  EXPECT_EQ(take(), Indices{102});
  EXPECT_TRUE(queue.waiting());
  EXPECT_EQ(queue.dropped(), 0U);
  push(230, 0);
  EXPECT_EQ(take(), Indices{230});
  EXPECT_EQ(queue.dropped(), 1U);
  EXPECT_FALSE(queue.waiting());
}

TEST_F(VideoReorderQueue, WrapsSequenceNumbers) {
  push(65534, 0);
  push(0, 0);
  EXPECT_EQ(take(), Indices{65534});
  push(65535, 0);
  EXPECT_EQ(take(), (Indices{65535, 0}));
}

TEST_F(VideoReorderQueue, GivesANewGapAfterProgressAFreshTimeout) {
  push(300, 0);
  push(302, 0);
  push(301, 5'000);
  EXPECT_EQ(take(), (Indices{300, 301, 302}));
  push(304, 20'000);
  flush(36'000);
  EXPECT_TRUE(ready.empty());
  flush(36'001);
  EXPECT_EQ(take(), Indices{304});
}

TEST_F(VideoReorderQueue, RestartsFromTheNextPacketAfterAReset) {
  push(400, 0);
  push(402, 0);
  queue.reset();
  EXPECT_FALSE(queue.waiting());
  ready.clear();
  push(7, 0);
  EXPECT_EQ(take(), Indices{7});
}

}  // namespace
}  // namespace remote_play_test
