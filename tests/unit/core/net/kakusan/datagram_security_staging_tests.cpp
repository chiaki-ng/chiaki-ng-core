#include "tests/unit/core/net/kakusan/datagram_security_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace net_kakusan_test {
namespace {

class SecureDatagramStaging : public DatagramSecurityRig {
 protected:
  static constexpr std::uint64_t begin = 5 * sec;

  static auto relative(std::optional<std::uint64_t> deadline, std::uint64_t now)
      -> std::int64_t {
    return deadline ? static_cast<std::int64_t>(*deadline) -
                          static_cast<std::int64_t>(now)
                    : std::int64_t{-1};
  }

  static auto watch(Pair& pair, std::uint64_t from, std::size_t polls,
                    bool deliver_them) -> std::string {
    std::string trace;
    for (std::size_t index = 1; index <= polls; ++index) {
      const auto now = from + index * 250 * ms;
      const bool client_ok = static_cast<bool>(pair.client->poll(now));
      const bool server_ok = static_cast<bool>(pair.server->poll(now));
      auto from_client = drain(*pair.client);
      auto from_server = drain(*pair.server);
      trace += std::format(" [{}{} c{} s{} {} {}]", client_ok ? "" : "C!",
                           server_ok ? "" : "S!", from_client.size(),
                           from_server.size(),
                           relative(pair.client->next_deadline_us(), now),
                           relative(pair.server->next_deadline_us(), now));
      if (deliver_them) {
        deliver(*pair.server, from_client, now);
        deliver(*pair.client, from_server, now);
      }
    }
    trace += std::format(" e{}{}", pair.client->established() ? 1 : 0,
                         pair.server->established() ? 1 : 0);
    return trace;
  }

  using Scenario = std::function<std::string()>;

  static auto scenarios()
      -> std::vector<std::pair<std::string_view, Scenario>> {
    return {
        {"client_hello_lost",
         [] {
           auto pair = make_pair();
           start(pair, begin);
           const auto lost = drain(*pair.client).size();
           return std::format("lost{}", lost) + watch(pair, begin, 8, true);
         }},
        {"server_flight_lost",
         [] {
           auto pair = make_pair();
           start(pair, begin);
           deliver(*pair.server, drain(*pair.client), begin);
           const auto lost = drain(*pair.server).size();
           return std::format("lost{}", lost) + watch(pair, begin, 8, true);
         }},
        {"final_flight_lost",
         [] {
           auto pair = make_pair();
           start(pair, begin);
           deliver(*pair.server, drain(*pair.client), begin);
           deliver(*pair.client, drain(*pair.server), begin);
           deliver(*pair.server, drain(*pair.client), begin);
           const auto lost = drain(*pair.server).size();
           return std::format("lost{}", lost) + watch(pair, begin, 8, true);
         }},
        {"nobody_answers",
         [] {
           auto pair = make_pair();
           start(pair, begin);
           const auto lost = drain(*pair.client).size();
           return std::format("lost{}", lost) + watch(pair, begin, 24, false);
         }},
    };
  }

  static auto pins()
      -> std::vector<std::pair<std::string_view, std::string_view>> {
    return {
        {"client_hello_lost",
         "lost1 [ c0 s0 150000 14750000] [ c1 s0 800000 14500000]"
         " [ c0 s1 550000 150000] [ c1 s5 150000 800000]"
         " [ c5 s1 800000 -1] [ c0 s2 -1 -1] [ c0 s0 -1 -1]"
         " [ c0 s0 -1 -1] e11"},
        {"server_flight_lost",
         "lost1 [ c0 s0 150000 150000] [ c1 s5 800000 800000]"
         " [ c1 s0 150000 550000] [ c5 s1 800000 -1] [ c0 s2 -1 -1]"
         " [ c0 s0 -1 -1] [ c0 s0 -1 -1] [ c0 s0 -1 -1] e11"},
        {"final_flight_lost",
         "lost1 [ c0 s0 150000 -1] [ c5 s0 800000 -1]"
         " [ c0 s2 550000 -1] [ c0 s0 -1 -1] [ c0 s0 -1 -1]"
         " [ c0 s0 -1 -1] [ c0 s0 -1 -1] [ c0 s0 -1 -1] e11"},
        {"nobody_answers",
         "lost1 [ c0 s0 150000 14750000] [ c1 s0 800000 14500000]"
         " [ c0 s0 550000 14250000] [ c0 s0 300000 14000000]"
         " [ c0 s0 50000 13750000] [ c1 s0 1600000 13500000]"
         " [ c0 s0 1350000 13250000] [ c0 s0 1100000 13000000]"
         " [ c0 s0 850000 12750000] [ c0 s0 600000 12500000]"
         " [ c0 s0 350000 12250000] [ c0 s0 100000 12000000]"
         " [ c1 s0 3200000 11750000] [ c0 s0 2950000 11500000]"
         " [ c0 s0 2700000 11250000] [ c0 s0 2450000 11000000]"
         " [ c0 s0 2200000 10750000] [ c0 s0 1950000 10500000]"
         " [ c0 s0 1700000 10250000] [ c0 s0 1450000 10000000]"
         " [ c0 s0 1200000 9750000] [ c0 s0 950000 9500000]"
         " [ c0 s0 700000 9250000] [ c0 s0 450000 9000000] e00"},
    };
  }
};

TEST_F(SecureDatagramStaging, EveryLossRetransmitsWhereTheTableSays) {
  const auto table = pins();
  std::size_t matched = 0;
  for (const auto& [name, scenario] : scenarios()) {
    const auto actual = scenario();
    const auto pin = std::ranges::find_if(
        table, [&](const auto& item) { return item.first == name; });
    if (pin == table.end()) {
      ADD_FAILURE() << "{\"" << name << "\", \"" << actual << "\"},";
      continue;
    }
    ++matched;
    EXPECT_EQ(actual, pin->second) << name;
  }
  EXPECT_EQ(matched, table.size());
}

}  // namespace
}  // namespace net_kakusan_test
