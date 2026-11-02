#include "src/core/net/protocol/takion/client/limits.hpp"
#include "src/core/net/protocol/takion/control_chunk_codec.hpp"
#include "src/core/net/protocol/takion/control_codec.hpp"
#include "src/core/net/protocol/takion/detail/packet_codec.hpp"
#include "src/core/net/protocol/takion/reliable_channel.hpp"
#include "src/core/utils/numeric/checked.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace takion_test {
namespace {

namespace takion = libchiaking::net::protocol::takion;
using libchiaking::net::protocol::takion::client_detail::ClientLimits;

class TakionClientLimits : public ::testing::Test {
 protected:
  static auto valid_limits() -> takion::TakionClientLimits {
    return takion::TakionClientLimits{
        .mailbox_capacity = 16,
        .maximum_commands_per_pump = 4,
        .maximum_datagrams_per_pump = 4,
        .maximum_message_size = 4096,
        .maximum_fragment_size = 1024,
        .maximum_fragment_count = 16,
        .retransmission_window_size = 32,
        .receive_reorder_size = 32,
        .retransmission_timeout_us = 1000,
        .maximum_retries = 5,
    };
  }

  static auto valid_config() -> takion::ClientConfig {
    takion::ClientConfig config{};
    config.limits = valid_limits();
    return config;
  }

  const takion::WireProfile profile = *takion::resolve_takion_wire_profile(7);
};

TEST(TakionCheckedArithmetic, ReportsOverflowAndUnderflowAsNothing) {
  using libchiaking::numeric::checked_add;
  using libchiaking::numeric::checked_mul;
  using libchiaking::numeric::checked_sub;
  const std::size_t ten = 10;
  const std::size_t three = 3;
  const auto huge = std::numeric_limits<std::size_t>::max();
  EXPECT_EQ(checked_sub(ten, three), std::optional<std::size_t>{7});
  EXPECT_EQ(checked_sub(three, ten), std::nullopt);
  EXPECT_EQ(checked_add(ten, three), std::optional<std::size_t>{13});
  EXPECT_EQ(checked_add(huge, std::size_t{1}), std::nullopt);
  EXPECT_EQ(checked_mul(ten, three), std::optional<std::size_t>{30});
  EXPECT_EQ(checked_mul(huge, std::size_t{2}), std::nullopt);
  EXPECT_EQ(checked_mul(std::size_t{0}, huge), std::optional<std::size_t>{0});
}

TEST_F(TakionClientLimits, ActiveProfileCapacityFitsTheFragmentAndLengthField) {
  const auto capacity = ClientLimits::active_profile_control_capacity(
      valid_limits(), profile, takion::PacketEnvelope{takion::PlainEnvelope{}});
  ASSERT_TRUE(capacity);
  EXPECT_LE(*capacity, valid_limits().maximum_fragment_size);
  EXPECT_LE(*capacity, std::numeric_limits<std::uint16_t>::max() -
                           takion::control_length_bias);
}

TEST_F(TakionClientLimits, ActiveProfileCapacityNeedsRoomForTheHeaders) {
  auto tiny_limits = valid_limits();
  tiny_limits.maximum_fragment_size = 1;
  EXPECT_FALSE(ClientLimits::active_profile_control_capacity(
      tiny_limits, profile, takion::PacketEnvelope{takion::PlainEnvelope{}}));
}

TEST_F(TakionClientLimits, ReliableCapacityIsBoundedByTheMessageSize) {
  const takion::ReliableChannelConfig reliable_config{
      .maximum_message_size = 4096,
      .maximum_fragment_size = 1024,
      .maximum_fragment_count = 16,
  };
  const auto reliable =
      ClientLimits::reliable_control_capacity(reliable_config);
  ASSERT_TRUE(reliable);
  EXPECT_LE(*reliable, 4096U);
}

TEST_F(TakionClientLimits, ReliableCapacityRefusesAZeroMessageSize) {
  const takion::ReliableChannelConfig zero_config{
      .maximum_message_size = 0,
      .maximum_fragment_size = 1024,
      .maximum_fragment_count = 16,
  };
  EXPECT_FALSE(ClientLimits::reliable_control_capacity(zero_config));
}

TEST_F(TakionClientLimits, EffectiveControlLimitIsTheSmallestNonZeroBound) {
  EXPECT_EQ(ClientLimits::effective_control_message_limit(100, 200, 300),
            std::optional<std::size_t>{100});
  EXPECT_EQ(ClientLimits::effective_control_message_limit(0, 200, 300),
            std::nullopt);
}

TEST_F(TakionClientLimits, RefuseAnEmptyMailbox) {
  EXPECT_TRUE(ClientLimits::valid_limits(valid_limits()));
  auto invalid = valid_limits();
  invalid.mailbox_capacity = 0;
  EXPECT_FALSE(ClientLimits::valid_limits(invalid));
}

TEST_F(TakionClientLimits, ConfigValidationRefusesAnUnknownBootstrapVersion) {
  EXPECT_TRUE(ClientLimits::validate_client_config(valid_config()));
  auto bad_config = valid_config();
  bad_config.bootstrap_version = 0xffffffff;
  EXPECT_FALSE(ClientLimits::validate_client_config(bad_config));
}

TEST_F(TakionClientLimits, ConfigValidationRefusesAZeroFragmentSize) {
  auto zero_limits_config = valid_config();
  zero_limits_config.limits.maximum_fragment_size = 0;
  EXPECT_FALSE(ClientLimits::validate_client_config(zero_limits_config));
}

TEST_F(TakionClientLimits, ConfigValidationRefusesAnEmptyNegotiation) {
  auto empty_negotiation = valid_config();
  empty_negotiation.profile_selection = takion::NegotiatedTakionProfiles{{}};
  EXPECT_FALSE(ClientLimits::validate_client_config(empty_negotiation));
}

}  // namespace
}  // namespace takion_test
