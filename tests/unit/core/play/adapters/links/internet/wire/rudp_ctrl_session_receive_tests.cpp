#include "src/core/net/transport/rudp/rudp_frame_codec.hpp"
#include "src/core/play/adapters/links/internet/wire/remote_ctrl.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/internet/wire/rudp_ctrl_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

namespace play_test {
namespace {

namespace internet = libchiaking::play::internet;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace rudp = libchiaking::net::transport::rudp;
namespace lifecycle = remote_play::ctrl_lifecycle_detail;
using rudp_ctrl::CounterCrypto;
using rudp_ctrl::SwitchChannel;

class RemoteCtrlSessionReceive : public ::testing::Test {
 protected:
  static constexpr auto target = remote_play::RegistrationTarget::ps5_1;

  void create(internet::remote_ctrl::StartupResult startup) {
    auto created =
        internet::remote_ctrl::Session::create(std::move(startup), 7);
    ASSERT_TRUE(created);
    owner_ = std::move(created.value());
  }

  void create_with_crypt_counter() {
    internet::remote_ctrl::StartupResult startup{};
    startup.final.response.next_remote_counter = 10;
    startup.final.next_local_counter = 0x1234;
    create(std::move(startup));
  }

  auto receive_once(CounterCrypto& crypto) {
    return owner_->receive_once(channel_, crypto, target,
                                std::chrono::microseconds{1}, {});
  }

  auto owner() -> internet::remote_ctrl::Session& { return *owner_; }

  SwitchChannel channel_;
  CounterCrypto counting_{true};

 private:
  std::unique_ptr<internet::remote_ctrl::Session> owner_;
};

TEST_F(RemoteCtrlSessionReceive, CompletesASeededFrameWithoutAFreshCipherSlot) {
  ASSERT_NO_FATAL_FAILURE(create_with_crypt_counter());
  const auto header = remote_play::CtrlCodec::encode_frame_header(0x7777, 8);
  ASSERT_TRUE(header);
  owner().byte_stream().seed({header->begin(), header->end()});
  channel_.deliver(SwitchChannel::switch_packet());
  EXPECT_TRUE(receive_once(counting_));
  EXPECT_TRUE(owner().byte_stream().pending().empty());
  EXPECT_EQ(owner().pump_state().remote_crypt_counter, 11U);
  EXPECT_EQ(counting_.cipher_counters(), (std::vector<std::uint64_t>{10}));
  EXPECT_EQ(channel_.sent_packets().size(), 1U);
}

TEST_F(RemoteCtrlSessionReceive, DropsAnOverflowingSeededFrameAndCarriesOn) {
  ASSERT_NO_FATAL_FAILURE(create_with_crypt_counter());
  const auto overflowing =
      remote_play::CtrlCodec::encode_frame_header(0x7777, 504);
  ASSERT_TRUE(overflowing);
  remote_play::WipingCtrlBytes seeded{overflowing->begin(), overflowing->end()};
  seeded.resize(seeded.size() + 500);
  owner().byte_stream().seed(std::move(seeded));
  channel_.deliver(SwitchChannel::switch_packet());
  EXPECT_TRUE(receive_once(counting_));
  EXPECT_EQ(owner().pump_state().last_receive.dropped_frames, 1U);
  EXPECT_TRUE(owner().byte_stream().pending().empty());
  EXPECT_EQ(channel_.sent_packets().size(), 1U);
  channel_.deliver(SwitchChannel::switch_packet());
  EXPECT_TRUE(receive_once(counting_));
  EXPECT_EQ(owner().pump_state().last_receive.dropped_frames, 0U);
  EXPECT_EQ(owner().pump_state().remote_counter, 0x5678);
  EXPECT_EQ(channel_.sent_packets().size(), 2U);
}

TEST_F(RemoteCtrlSessionReceive, AcknowledgesAStreamSwitchAckAndThenTimesOut) {
  internet::remote_ctrl::StartupResult startup{};
  startup.final.next_local_counter = 0x1234;
  startup.final.remote_counter = 0x0231;
  ASSERT_NO_FATAL_FAILURE(create(std::move(startup)));
  CounterCrypto crypto;
  channel_.deliver(SwitchChannel::switch_packet());
  auto received = receive_once(crypto);
  EXPECT_TRUE(received);
  const auto expected = rudp::FrameCodec::encode_ack_message(0x1234, 0x5678);
  ASSERT_TRUE(expected);
  EXPECT_EQ(channel_.sent(), expected.value());
  EXPECT_EQ(owner().pump_state().local_counter, 0x1234);
  EXPECT_EQ(owner().pump_state().remote_counter, 0x5678);
  const auto effect = owner().inbox().take_effect();
  ASSERT_TRUE(effect);
  EXPECT_TRUE(std::holds_alternative<lifecycle::PublishSwitchAck>(*effect));
  received = receive_once(crypto);
  ASSERT_FALSE(received);
  EXPECT_EQ(received.error(), internet::remote_ctrl::PumpError::timeout);
  EXPECT_EQ(owner().pump_state().local_counter, 0x1234);
  EXPECT_EQ(owner().pump_state().remote_counter, 0x5678);
}

}  // namespace
}  // namespace play_test
