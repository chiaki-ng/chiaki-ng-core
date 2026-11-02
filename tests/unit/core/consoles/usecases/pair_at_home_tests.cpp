#include "src/core/net/ports.hpp"
#include "src/core/net/protocol/remote_play/pairing/request.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/unit/core/consoles/usecases/home_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace consoles_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::AccountId;
using libchiaking::consoles::ConsoleName;
using libchiaking::consoles::Firmware;
using libchiaking::consoles::Generation;
using libchiaking::consoles::HomePairing;
using libchiaking::consoles::PairingFailure;
using libchiaking::platform::StopToken;
using Reason = PairingFailure::Reason;

static_assert(libchiaking::consoles::Host::limit == 255);
static_assert(HomePairing::host_limit == 255);

class PairAtHome : public ::testing::Test {
 protected:
  auto sighted_den() {
    return home.facts.sighted(
        {Home<>::den(), Firmware{Firmware::Family::ps5_1},
         ConsoleName::create("PS5-Den", ConsoleName::Source::discovered),
         Home<>::host("127.0.0.1"), false, std::nullopt});
  }

  Home<> home;
};

TEST_F(PairAtHome, NeverStartsWhenAlreadyStopped) {
  HomeConsole console{home.network};
  const auto user = home.signed_in.as("npsso", 7);
  const auto stop = std::make_shared<test_support::AtomicStop>();
  stop->stopped.store(true);
  stop->stopped.store(true);
  const auto result =
      home.known.pair_at_home(*user, Home<>::host("127.0.0.1"), Generation::ps5,
                              Home<>::pin(), StopToken{stop});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, Reason::cancelled);
  EXPECT_FALSE(result.error().http_status);
  EXPECT_FALSE(result.error().ps_reason);
  EXPECT_TRUE(home.network.sent().empty());
  EXPECT_TRUE(home.nothing_is_live());
}

TEST(PairAtHomeHost, MustBePrintableAscii) {
  for (const auto& host :
       {std::string{}, std::string{"bad host"}, std::string{"caf\xc3\xa9.lan"},
        std::string{"a\0b", 3}, std::string(256, 'a')})
    EXPECT_FALSE(libchiaking::consoles::Host::create(host, Generation::ps5))
        << host;
  EXPECT_EQ(
      libchiaking::consoles::Host::create("fe80::1", Generation::ps4).error(),
      libchiaking::consoles::Host::Invalid::ipv6_needs_ps5);
}

TEST_F(PairAtHome, RefusesAnAddressThatIsNotForTheGeneration) {
  const auto user = home.signed_in.as("npsso", 7);
  const auto wrong_generation =
      home.known.pair_at_home(*user, Home<>::host("fe80::1"), Generation::ps4,
                              Home<>::pin(), home.never);
  ASSERT_FALSE(wrong_generation);
  EXPECT_EQ(wrong_generation.error().reason,
            Reason::address_not_for_generation);
  EXPECT_TRUE(home.network.sent().empty());
}

TEST_F(PairAtHome, AnExpiredRunIsMalformedAndSendsNothing) {
  const auto user = home.signed_in.as("npsso", 7);
  HomePairing pairing{home.network, test_support::process_threading(),
                      home.crypto, home.clock, home.random};
  auto request = HomePairing::Request::of(
      *user, Firmware::newest(Generation::ps5), Home<>::pin());
  ASSERT_TRUE(request);
  const auto expired =
      pairing.run(std::move(request.value()), "127.0.0.1", home.never,
                  libchiaking::rp_session_port, std::chrono::milliseconds{0});
  ASSERT_FALSE(expired);
  EXPECT_EQ(expired.error().reason, Reason::malformed);
  EXPECT_TRUE(home.network.sent().empty());
}

