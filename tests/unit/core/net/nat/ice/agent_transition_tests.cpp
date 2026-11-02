#include "tests/unit/core/net/nat/ice/agent_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace nat_test {
namespace {

namespace event = libchiaking::net::nat::ice::event;

class IceAgentTransitions : public IceAgentRig, public ::testing::Test {
 protected:
  static auto trace(Session& session) -> std::string {
    auto& sim = *session.sim;
    auto text = std::format("now {}", sim.now());
    for (const auto peer : {session.host, session.guest}) {
      text += " |";
      for (const auto& record : sim.events(peer)) {
        text += std::format(" {}@{}", record.event.index(), record.at_us);
        if (const auto* failed = std::get_if<event::Failed>(&record.event))
          text += std::format("e{}", static_cast<int>(failed->error));
      }
      auto& agent = sim.agent(peer);
      const auto deadline = agent.next_deadline_us();
      text += std::format(" n{} c{} q{} d{}", agent.nominated() ? 1 : 0,
                          agent.consent() ? 1 : 0, agent.check_requests_sent(),
                          deadline ? static_cast<std::int64_t>(*deadline) -
                                         static_cast<std::int64_t>(sim.now())
                                   : -1);
    }
    return text;
  }

  using Scenario = std::function<std::string(IceAgentTransitions&)>;

  static auto scenarios()
      -> std::vector<std::pair<std::string_view, Scenario>> {
    return {
        {"public",
         [](IceAgentTransitions& rig) {
           auto session = rig.prepare(11, Topology{});
           session.sim->run_for(5 * second);
           return trace(session);
         }},
        {"port_restricted",
         [](IceAgentTransitions& rig) {
           auto session = rig.prepare(
               12, Topology{NatKind::port_restricted, NatKind::none, true});
           session.sim->run_for(5 * second);
           return trace(session);
         }},
        {"symmetric_both",
         [](IceAgentTransitions& rig) {
           auto session = rig.prepare(
               13, Topology{NatKind::symmetric, NatKind::symmetric, true});
           session.sim->run_for(40 * second);
           return trace(session);
         }},
        {"no_stun",
         [](IceAgentTransitions& rig) {
           auto session = rig.prepare(
               14, Topology{NatKind::full_cone, NatKind::none, false});
           session.sim->run_for(5 * second);
           return trace(session);
         }},
        {"guest_head_start",
         [](IceAgentTransitions& rig) {
           Topology topology{NatKind::address_restricted, NatKind::none, true};
           topology.head_start_us = 2 * second;
           auto session = rig.prepare(15, topology);
           session.sim->run_for(5 * second);
           return trace(session);
         }},
        {"lossy",
         [](IceAgentTransitions& rig) {
           Topology topology{NatKind::none, NatKind::none, true};
           topology.loss_permille = 300;
           auto session = rig.prepare(16, topology);
           session.sim->run_for(10 * second);
           return trace(session);
         }},
        {"wrong_password",
         [](IceAgentTransitions& rig) {
           auto session = rig.prepare_with(17, [](const IceParameters& ice) {
             return with_credentials(
                 ice, libchiaking::SecretString{"ffffffffffffffff"},
                 libchiaking::SecretString{std::string(48, 'e')});
           });
           session.sim->run_for(
               libchiaking::net::nat::ice::connectivity_timeout_us +
               2 * second);
           return trace(session);
         }},
        {"connect_twice",
         [](IceAgentTransitions& rig) {
           auto session = rig.prepare(18, Topology{});
           auto& sim = *session.sim;
           const auto again =
               sim.agent(session.host).connect(session.guest_ice, sim.now());
           return std::format("again {} ", again ? 0 : 1) + trace(session);
         }},
        {"connect_before_gathering",
         [](IceAgentTransitions& rig) {
           Session session;
           session.sim = std::make_unique<Simulation>(rig.crypto, 19);
           auto& sim = *session.sim;
           sim.network().add_stun_server(dead_server(), false);
           session.host = sim.add_peer(
               TransportRole::host,
               {public_peer(at(ipv4(203, 0, 113, 10), 5000))}, {dead_server()});
           session.guest =
               sim.add_peer(TransportRole::guest,
                            {public_peer(at(ipv4(198, 51, 100, 20), 6000))},
                            {dead_server()});
           const auto early =
               sim.agent(session.host).connect(placeholder(), sim.now());
           auto text = std::format("early {} ", early ? 0 : 1);
           sim.run_for(libchiaking::net::kakusan::gathering_timeout_us +
                       second);
           return text + trace(session);
         }},
        {"closed_after_nomination",
         [](IceAgentTransitions& rig) {
           auto session = rig.prepare(20, Topology{});
           auto& sim = *session.sim;
           static_cast<void>(
               run_to_nomination(session, sim.now() + 10 * second));
           sim.agent(session.host).close();
           sim.run_for(40 * second);
           const auto again =
               sim.agent(session.host).connect(session.guest_ice, sim.now());
           return std::format("again {} ", again ? 0 : 1) + trace(session);
         }},
        {"closed_before_connect",
         [](IceAgentTransitions& rig) {
           auto fresh = rig.prepare_public(21);
           auto& sim = *fresh.sim;
           sim.agent(fresh.host).close();
           const auto again =
               sim.agent(fresh.host).connect(fresh.guest_ice, sim.now());
           EXPECT_TRUE(
               sim.agent(fresh.guest).connect(fresh.host_ice, sim.now()));
           sim.run_for(5 * second);
           return std::format("again {} ", again ? 0 : 1) + trace(fresh);
         }},
        {"consent_lost",
         [](IceAgentTransitions& rig) {
           auto session = rig.prepare(22, Topology{});
           auto& sim = *session.sim;
           static_cast<void>(
               run_to_nomination(session, sim.now() + 10 * second));
           sim.network().set_cut(true);
           sim.run_for(40 * second);
           return trace(session);
         }},
    };
  }

