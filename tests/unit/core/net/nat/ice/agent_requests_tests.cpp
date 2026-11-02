#include "tests/unit/core/net/nat/ice/agent_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace nat_test {
namespace {

namespace event = libchiaking::net::nat::ice::event;
namespace stun = libchiaking::net::nat::stun;

class IceRequestHandling : public SeededIceAgentRig {
 protected:
  struct Direct {
    std::unique_ptr<test_support::SeededRandom> entropy;
    std::unique_ptr<Agent> agent;
    IceParameters local{placeholder()};
    IceParameters remote{placeholder()};
  };

  struct Crafted {
    std::optional<std::uint64_t> controlling;
    std::optional<std::uint64_t> controlled;
    bool use_candidate{};
    std::uint8_t id{1};
    std::optional<std::uint16_t> unknown;
    bool fingerprint{true};
    bool priority{true};
    std::string suffix{"0123456789abcdef"};
    std::string key;
  };

  static constexpr std::string_view peer_password =
      "00112233445566778899aabbccddeeff0011223344556677";
  static constexpr auto top = std::numeric_limits<std::uint64_t>::max();

  static auto peer_endpoint() -> Endpoint {
    return at(ipv4(198, 51, 100, 20), 6000);
  }

  static auto own_endpoint() -> Endpoint {
    return at(ipv4(203, 0, 113, 10), 5000);
  }

