#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/session_share_case.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace play_test {

using test_support::play::SessionRig;
namespace {

class ThreadAllocations {
 public:
  static void count_one() noexcept { ++count_; }
  static auto so_far() noexcept -> std::size_t { return count_; }

 private:
  static inline thread_local std::size_t count_ = 0;
};

}  // namespace
}  // namespace play_test

auto operator new(std::size_t size) -> void* {
  play_test::ThreadAllocations::count_one();
  if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept { std::free(memory); }

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace play_test {
namespace {

namespace play = libchiaking::play;
namespace takion = libchiaking::net::protocol::takion;

class PlaySessionShareDatagrams : public ShareCase {
 protected:
  static constexpr std::size_t total = 96;

  void SetUp() override {
    for (std::size_t index = 0; index < total; ++index) {
      payloads_[index].fill(static_cast<std::byte>(index));
      auto& datagram = wired_[index];
      datagram.media = index % 3 == 0 ? takion::SourceMedia::audio
                                      : takion::SourceMedia::video;
      datagram.key_position = 1000 + index * 16;
      datagram.bytes = payloads_[index];
      datagram.sealed_size = 20;
      datagram.unit_index = static_cast<std::uint16_t>(index);
      datagram.unit_total = static_cast<std::uint16_t>(total);
      datagram.unit_redundant = 2;
      datagram.unit_size = 24;
      datagram.codec = 5;
      datagram.stream_index = 1;
      datagram.auxiliary = index % 5 == 0;
    }
  }

  auto payload(std::size_t index) const -> const std::array<std::byte, 24>& {
    return payloads_[index];
  }
  auto wired() const -> const std::array<takion::SourceDatagram, total>& {
    return wired_;
  }

 private:
  std::array<std::array<std::byte, 24>, total> payloads_{};
  std::array<takion::SourceDatagram, total> wired_{};
};

TEST_F(PlaySessionShareDatagrams, AreForwardedInOrderWithoutAllocating) {
  SessionRig rig;
  RecordedAudience audience;
  auto sharing = rig.session->share().begin(audience);
  ASSERT_TRUE(sharing);
  rig.start_playing();

  std::atomic<bool> early{false};
  rig.link.then([&](play::testing::ScriptedLink&) {
    rig.core->share_rules().tap().on_source_datagram(wired()[0]);
    early.store(true);
  });
  SessionRig::eventually([&] { return early.load(); });
  EXPECT_EQ(audience.datagram_count(), 0U);

  announce(rig);
  rig.link.describe(shared_description());
  audience.until("stream_started");

  std::atomic<std::size_t> allocated{1};
  rig.link.then([&](play::testing::ScriptedLink&) {
    auto& tap = rig.core->share_rules().tap();
    const auto before = ThreadAllocations::so_far();
    for (const auto& datagram : wired()) tap.on_source_datagram(datagram);
    allocated.store(ThreadAllocations::so_far() - before);
  });
  audience.until_datagrams(total);
  SessionRig::eventually([&] { return allocated.load() != 1; });
  EXPECT_EQ(allocated.load(), 0U);
  ASSERT_EQ(audience.datagram_count(), total);
  const auto core_thread = *rig.events.threads().begin();
  for (std::size_t index = 0; index < total; ++index) {
    SCOPED_TRACE(index);
    const auto& seen = audience.datagram(index);
    const auto& sent = wired()[index];
    EXPECT_EQ(seen.source, payload(index).data());
    EXPECT_EQ(seen.size, 24U);
    EXPECT_EQ(seen.thread, core_thread);
    EXPECT_TRUE(std::ranges::equal(seen.view().bytes, payload(index)));
    EXPECT_EQ(seen.facts.media, index % 3 == 0
                                    ? play::SharedDatagram::Media::audio
                                    : play::SharedDatagram::Media::video);
    EXPECT_EQ(seen.facts.key_position, sent.key_position);
    EXPECT_EQ(seen.facts.sealed_size, 20U);
    EXPECT_EQ(seen.facts.unit_index, index);
    EXPECT_EQ(seen.facts.unit_total, total);
    EXPECT_EQ(seen.facts.unit_redundant, 2U);
    EXPECT_EQ(seen.facts.unit_size, 24U);
    EXPECT_EQ(seen.facts.codec, 5U);
    EXPECT_EQ(seen.facts.stream_index, 1U);
    EXPECT_EQ(seen.facts.auxiliary, index % 5 == 0);
  }

  sharing.value().stop();
  std::atomic<bool> after{false};
  rig.link.then([&](play::testing::ScriptedLink&) {
    rig.core->share_rules().tap().on_source_datagram(wired()[1]);
    after.store(true);
  });
  SessionRig::eventually([&] { return after.load(); });
  EXPECT_EQ(audience.datagram_count(), total);
}

}  // namespace
}  // namespace play_test