TEST_F(PairAtHome, ReportsTheConsolesRefusal) {
  HomeConsole console{home.network};
  console.serve("RES3", HomeConsole::bytes(HomeConsole::refusal));
  const auto user = home.signed_in.as("npsso", 7);
  const auto result =
      home.known.pair_at_home(*user, Home<>::host("127.0.0.1"), Generation::ps5,
                              Home<>::pin(), home.never);
  console.stop_serving();
  ASSERT_FALSE(result);
  const auto& failure = result.error();
  EXPECT_EQ(failure.reason, Reason::refused);
  EXPECT_EQ(failure.http_status, std::uint16_t{403});
  EXPECT_EQ(failure.ps_reason, std::uint32_t{0x80108b12});
  EXPECT_FALSE(failure.internet_stage);
  EXPECT_FALSE(failure.unanswered);
  EXPECT_TRUE(console.knock.starts_with("SRC3")) << console.knock;
  EXPECT_TRUE(console.request_text().starts_with(
      "POST /sie/ps5/rp/sess/rgst HTTP/1.1\r\n"))
      << console.request_text();
  EXPECT_EQ(console.request_text().find("12345678"), std::string::npos);
  EXPECT_TRUE(home.nothing_is_live());
  EXPECT_EQ(home.signed_in.offline.requests, 0);
}

TEST_F(PairAtHome, AsksAPs4AtATypedAddressAsTheNewestFamily) {
  HomeConsole console{home.network};
  console.serve("RES2", HomeConsole::bytes(HomeConsole::refusal));
  const auto user = home.signed_in.as("npsso", 7);
  const auto result =
      home.known.pair_at_home(*user, Home<>::host("127.0.0.1", Generation::ps4),
                              Generation::ps4, Home<>::pin(), home.never);
  console.stop_serving();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, Reason::refused);
  EXPECT_EQ(console.knock.substr(0, 4), "SRC2");
  const auto request = console.request_text();
  EXPECT_TRUE(request.starts_with("POST /sie/ps4/rp/sess/rgst HTTP/1.1\r\n"))
      << request;
  EXPECT_NE(request.find("RP-Version: 10.0\r\n"), std::string::npos);
}

TEST_F(PairAtHome, PairsAPs4BelowSevenByOnlineId) {
  HomeConsole console{home.network};
  console.serve("RES2", HomeConsole::bytes(HomeConsole::refusal));
  const auto found = home.facts.sighted(
      {libchiaking::consoles::HardwareId::parse("001122334455").value(),
       Firmware::from_system_version(Generation::ps4, "06720001"),
       ConsoleName::create("PS4-Loft", ConsoleName::Source::discovered),
       Home<>::host("127.0.0.1", Generation::ps4), false, std::nullopt});
  EXPECT_EQ(found->firmware().family(), Firmware::Family::ps4_8);

  const auto sofa = home.signed_in.users.add_home_user(AccountId{77}, "Sofa");
  ASSERT_TRUE(sofa);
  const auto unnamed =
      home.known.pair_at_home(*sofa.value(), *found, Home<>::pin(), home.never);
  ASSERT_FALSE(unnamed);
  EXPECT_EQ(unnamed.error().reason, Reason::needs_online_id);
  EXPECT_TRUE(home.network.sent().empty());

  const auto player = home.signed_in.as("npsso", 7);
  const auto result =
      home.known.pair_at_home(*player, *found, Home<>::pin(), home.never);
  console.stop_serving();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, Reason::refused);
  EXPECT_EQ(result.error().http_status, std::uint16_t{403});
  EXPECT_EQ(console.knock.substr(0, 4), "SRC2");
  const auto request = console.request_text();
  EXPECT_EQ(request.find("POST /sce/rp/regist HTTP/1.1\r\n"), 0U);
  EXPECT_NE(request.find("Content-Length: 524\r\n"), std::string::npos);
  EXPECT_FALSE(found->is_paired());
  EXPECT_EQ(home.signed_in.offline.requests, 0);
}

