#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/messages.hpp"
#include "tests/unit/core/net/kakusan/control_channel_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <variant>
#include <vector>

namespace net_kakusan_test {
namespace {

namespace control = libchiaking::net::kakusan::control;
using libchiaking::net::kakusan::SeatBinding;

class ControlChannelExchange : public ControlChannelTest {
 protected:
  class Dice {
   public:
    explicit Dice(std::uint64_t seed) : state_(0x9e3779b97f4a7c15ULL * seed) {}

    auto roll(std::uint64_t bound) -> std::uint64_t {
      state_ ^= state_ << 13U;
      state_ ^= state_ >> 7U;
      state_ ^= state_ << 17U;
      return state_ % bound;
    }

   private:
    std::uint64_t state_;
  };

  class LossyLink {
   public:
    explicit LossyLink(Dice& dice) : dice_(dice) {}

    void route(const Records& records, std::uint64_t now) {
      for (const auto& record : records) {
        if (dice_.roll(100) < 20) continue;
        const auto copies = dice_.roll(100) < 25 ? 2 : 1;
        for (int copy = 0; copy < copies; ++copy)
          in_flight_.emplace(now + dice_.roll(60) * ms, record);
      }
    }

    void drain(ControlChannel& channel, std::uint64_t now,
               std::vector<Message>& delivered) {
      while (!in_flight_.empty() && in_flight_.begin()->first <= now) {
        const auto record = in_flight_.begin()->second;
        in_flight_.erase(in_flight_.begin());
        auto result = channel.receive(record, now);
        EXPECT_TRUE(result);
        if (result && result.value())
          delivered.push_back(std::move(*result.value()));
      }
    }

   private:
    Dice& dice_;
    std::multimap<std::uint64_t, Record> in_flight_;
  };

  static auto every_flow_message() -> SentMessages {
    auto messages = fixture_messages();
    messages.emplace_back(
        true,
        control::SeatRevoked{SeatBinding{3, 7}, control::Reason::capacity});
    messages.emplace_back(true, ping_with(0x11));
    messages.emplace_back(false, control::Pong{ping_with(0x11).nonce});
    messages.emplace_back(false, control::Ping{ping_with(0x22).nonce});
    messages.emplace_back(true, control::Pong{ping_with(0x22).nonce});
    messages.emplace_back(true, control::End{control::Reason::user});
    messages.emplace_back(false, control::Failure{Error::source_ended});
    messages.emplace_back(true, setup_with_headers({512, 512, 512, 512}));
    messages.emplace_back(false, setup_with_headers({512, 512, 512}));
    return messages;
  }
};

TEST_F(ControlChannelExchange, DeliversEveryFlowMessageOverALossyLink) {
  const auto messages = every_flow_message();
  for (std::uint64_t seed = 1; seed <= 6; ++seed) {
    SCOPED_TRACE(seed);
    ControlChannel host{start};
    ControlChannel guest{start};
    std::vector<Message> host_expected;
    std::vector<Message> guest_expected;
    std::vector<ControlTicket> host_tickets;
    std::vector<ControlTicket> guest_tickets;
    for (const auto& [from_host, message] : messages) {
      auto& channel = from_host ? host : guest;
      (from_host ? guest_expected : host_expected)
          .push_back(test_support::kakusan::Messages::clone(message));
      (from_host ? host_tickets : guest_tickets)
          .push_back(enqueued(channel, message, start));
    }

    Dice dice{seed};
    LossyLink to_host{dice};
    LossyLink to_guest{dice};
    std::vector<Message> at_host;
    std::vector<Message> at_guest;
    std::vector<ControlTicket> host_acked;
    std::vector<ControlTicket> guest_acked;

    auto now = start;
    const auto limit = start + 600 * sec;
    while (now < limit) {
      to_host.drain(host, now, at_host);
      to_guest.drain(guest, now, at_guest);
      to_guest.route(polled(host, now), now);
      to_host.route(polled(guest, now), now);
      while (const auto ticket = host.take_acknowledged())
        host_acked.push_back(*ticket);
      while (const auto ticket = guest.take_acknowledged())
        guest_acked.push_back(*ticket);
      if (host.idle() && guest.idle() &&
          at_host.size() == host_expected.size() &&
          at_guest.size() == guest_expected.size())
        break;
      now += ms;
    }
    EXPECT_LT(now, limit);
    ASSERT_EQ(at_host.size(), host_expected.size());
    ASSERT_EQ(at_guest.size(), guest_expected.size());
    for (std::size_t index = 0; index < at_host.size(); ++index)
      EXPECT_EQ(encoded_text(at_host[index]),
                encoded_text(host_expected[index]));
    for (std::size_t index = 0; index < at_guest.size(); ++index)
      EXPECT_EQ(encoded_text(at_guest[index]),
                encoded_text(guest_expected[index]));
    EXPECT_EQ(host_acked, host_tickets);
    EXPECT_EQ(guest_acked, guest_tickets);

    std::size_t covered = 0;
    for (const auto& message : at_host)
      covered |= std::size_t{1} << message.index();
    for (const auto& message : at_guest)
      covered |= std::size_t{1} << message.index();
    constexpr auto all_types =
        (std::size_t{1} << std::variant_size_v<Message>)-1;
    EXPECT_EQ(covered, all_types);

    to_host.drain(host, limit, at_host);
    to_guest.drain(guest, limit, at_guest);
    EXPECT_EQ(at_host.size(), host_expected.size());
    EXPECT_EQ(at_guest.size(), guest_expected.size());
  }
}

TEST_F(ControlChannelExchange, DeliversEveryFixtureMessageWithoutLoss) {
  Pair pair{start};
  std::size_t host_sent = 0;
  std::size_t guest_sent = 0;
  for (const auto& [from_host, message] : fixture_messages()) {
    static_cast<void>(
        enqueued(from_host ? pair.host : pair.guest, message, start));
    ++(from_host ? host_sent : guest_sent);
  }
  for (int round = 0; round < 200 && !(pair.host.idle() && pair.guest.idle());
       ++round)
    pair.settle(start + static_cast<std::uint64_t>(round) * 100 * ms);
  EXPECT_EQ(pair.at_guest.size(), host_sent);
  EXPECT_EQ(pair.at_host.size(), guest_sent);
  EXPECT_EQ(host_sent + guest_sent, 18U);
}

}  // namespace
}  // namespace net_kakusan_test
