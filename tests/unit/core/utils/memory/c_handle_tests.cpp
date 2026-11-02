#include "src/core/utils/memory/c_handle.hpp"

#include <gtest/gtest.h>

#include <type_traits>
#include <utility>

namespace memory_test {
namespace {

class CHandle : public ::testing::Test {
 protected:
  struct NativeSession {
    int id{};
  };

  static void release_session(NativeSession* session) {
    ++sessions_released;
    last_released_id = session->id;
    delete session;
  }

  static auto close_session(NativeSession* session) -> int {
    release_session(session);
    return 1;
  }

  using Session = libchiaking::memory::CHandle<NativeSession, release_session>;
  using ClosingSession =
      libchiaking::memory::CHandle<NativeSession, close_session>;

  void SetUp() override {
    sessions_released = 0;
    last_released_id = 0;
  }

  static inline int sessions_released = 0;
  static inline int last_released_id = 0;
};

TEST_F(CHandle, ReleasesWhatItOwnsOnce) {
  {
    Session session{new NativeSession{7}};
    EXPECT_EQ(session->id, 7);
    EXPECT_EQ(sessions_released, 0);
  }
  EXPECT_EQ(sessions_released, 1);
  EXPECT_EQ(last_released_id, 7);
}

TEST_F(CHandle, ReleasesNothingWhenEmpty) {
  {
    Session empty;
  }
  EXPECT_EQ(sessions_released, 0);
}

TEST_F(CHandle, MovesOwnershipWithTheHandle) {
  Session first{new NativeSession{3}};
  Session second = std::move(first);
  EXPECT_FALSE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(second->id, 3);
  second.reset(new NativeSession{4});
  EXPECT_EQ(sessions_released, 1);
  EXPECT_EQ(last_released_id, 3);
  second.reset();
  EXPECT_EQ(sessions_released, 2);
  EXPECT_EQ(last_released_id, 4);
}

TEST_F(CHandle, AcceptsAReleaseThatReportsAStatus) {
  {
    ClosingSession session{new NativeSession{9}};
  }
  EXPECT_EQ(sessions_released, 1);
  EXPECT_EQ(last_released_id, 9);
}

TEST_F(CHandle, IsAMoveOnlyOwnerTheSizeOfAPointer) {
  static_assert(sizeof(Session) == sizeof(NativeSession*));
  static_assert(
      std::is_empty_v<libchiaking::memory::CDeleter<release_session>>);
  static_assert(!std::is_copy_constructible_v<Session>);
}

}  // namespace
}  // namespace memory_test
