#include "tests/support/enum_printing.hpp"
#include "tests/support/play/public_household.hpp"

#include <libchiaking/platform/runtime/threading.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <optional>

namespace play_test {

using test_support::LiveProviders;
using test_support::play::PublicEvents;
using test_support::play::PublicHousehold;
using test_support::play::PublicMedia;
using test_support::play::SignedInPlayer;
namespace {

namespace platform = libchiaking::platform;
namespace play = libchiaking::play;
using play::Where;
using play::testing::ScriptedLink;

class SessionThreadRefused final : public platform::ThreadingProvider {
 public:
  auto start_thread(platform::native::ThreadEntry entry)
      -> libchiaking::Expected<std::unique_ptr<platform::native::Thread>,
                               platform::PlatformError> override {
    if (entry.role == platform::ThreadRole::session &&
        refusing_.load(std::memory_order_acquire)) {
      refused_.fetch_add(1, std::memory_order_acq_rel);
      return libchiaking::unexpected(platform::PlatformError::thread_failure);
    }
    return inner_.start_thread(entry);
  }
  auto create_mutex(platform::MutexKind kind)
      -> libchiaking::Expected<std::unique_ptr<platform::native::Mutex>,
                               platform::PlatformError> override {
    return inner_.create_mutex(kind);
  }
  auto create_condition_variable() -> libchiaking::Expected<
      std::unique_ptr<platform::native::ConditionVariable>,
      platform::PlatformError> override {
    return inner_.create_condition_variable();
  }
  auto create_stop_source()
      -> libchiaking::Expected<std::unique_ptr<platform::native::StopSource>,
                               platform::PlatformError> override {
    return inner_.create_stop_source();
  }

  auto refused() const noexcept -> unsigned {
    return refused_.load(std::memory_order_acquire);
  }

  void start_sessions() noexcept {
    refusing_.store(false, std::memory_order_release);
  }

 private:
  platform::DefaultThreadingProvider inner_;
  std::atomic<unsigned> refused_{};
  std::atomic<bool> refusing_{true};
};

TEST(ScriptedStartThreading, RefusesTheStartWhenTheSessionThreadCannotStart) {
  SessionThreadRefused threading;
  PublicHousehold home{&threading};
  PublicMedia media;
  PublicEvents events;
  ScriptedLink link{LiveProviders::threading(), LiveProviders::steady()};
  std::optional<
      libchiaking::Expected<std::shared_ptr<play::Session>, play::StartRefusal>>
      started;
  EXPECT_NO_THROW(started.emplace(home.play.start_scripted(
      home.home_user(41), Where::home, link, {}, media, events)));
  EXPECT_EQ(threading.refused(), 1U);
  ASSERT_TRUE(started.has_value());
  ASSERT_FALSE(started->has_value());
  EXPECT_EQ(started->error().kind(), play::StartRefusal::Kind::host_refused);

  threading.start_sessions();
  ScriptedLink working{LiveProviders::threading(), LiveProviders::steady()};
  working.connect();
  PublicMedia later_media;
  PublicEvents later_events;
  const auto again = home.play.start_scripted(
      home.home_user(41), Where::home, working, {}, later_media, later_events);
  if (!again) {
    EXPECT_NE(again.error().kind(), play::StartRefusal::Kind::busy);
  }
  ASSERT_TRUE(again);
  EXPECT_TRUE(again.value()->leave());
  static_cast<void>(again.value()->wait_until_ended());
}

}  // namespace
}  // namespace play_test