  auto direct_agent(std::uint64_t entropy_seed, TransportRole role) -> Direct {
    Direct direct;
    direct.entropy = std::make_unique<test_support::SeededRandom>(entropy_seed);
    AgentConfig config;
    config.role = role;
    config.bases.push_back(Base{1, own_endpoint(), 0});
    auto created = Agent::create(crypto, *direct.entropy, config.clone(), 0);
    EXPECT_TRUE(created);
    if (!created) return direct;
    direct.agent = std::move(created.value());
    auto gathered_event = direct.agent->take_event();
    EXPECT_TRUE(gathered_event);
    EXPECT_TRUE(gathered_event &&
                std::holds_alternative<event::Gathered>(*gathered_event));
    if (gathered_event &&
        std::holds_alternative<event::Gathered>(*gathered_event))
      direct.local = std::get<event::Gathered>(*gathered_event).local.clone();
    direct.remote = with_credentials(
        {libchiaking::net::kakusan::Candidate{
            "a", 2'000'000'000U, peer_endpoint(), CandidateType::host, {}}},
        0, libchiaking::SecretString{"0123456789abcdef"},
        libchiaking::SecretString{peer_password});
    return direct;
  }

  static auto request_with(std::optional<std::uint64_t> controlling,
                           std::optional<std::uint64_t> controlled,
                           bool use_candidate = false, std::uint8_t id = 1)
      -> Crafted {
    Crafted crafted;
    crafted.controlling = controlling;
    crafted.controlled = controlled;
    crafted.use_candidate = use_candidate;
    crafted.id = id;
    return crafted;
  }

  auto crafted_request(const Direct& direct, const Crafted& crafted)
      -> std::vector<std::byte> {
    stun::Message message;
    message.kind = stun::MessageClass::request;
    message.transaction =
        stun::TransactionId{std::byte{0x42}, std::byte{crafted.id}};
    message.username = direct.local.ufrag().clone();
    *message.username += ':';
    *message.username += crafted.suffix;
    if (crafted.priority) message.priority = 0x6e0000ffU;
    message.ice_controlling = crafted.controlling;
    message.ice_controlled = crafted.controlled;
    message.use_candidate = crafted.use_candidate;
    auto encoded = message.encode(crypto, {}, stun::Fingerprint::absent);
    EXPECT_TRUE(encoded);
    if (!encoded) return {};
    auto bytes = std::move(encoded.value());
    if (crafted.unknown) {
      const std::array<std::byte, 8> raw{
          static_cast<std::byte>(*crafted.unknown >> 8U),
          static_cast<std::byte>(*crafted.unknown & 0xffU),
          std::byte{0},
          std::byte{4},
          std::byte{1},
          std::byte{2},
          std::byte{3},
          std::byte{4}};
      bytes.insert(bytes.end(), raw.begin(), raw.end());
    }
    const auto key = crafted.key.empty() ? direct.local.password().view()
                                         : std::string_view{crafted.key};
    EXPECT_TRUE(stun::Message::append_integrity(
        crypto, bytes, libchiaking::net::kakusan::Hash::bytes_of(key)));
    if (crafted.fingerprint) stun::Message::append_fingerprint(bytes);
    return bytes;
  }

  auto responses_to(const std::vector<Outgoing>& outgoing, const Direct& direct)
      -> std::vector<stun::ReceivedMessage> {
    std::vector<stun::ReceivedMessage> responses;
    for (const auto& packet : outgoing) {
      auto decoded = stun::ReceivedMessage::decode(packet.bytes);
      EXPECT_TRUE(decoded);
      if (!decoded) continue;
      if (decoded.value().message.kind == stun::MessageClass::request) continue;
      EXPECT_EQ(packet.socket, 1U);
      EXPECT_EQ(packet.remote, peer_endpoint());
      EXPECT_TRUE(decoded.value().fingerprinted);
      const auto key = libchiaking::net::kakusan::Hash::bytes_of(
          direct.local.password().view());
      EXPECT_TRUE(decoded.value().verify_integrity(crypto, packet.bytes, key));
      responses.push_back(std::move(decoded.value()));
    }
    return responses;
  }

  auto exchange(Direct& direct, const Crafted& crafted, std::uint64_t now_us)
      -> std::vector<stun::ReceivedMessage> {
    std::vector<Outgoing> outgoing;
    direct.agent->receive(1, peer_endpoint(), crafted_request(direct, crafted),
                          now_us, outgoing);
    return responses_to(outgoing, direct);
  }

  auto signed_response(const stun::TransactionId& transaction,
                       std::optional<stun::ErrorResponse> error,
                       std::string_view key) -> std::vector<std::byte> {
    stun::Message message;
    message.transaction = transaction;
    if (error) {
      message.kind = stun::MessageClass::error;
      message.error = std::move(error);
    } else {
      message.kind = stun::MessageClass::success;
      message.xor_mapped = at(ipv4(203, 0, 113, 99), 41000);
    }
    auto encoded =
        message.encode(crypto, libchiaking::net::kakusan::Hash::bytes_of(key),
                       stun::Fingerprint::present);
    EXPECT_TRUE(encoded);
    if (!encoded) return {};
    return std::move(encoded.value());
  }

  auto first_check(Direct& direct, std::uint64_t now_us)
      -> stun::TransactionId {
    std::vector<Outgoing> outgoing;
    direct.agent->poll(now_us, outgoing);
    for (const auto& packet : outgoing) {
      const auto decoded = stun::ReceivedMessage::decode(packet.bytes);
      EXPECT_TRUE(decoded);
      if (!decoded) continue;
      if (decoded.value().message.kind != stun::MessageClass::request) continue;
      EXPECT_EQ(packet.remote, peer_endpoint());
      EXPECT_TRUE(decoded.value().verify_integrity(
          crypto, packet.bytes,
          libchiaking::net::kakusan::Hash::bytes_of(peer_password)));
      const auto& username = decoded.value().message.username;
      EXPECT_TRUE(username && username->view() ==
                                  std::string{direct.remote.ufrag().view()} +
                                      ":" +
                                      std::string{direct.local.ufrag().view()});
      return decoded.value().message.transaction;
    }
    ADD_FAILURE() << "the agent sent no connectivity check";
    return {};
  }

  static auto answered_with(const std::vector<stun::ReceivedMessage>& responses,
                            stun::MessageClass kind)
      -> ::testing::AssertionResult {
    if (responses.size() != 1)
      return ::testing::AssertionFailure()
             << responses.size() << " responses, expected one";
    if (responses[0].message.kind != kind)
      return ::testing::AssertionFailure()
             << "the response is of another class";
    return ::testing::AssertionSuccess();
  }

  const stun::ErrorResponse conflict{stun::error_code::role_conflict,
                                     "Role Conflict"};
};

TEST_P(IceRequestHandling, AnswersAnEarlyCheckOnceConnected) {
  auto direct = direct_agent(seed(), TransportRole::host);
  ASSERT_TRUE(direct.agent);
  EXPECT_TRUE(
      exchange(direct, request_with(std::nullopt, 5, false, 9), 0).empty());
  EXPECT_TRUE(direct.agent->connect(direct.remote, 10));
  std::vector<Outgoing> outgoing;
  direct.agent->poll(10, outgoing);
  const auto responses = responses_to(outgoing, direct);
  ASSERT_EQ(responses.size(), 1U);
  EXPECT_EQ(responses[0].message.kind, stun::MessageClass::success);
  EXPECT_EQ(responses[0].message.transaction[1], std::byte{9});
  ASSERT_TRUE(responses[0].message.xor_mapped);
  EXPECT_EQ(*responses[0].message.xor_mapped, peer_endpoint());
}

TEST_P(IceRequestHandling, ResolvesRoleConflictsByTieBreaker) {
  auto direct = direct_agent(seed(), TransportRole::host);
  ASSERT_TRUE(direct.agent);
  EXPECT_TRUE(direct.agent->connect(direct.remote, 0));
  EXPECT_EQ(direct.agent->role(), Role::controlling);
  auto responses = exchange(direct, request_with(0, std::nullopt), 1);
  ASSERT_TRUE(answered_with(responses, stun::MessageClass::error));
  EXPECT_EQ(responses[0].message.error->code, stun::error_code::role_conflict);
  EXPECT_EQ(direct.agent->role(), Role::controlling);
  responses = exchange(direct, request_with(top, std::nullopt), 2);
  EXPECT_TRUE(answered_with(responses, stun::MessageClass::success));
  EXPECT_EQ(direct.agent->role(), Role::controlled);
  responses = exchange(direct, request_with(std::nullopt, top), 3);
  ASSERT_TRUE(answered_with(responses, stun::MessageClass::error));
  EXPECT_EQ(responses[0].message.error->code, stun::error_code::role_conflict);
  EXPECT_EQ(direct.agent->role(), Role::controlled);
  responses = exchange(direct, request_with(std::nullopt, 0), 4);
  EXPECT_TRUE(answered_with(responses, stun::MessageClass::success));
  EXPECT_EQ(direct.agent->role(), Role::controlling);
}

TEST_P(IceRequestHandling, RefusesUnknownRequiredAttributesAndDropsBadChecks) {
  auto direct = direct_agent(seed(), TransportRole::host);
  ASSERT_TRUE(direct.agent);
  EXPECT_TRUE(direct.agent->connect(direct.remote, 0));
  static_cast<void>(exchange(direct, request_with(0, std::nullopt), 1));
  static_cast<void>(exchange(direct, request_with(top, std::nullopt), 2));
  static_cast<void>(exchange(direct, request_with(std::nullopt, top), 3));
  static_cast<void>(exchange(direct, request_with(std::nullopt, 0), 4));
  ASSERT_EQ(direct.agent->role(), Role::controlling);

  auto unknown = request_with(std::nullopt, 0, false, 5);
  unknown.unknown = 0x0014;
  const auto responses = exchange(direct, unknown, 5);
  ASSERT_EQ(responses.size(), 1U);
  ASSERT_TRUE(responses[0].message.error);
  EXPECT_EQ(responses[0].message.error->code,
            stun::error_code::unknown_attribute);
  EXPECT_EQ(responses[0].message.unknown_attributes,
            std::vector<std::uint16_t>{0x0014});

  auto dropped = request_with(std::nullopt, top);
  dropped.suffix = "fedcba9876543210";
  EXPECT_TRUE(exchange(direct, dropped, 6).empty());
  dropped = request_with(std::nullopt, top);
  dropped.fingerprint = false;
  EXPECT_TRUE(exchange(direct, dropped, 7).empty());
  dropped = request_with(std::nullopt, top);
  dropped.priority = false;
  EXPECT_TRUE(exchange(direct, dropped, 8).empty());
  dropped = request_with(std::nullopt, top);
  dropped.key = std::string(48, 'e');
  EXPECT_TRUE(exchange(direct, dropped, 9).empty());
  EXPECT_TRUE(exchange(direct, request_with(top, top), 10).empty());
  EXPECT_TRUE(
      exchange(direct, request_with(std::nullopt, std::nullopt), 11).empty());
  EXPECT_EQ(direct.agent->role(), Role::controlling);
  EXPECT_FALSE(direct.agent->take_event());
}

TEST_P(IceRequestHandling, SwitchesRoleOnlyOnAnAuthenticConflictResponse) {
  auto direct = direct_agent(seed(), TransportRole::host);
  ASSERT_TRUE(direct.agent);
  EXPECT_TRUE(direct.agent->connect(direct.remote, 0));
  const auto transaction = first_check(direct, 0);
  std::vector<Outgoing> outgoing;
  direct.agent->receive(
      1, peer_endpoint(),
      signed_response(transaction, conflict, std::string(48, 'e')), 1,
      outgoing);
  EXPECT_EQ(direct.agent->role(), Role::controlling);
  direct.agent->receive(1, peer_endpoint(),
                        signed_response(transaction, conflict, peer_password),
                        1, outgoing);
  EXPECT_EQ(direct.agent->role(), Role::controlled);
  EXPECT_TRUE(outgoing.empty());
}

TEST_P(IceRequestHandling, IgnoresAConflictResponseFromAnotherAddress) {
  std::vector<Outgoing> outgoing;
  auto asymmetric = direct_agent(seed() + 100, TransportRole::host);
  ASSERT_TRUE(asymmetric.agent);
  EXPECT_TRUE(asymmetric.agent->connect(asymmetric.remote, 0));
  const auto stray = first_check(asymmetric, 0);
  asymmetric.agent->receive(1, at(ipv4(198, 51, 100, 21), 6000),
                            signed_response(stray, conflict, peer_password), 1,
                            outgoing);
  asymmetric.agent->receive(1, peer_endpoint(),
                            signed_response(stray, conflict, peer_password), 1,
                            outgoing);
  EXPECT_EQ(asymmetric.agent->role(), Role::controlling);
  EXPECT_TRUE(outgoing.empty());
}

TEST_P(IceRequestHandling, NominatesOnceOnAControllingUseCandidate) {
  auto direct = direct_agent(seed(), TransportRole::guest);
  ASSERT_TRUE(direct.agent);
  EXPECT_TRUE(direct.agent->connect(direct.remote, 0));
  const auto transaction = first_check(direct, 0);
  std::vector<Outgoing> outgoing;
  direct.agent->receive(
      1, peer_endpoint(),
      signed_response(transaction, std::nullopt, peer_password), 1, outgoing);
  EXPECT_FALSE(direct.agent->take_event());
  auto responses =
      exchange(direct, request_with(top, std::nullopt, true, 20), 2);
  EXPECT_TRUE(answered_with(responses, stun::MessageClass::success));
  const auto nominated = direct.agent->take_event();
  ASSERT_TRUE(nominated);
  ASSERT_TRUE(std::holds_alternative<event::Nominated>(*nominated));
  const auto& path = std::get<event::Nominated>(*nominated).path;
  EXPECT_EQ(path.socket, 1U);
  EXPECT_EQ(path.remote, peer_endpoint());
  EXPECT_EQ(path.mapped, at(ipv4(203, 0, 113, 99), 41000));
  EXPECT_TRUE(direct.agent->consent());
  responses = exchange(direct, request_with(top, std::nullopt, true, 21), 3);
  EXPECT_TRUE(answered_with(responses, stun::MessageClass::success));
  EXPECT_FALSE(direct.agent->take_event());
  ASSERT_TRUE(direct.agent->nominated());
  EXPECT_TRUE(same_path(*direct.agent->nominated(), path));
}

INSTANTIATE_TEST_SUITE_P(Seeds, IceRequestHandling,
                         ::testing::Range<std::uint64_t>(1, 7));

class IceAgentApi : public IceAgentRig, public ::testing::Test {
 protected:
  test_support::SeededRandom entropy{42};
};

TEST_F(IceAgentApi, FailsAtOnceWithoutBases) {
  auto created = Agent::create(crypto, entropy, AgentConfig{}, 0);
  ASSERT_TRUE(created);
  auto& agent = *created.value();
  const auto failed = agent.take_event();
  ASSERT_TRUE(failed);
  ASSERT_TRUE(std::holds_alternative<event::Failed>(*failed));
  EXPECT_EQ(std::get<event::Failed>(*failed).error, Error::no_path);
  EXPECT_FALSE(agent.take_event());
  EXPECT_FALSE(agent.next_deadline_us());
  EXPECT_FALSE(agent.connect(
      remote_with_one_candidate(at(ipv4(198, 18, 0, 1), 9), 1), 0));
}

TEST_F(IceAgentApi, FailsAtOnceWithOnlyUnusableBases) {
  AgentConfig config;
  config.bases.push_back(Base{1, at(IpAddress::loopback_ipv4(), 5000), 0});
  config.bases.push_back(Base{2, at(IpAddress::any_ipv4(), 5000), 0});
  auto created = Agent::create(crypto, entropy, config.clone(), 0);
  ASSERT_TRUE(created);
  const auto failed = created.value()->take_event();
  ASSERT_TRUE(failed);
  EXPECT_TRUE(std::holds_alternative<event::Failed>(*failed));
}

TEST_F(IceAgentApi, RefusesTwoBasesWithOneSocket) {
  AgentConfig config;
  config.bases.push_back(Base{1, at(ipv4(203, 0, 113, 1), 5000), 0});
  config.bases.push_back(Base{1, at(ipv4(203, 0, 113, 2), 5000), 0});
  const auto created = Agent::create(crypto, entropy, config.clone(), 0);
  ASSERT_FALSE(created);
  EXPECT_EQ(created.error(), Error::internal);
}

TEST_F(IceAgentApi, RefusesEntropyThatIsAllZeros) {
  test_support::FixedRandom zero{std::byte{}};
  AgentConfig config;
  config.bases.push_back(Base{1, at(ipv4(203, 0, 113, 1), 5000), 0});
  const auto created = Agent::create(crypto, zero, config.clone(), 0);
  ASSERT_FALSE(created);
  EXPECT_EQ(created.error(), Error::crypto_unavailable);
}

TEST_F(IceAgentApi, ConnectsOnlyOnceAfterGatheringAndGoesQuietWhenClosed) {
  AgentConfig config;
  config.bases.push_back(Base{1, at(ipv4(203, 0, 113, 1), 5000), 0});
  config.stun_servers.push_back(dead_server());
  auto created = Agent::create(crypto, entropy, config.clone(), 0);
  ASSERT_TRUE(created);
  auto& agent = *created.value();
  EXPECT_FALSE(agent.take_event());
  ASSERT_TRUE(agent.next_deadline_us());
  EXPECT_EQ(*agent.next_deadline_us(), 0U);
  std::vector<Outgoing> outgoing;
  agent.poll(0, outgoing);
  ASSERT_EQ(outgoing.size(), 1U);
  EXPECT_EQ(outgoing[0].remote, dead_server());
  EXPECT_EQ(outgoing[0].socket, 1U);
  EXPECT_TRUE(stun::Message::recognises(outgoing[0].bytes));
  const auto remote = remote_with_one_candidate(at(ipv4(198, 18, 0, 1), 9), 1);
  const auto early = agent.connect(remote, 1);
  ASSERT_FALSE(early);
  EXPECT_EQ(early.error(), Error::protocol);
  outgoing.clear();
  agent.poll(libchiaking::net::kakusan::gathering_timeout_us, outgoing);
  const auto gathered_event = agent.take_event();
  ASSERT_TRUE(gathered_event);
  EXPECT_TRUE(std::holds_alternative<event::Gathered>(*gathered_event));
  const auto invalid = libchiaking::net::nat::ice::Credentials::make(
      libchiaking::SecretString{"XYZ"}, remote.password().clone());
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(),
            libchiaking::net::nat::ice::CredentialsError::invalid_ufrag);
  EXPECT_TRUE(
      agent.connect(remote, libchiaking::net::kakusan::gathering_timeout_us));
  EXPECT_FALSE(
      agent.connect(remote, libchiaking::net::kakusan::gathering_timeout_us));
  EXPECT_TRUE(agent.next_deadline_us());
  agent.close();
  EXPECT_FALSE(agent.next_deadline_us());
  EXPECT_FALSE(agent.nominated());
  EXPECT_FALSE(agent.consent());
  EXPECT_FALSE(agent.take_event());
  outgoing.clear();
  agent.poll(libchiaking::net::kakusan::gathering_timeout_us + second,
             outgoing);
  agent.receive(1, dead_server(), forged_request("a:b", "c", true), 0,
                outgoing);
  EXPECT_TRUE(outgoing.empty());
  EXPECT_FALSE(agent.connect(
      remote, libchiaking::net::kakusan::gathering_timeout_us + second));
}

class FailingEntropy final
    : public libchiaking::platform::SecureRandomProvider {
 public:
  explicit FailingEntropy(std::size_t successful_draws) noexcept
      : remaining_(successful_draws) {}

  auto fill_bytes(std::span<std::byte> output)
      -> libchiaking::Expected<void,
                               libchiaking::platform::PlatformError> override {
    std::fill(output.begin(), output.end(), std::byte{0x55});
    if (remaining_ == 0)
      return libchiaking::unexpected(
          libchiaking::platform::PlatformError::system_failure);
    --remaining_;
    return {};
  }

 private:
  std::size_t remaining_;
};

class IceAgentEntropy : public IceAgentRig, public ::testing::Test {
 protected:
  void SetUp() override {
    config.bases.push_back(Base{1, at(ipv4(192, 0, 2, 10), 50000), 0});
  }