TEST_F(PairAtHome, PairsAHomeUserWithoutAskingPsn) {
  HomeConsole console{home.network};
  console.serve("RES3", home.welcome());
  const auto sofa = home.signed_in.users.add_home_user(AccountId{77}, "Sofa");
  ASSERT_TRUE(sofa);
  EXPECT_FALSE(sofa.value()->has_psn_account());
  const auto result =
      home.known.pair_at_home(*sofa.value(), Home<>::host("127.0.0.1"),
                              Generation::ps5, Home<>::pin(), home.never);
  console.stop_serving();
  ASSERT_TRUE(result);
  const auto& paired = result.value();
  EXPECT_EQ(home.signed_in.offline.requests, 0);
  EXPECT_EQ(paired->hardware_id(), Home<>::den());
  EXPECT_EQ(paired->generation(), Generation::ps5);
  EXPECT_EQ(paired->firmware(), Firmware{Firmware::Family::ps5_1});
  EXPECT_EQ(paired->name(), "Den");
  EXPECT_EQ(paired->name_source(), ConsoleName::Source::pairing);
  EXPECT_EQ(paired->last_address(), Home<>::host("127.0.0.1"));
  EXPECT_TRUE(paired->is_saved());
  EXPECT_TRUE(paired->is_paired_with(*sofa.value()));
  EXPECT_EQ(home.known.find(Home<>::den()), paired);
  EXPECT_EQ(home.known.everything().size(), 1U);
  EXPECT_TRUE(home.store.console(Home<>::den()));
  EXPECT_TRUE(home.store.pairing(AccountId{77}, Home<>::den()));
  const auto keys = home.facts.keys_of(*paired, *sofa.value());
  ASSERT_TRUE(keys);
  EXPECT_EQ(keys->key_type(), 2U);
  EXPECT_EQ(keys->registration_key()[0], std::byte{1});
  EXPECT_EQ(keys->registration_key()[1], std::byte{2});
  EXPECT_EQ(keys->remote_play_key()[15], std::byte{0xff});
}

TEST_F(PairAtHome, SendsTheHomeUsersAccountInTheRequest) {
  HomeConsole console{home.network};
  console.serve("RES3", home.welcome());
  const auto sofa = home.signed_in.users.add_home_user(AccountId{77}, "Sofa");
  ASSERT_TRUE(sofa);
  ASSERT_TRUE(home.known.pair_at_home(*sofa.value(), Home<>::host("127.0.0.1"),
                                      Generation::ps5, Home<>::pin(),
                                      home.never));
  console.stop_serving();
  const auto account = AccountId{77}.bytes();
  const auto material = HomeConsole::material(
      remote_play::RegistrationTarget::ps5_1, Home<>::digits);
  const auto payload = remote_play::RegistrationRequestCodec::encode_payload(
                           home.crypto, remote_play::RegistrationTarget::ps5_1,
                           material, account)
                           .value();
  const auto header =
      remote_play::RegistrationRequestCodec::encode_header(
          remote_play::RegistrationTarget::ps5_1, payload.size())
          .value();
  auto expected = HomeConsole::bytes(header);
  expected.insert(expected.end(), payload.begin(), payload.end());
  EXPECT_EQ(console.request, expected);
  EXPECT_EQ(console.knock, std::string("SRC3\0", 5));
}

TEST_F(PairAtHome, PairsTheConsoleThatWasFound) {
  HomeConsole console{home.network};
  console.serve("RES3", home.welcome());
  const auto found = sighted_den();
  EXPECT_FALSE(found->is_paired());
  EXPECT_FALSE(found->is_saved());
  const auto user = home.signed_in.as("npsso", 7);
  const auto result =
      home.known.pair_at_home(*user, *found, Home<>::pin(), home.never);
  console.stop_serving();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value(), found);
  EXPECT_TRUE(found->is_paired_with(*user));
  EXPECT_TRUE(found->is_saved());
  EXPECT_EQ(found->name(), "PS5-Den");
  EXPECT_TRUE(home.store.pairing(AccountId{7}, Home<>::den()));
  EXPECT_EQ(home.signed_in.offline.requests, 0);
}