  static auto pins()
      -> std::vector<std::pair<std::string_view, std::string_view>> {
    return {
        {"public",
         "now 5080000"
         " | 0@40000 1@170000 n1 c1 q2 d285954"
         " | 0@80000 1@150000 n1 c1 q1 d4823326"},
        {"port_restricted",
         "now 5080000"
         " | 0@40000 1@170000 n1 c1 q2 d323791"
         " | 0@80000 1@170000 n1 c1 q2 d424981"},
        {"symmetric_both",
         "now 40080000"
         " | 0@40000 2@30080000e1 n0 c0 q10 d-1"
         " | 0@80000 n0 c0 q30 d3650000"},
        {"no_stun",
         "now 5000000"
         " | 0@0 1@90000 n1 c1 q2 d725640"
         " | 0@0 1@90000 n1 c1 q2 d3407428"},
        {"guest_head_start",
         "now 7080000"
         " | 0@40000 1@2170000 n1 c1 q2 d4747837"
         " | 0@80000 1@2150000 n1 c1 q7 d160127"},
        {"lossy",
         "now 10080000"
         " | 0@40000 1@170000 n1 c1 q2 d218840"
         " | 0@80000 1@1720000 n1 c1 q5 d1275280"},
        {"wrong_password",
         "now 32000000"
         " | 0@0 2@30000000e1 n0 c0 q5 d-1"
         " | 0@0 n0 c0 q13 d500000"},
        {"connect_twice",
         "again 1 now 80000"
         " | 0@40000 n0 c0 q0 d0"
         " | 0@80000 n0 c0 q1 d500000"},
        {"connect_before_gathering",
         "early 1 now 16000000"
         " | 0@15000000 n0 c0 q0 d-1"
         " | 0@15000000 n0 c0 q0 d-1"},
        {"closed_after_nomination",
         "again 1 now 40170000"
         " | 0@40000 1@170000 n0 c0 q2 d-1"
         " | 0@80000 1@150000 2@30150000e1 n1 c0 q1 d-1"},
        {"closed_before_connect",
         "again 1 now 5000000"
         " | 0@0 n0 c0 q0 d-1"
         " | 0@0 n0 c0 q4 d2500000"},
        {"consent_lost",
         "now 40170000"
         " | 0@40000 1@170000 2@30170000e1 n1 c0 q2 d-1"
         " | 0@80000 1@150000 2@30150000e1 n1 c0 q1 d-1"},
    };
  }
};

TEST_F(IceAgentTransitions, EveryScenarioEndsWhereTheTableSays) {
  const auto table = pins();
  for (const auto& [name, scenario] : scenarios()) {
    const auto actual = scenario(*this);
    const auto pin = std::ranges::find_if(
        table, [&](const auto& item) { return item.first == name; });
    if (pin == table.end()) {
      ADD_FAILURE() << "{\"" << name << "\", \"" << actual << "\"},";
      continue;
    }
    EXPECT_EQ(actual, pin->second) << name;
  }
}

}  // namespace
}  // namespace nat_test