  static void expect_stopped(Agent& agent, Error expected) {
    const auto stopped = agent.take_event();
    ASSERT_TRUE(stopped);
    ASSERT_TRUE(std::holds_alternative<event::Failed>(*stopped));
    EXPECT_EQ(std::get<event::Failed>(*stopped).error, expected);
    EXPECT_FALSE(agent.take_event());
    EXPECT_FALSE(agent.next_deadline_us());
    std::vector<Outgoing> outgoing;
    agent.poll(10 * second, outgoing);
    EXPECT_TRUE(outgoing.empty());
  }

  AgentConfig config;
};

TEST_F(IceAgentEntropy, RefusesToCreateWithoutCredentials) {
  FailingEntropy entropy{0};
  const auto agent = Agent::create(crypto, entropy, config.clone(), 0);
  ASSERT_FALSE(agent);
  EXPECT_EQ(agent.error(), Error::crypto_unavailable);
}

TEST_F(IceAgentEntropy, StopsWhenAServerRequestCannotBeDrawn) {
  FailingEntropy entropy{1};
  auto gathering = config.clone();
  gathering.stun_servers.push_back(stun_server());
  auto agent = Agent::create(crypto, entropy, std::move(gathering), 0);
  ASSERT_TRUE(agent);
  expect_stopped(*agent.value(), Error::crypto_unavailable);
}

TEST_F(IceAgentEntropy, StopsWhenACheckCannotBeDrawn) {
  FailingEntropy entropy{1};
  auto agent = Agent::create(crypto, entropy, config.clone(), 0);
  ASSERT_TRUE(agent);
  const auto gathered_event = agent.value()->take_event();
  ASSERT_TRUE(gathered_event);
  EXPECT_TRUE(std::holds_alternative<event::Gathered>(*gathered_event));
  EXPECT_TRUE(agent.value()->connect(
      remote_with_one_candidate(at(ipv4(192, 0, 2, 20), 50000), 1), 0));
  std::vector<Outgoing> outgoing;
  agent.value()->poll(second, outgoing);
  EXPECT_TRUE(outgoing.empty());
  expect_stopped(*agent.value(), Error::crypto_unavailable);
}

TEST_F(IceAgentEntropy, StopsWhenEntropyRepeatsAcrossServerRequests) {
  FailingEntropy entropy{100};
  auto gathering = config.clone();
  gathering.stun_servers = {stun_server(), dead_server()};
  auto agent = Agent::create(crypto, entropy, std::move(gathering), 0);
  ASSERT_TRUE(agent);
  expect_stopped(*agent.value(), Error::internal);
}

}  // namespace
}  // namespace nat_test