TEST_F(PairAtHome, CannotReachAConsoleWithNoAddress) {
  const auto user = home.signed_in.as("npsso", 7);
  const auto other = home.signed_in.as("npsso-other", 9);
  std::array<std::byte,
             libchiaking::consoles::PairingKeys::registration_key_size>
      registration_key{};
  std::array<std::byte,
             libchiaking::consoles::PairingKeys::remote_play_key_size>
      remote_play_key{};
  registration_key.fill(std::byte{1});
  remote_play_key.fill(std::byte{2});
  const auto console = home.facts.paired(
      *user, libchiaking::consoles::PairedConsole{
                 Home<>::den(), Firmware{Firmware::Family::ps5_1}, std::nullopt,
                 libchiaking::consoles::PairingKeys::create(registration_key,
                                                            remote_play_key, 2)
                     .value()});
  ASSERT_TRUE(console);
  EXPECT_FALSE(console.value()->last_address());
  const auto result = home.known.pair_at_home(*other, *console.value(),
                                              Home<>::pin(), home.never);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, Reason::unreachable);
  EXPECT_TRUE(home.network.sent().empty());
}

class PairAtHomeFailedSave : public ::testing::TestWithParam<bool> {
 protected:
  Home<> home;
};

TEST_P(PairAtHomeFailedSave, LeavesNothingLive) {
  const bool consoles_refused = GetParam();
  HomeConsole console{home.network};
  console.serve("RES3", home.welcome());
  home.store.refuse_console_saves = consoles_refused;
  home.store.refuse_pairing_saves = !consoles_refused;
  const auto user = home.signed_in.as("npsso", 7);
  const auto result =
      home.known.pair_at_home(*user, Home<>::host("127.0.0.1"), Generation::ps5,
                              Home<>::pin(), home.never);
  console.stop_serving();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, Reason::not_saved);
  EXPECT_FALSE(result.error().http_status);
  EXPECT_FALSE(result.error().ps_reason);
  EXPECT_TRUE(console.request_complete());
  EXPECT_TRUE(home.nothing_is_live());
  EXPECT_FALSE(home.known.find(Home<>::den()));
}

INSTANTIATE_TEST_SUITE_P(PairingOrConsoleRefused, PairAtHomeFailedSave,
                         ::testing::Bool());

TEST_F(PairAtHome, LeavesAFoundConsoleUnpairedWhenTheSaveFails) {
  HomeConsole console{home.network};
  console.serve("RES3", home.welcome());
  const auto found = sighted_den();
  home.store.refuse_pairing_saves = true;
  const auto user = home.signed_in.as("npsso", 7);
  const auto result =
      home.known.pair_at_home(*user, *found, Home<>::pin(), home.never);
  console.stop_serving();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, Reason::not_saved);
  EXPECT_FALSE(found->is_paired());
  EXPECT_FALSE(found->is_saved());
  EXPECT_EQ(home.store.console_count(), 0U);
  EXPECT_EQ(home.store.pairing_count(), 0U);
}

TEST_F(PairAtHome, AReplyThatCannotBeReadIsNotAPairing) {
  HomeConsole console{home.network};
  console.serve("RES3", HomeConsole::welcome(
                            home.crypto, remote_play::RegistrationTarget::ps5_1,
                            Home<>::digits + 1, HomeConsole::ps5_facts));
  const auto user = home.signed_in.as("npsso", 7);
  const auto result =
      home.known.pair_at_home(*user, Home<>::host("127.0.0.1"), Generation::ps5,
                              Home<>::pin(), home.never);
  console.stop_serving();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().reason, Reason::malformed);
  EXPECT_TRUE(home.nothing_is_live());
}

TEST_F(PairAtHome, NobodyAtTheAddressIsUnreachable) {
  const auto user = home.signed_in.as("npsso", 7);
  const auto absent =
      home.known.pair_at_home(*user, Home<>::host("127.0.0.1"), Generation::ps5,
                              Home<>::pin(), home.never);
  ASSERT_FALSE(absent);
  EXPECT_EQ(absent.error().reason, Reason::unreachable);
  const auto unresolved =
      home.known.pair_at_home(*user, Home<>::host("unresolvable.invalid"),
                              Generation::ps5, Home<>::pin(), home.never);
  ASSERT_FALSE(unresolved);
  EXPECT_EQ(unresolved.error().reason, Reason::unreachable);
  EXPECT_TRUE(home.nothing_is_live());
}

}  // namespace
}  // namespace consoles_test
