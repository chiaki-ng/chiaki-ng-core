#include "libchiaking/platform/net/default.hpp"
#include "libchiaking/testing/service_builder.hpp"
#include "tests/support/crypto/refusing.hpp"
#include "tests/unit/core/consoles/known_consoles_rig.hpp"

#include <libchiaking/consoles_types.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace consoles_test {
namespace {

using libchiaking::AccountId;
using libchiaking::consoles::AccountConsole;
using libchiaking::consoles::ConsoleName;
using libchiaking::consoles::DeviceUid;
using libchiaking::consoles::Generation;
using libchiaking::consoles::InternetPairingStage;
using libchiaking::consoles::PairingFailure;
using libchiaking::consoles::PairingStep;
using PairingReason = PairingFailure::Reason;
using Stages = std::vector<InternetPairingStage>;
using Steps = std::vector<PairingStep>;

class ScriptedPairing final : public libchiaking::consoles::InternetPairing {
 public:
  static constexpr std::string_view den = "aa:bb:cc:dd:ee:01";
  static constexpr std::string_view attic = "aa:bb:cc:dd:ee:02";

  auto pair(libchiaking::users::PsUser& as, const DeviceUid& which,
            Generation generation,
            libchiaking::consoles::PairingWatcher* watcher,
            const libchiaking::platform::StopToken&)
      -> libchiaking::Expected<libchiaking::consoles::PairedConsole,
                               PairingFailure> override {
    ++calls;
    asked_as = as.account_id();
    asked = which;
    asked_generation = generation;
    if (watcher != nullptr) {
      watcher->on_stage(InternetPairingStage::opening_session);
      watcher->on_progress({.step = PairingStep::opening_account_session});
      watcher->on_stage(InternetPairingStage::pairing);
      watcher->on_progress({.step = PairingStep::sending_request});
    }
    if (failure) return libchiaking::unexpected(*failure);
    std::optional<ConsoleName> named;
    if (!nickname.empty())
      named = Values::name(nickname, ConsoleName::Source::pairing);
    return libchiaking::consoles::PairedConsole{
        Values::hardware_id(answers),
        libchiaking::consoles::Firmware{
            generation == Generation::ps4
                ? libchiaking::consoles::Firmware::Family::ps4_10
                : libchiaking::consoles::Firmware::Family::ps5_1},
        std::move(named), Values::keys(5)};
  }

  int calls{};
  std::optional<AccountId> asked_as;
  std::optional<DeviceUid> asked;
  std::optional<Generation> asked_generation;
  std::optional<PairingFailure> failure;
  std::string_view answers{den};
  std::string nickname;
};

class StagesHeard final : public libchiaking::consoles::PairingWatcher {
 public:
  void on_stage(InternetPairingStage stage) override {
    stages.push_back(stage);
  }

  void on_progress(
      const libchiaking::consoles::PairingProgress& progress) override {
    steps.push_back(progress.step);
  }

  Stages stages;
  Steps steps;
};

class PairOverTheInternet : public ::testing::Test, public Values {
 protected:
  static constexpr std::string_view den = ScriptedPairing::den;
  static constexpr std::string_view attic = ScriptedPairing::attic;

  static auto uid(std::uint8_t seed) -> DeviceUid {
    std::array<std::byte, DeviceUid::size> bytes{};
    bytes.fill(static_cast<std::byte>(seed));
    return DeviceUid::from_bytes(bytes).value();
  }

  static auto listed(std::uint8_t seed, std::string named,
                     bool remote_play_enabled = true) -> AccountConsole {
    return AccountConsole::create(uid(seed), std::move(named), Generation::ps5,
                                  remote_play_enabled)
        .value();
  }

  libchiaking::platform::DefaultThreadingProvider threading;
  libchiaking::platform::SteadyClock steady;
  test_support::users::SignedIn signed_in;
  libchiaking::platform::DefaultNetworkProvider network{threading, steady};
  test_support::NoCrypto crypto;
  libchiaking::platform::SystemSecureRandom random;
  libchiaking::testing::InMemoryConsoleStore store{threading};
  ScriptedPairing pairing;
  libchiaking::consoles::KnownConsoles consoles{
      libchiaking::testing::ServiceBuilder::build<
          libchiaking::consoles::KnownConsoles>(network, signed_in.offline,
                                                crypto, random, store,
                                                threading, steady, &pairing)};
  const libchiaking::platform::StopToken stop;
};

TEST(PairOverTheInternetWithoutAPort, IsAnsweredWithoutARequest) {
  Known known;
  const auto user = known.signed_in.as("npsso", 7);
  const libchiaking::platform::StopToken stop;
  const auto answered = known.consoles.pair_over_the_internet(
      *user, AccountConsole::main_ps4(), nullptr, stop);
  ASSERT_FALSE(answered);
  EXPECT_EQ(answered.error().reason, PairingReason::internet_pairing_missing);
  EXPECT_EQ(known.signed_in.offline.requests, 0);
  EXPECT_TRUE(known.consoles.everything().empty());
}

TEST_F(PairOverTheInternet, NeverReportsSavingWhenThePairingFails) {
  const auto user = signed_in.as("npsso", 7);
  StagesHeard heard;
  pairing.failure = PairingFailure{PairingReason::timed_out};
  const auto failed = consoles.pair_over_the_internet(
      *user, listed(0x11, "Living Room"), &heard, stop);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().reason, PairingReason::timed_out);
  EXPECT_EQ(heard.steps, (Steps{PairingStep::opening_account_session,
                                PairingStep::sending_request}));
}

TEST_F(PairOverTheInternet, AsksForTheListedConsoleAsTheUser) {
  const auto user = signed_in.as("npsso", 7);
  StagesHeard heard;
  pairing.nickname = "Den PS5";
  const auto which = listed(0x11, "Living Room");
  ASSERT_TRUE(consoles.pair_over_the_internet(*user, which, &heard, stop));
  EXPECT_EQ(pairing.calls, 1);
  EXPECT_EQ(pairing.asked_as, AccountId{7});
  EXPECT_EQ(pairing.asked, which.device_uid());
  EXPECT_EQ(pairing.asked_generation, Generation::ps5);
  EXPECT_EQ(heard.stages, (Stages{InternetPairingStage::opening_session,
                                  InternetPairingStage::pairing}));
  EXPECT_EQ(heard.steps,
            (Steps{PairingStep::opening_account_session,
                   PairingStep::sending_request, PairingStep::saving}));
}

TEST_F(PairOverTheInternet, PairsAndSavesAConsoleOnTheAccount) {
  const auto user = signed_in.as("npsso", 7);
  pairing.nickname = "Den PS5";
  const auto which = listed(0x11, "Living Room");
  const auto paired =
      consoles.pair_over_the_internet(*user, which, nullptr, stop);
  ASSERT_TRUE(paired);
  const auto& console = paired.value();
  EXPECT_EQ(console->hardware_id(), hardware_id(den));
  EXPECT_EQ(console->generation(), Generation::ps5);
  EXPECT_TRUE(console->is_saved());
  EXPECT_TRUE(console->is_paired_with(*user));
  EXPECT_EQ(console->name(), "Den PS5");
  EXPECT_EQ(console->name_source(), ConsoleName::Source::pairing);
  EXPECT_EQ(consoles.find(hardware_id(den)), console);
  const auto row = store.pairing(AccountId{7}, hardware_id(den));
  ASSERT_TRUE(row);
  EXPECT_EQ(row->device_uid, which.device_uid());
  EXPECT_EQ(row->keys.registration_key()[0], std::byte{5});
  EXPECT_EQ(row->keys.remote_play_key()[0], std::byte{6});
  EXPECT_TRUE(store.console(hardware_id(den)));
  EXPECT_EQ(signed_in.offline.requests, 0);
}

TEST_F(PairOverTheInternet, KeepsThePsNameWhenThePairingGaveNone) {
  const auto user = signed_in.as("npsso", 7);
  pairing.answers = attic;
  const auto paired = consoles.pair_over_the_internet(
      *user, listed(0x12, "Attic"), nullptr, stop);
  ASSERT_TRUE(paired);
  EXPECT_EQ(paired.value()->name(), "Attic");
  EXPECT_EQ(paired.value()->name_source(), ConsoleName::Source::account);
  const auto row = store.console(hardware_id(attic));
  ASSERT_TRUE(row);
  EXPECT_EQ(row->name.text(), "Attic");
}

TEST_F(PairOverTheInternet, PairsTheMainPs4ByItsPlaceholder) {
  const auto user = signed_in.as("npsso", 7);
  pairing.nickname = "PS4-Living";
  const auto paired = consoles.pair_over_the_internet(
      *user, AccountConsole::main_ps4(), nullptr, stop);
  ASSERT_TRUE(paired);
  ASSERT_TRUE(pairing.asked);
  EXPECT_TRUE(pairing.asked->is_main_ps4());
  EXPECT_EQ(pairing.asked_generation, Generation::ps4);
  EXPECT_EQ(paired.value()->generation(), Generation::ps4);
  EXPECT_EQ(paired.value()->name(), "PS4-Living");
  const auto row = store.pairing(AccountId{7}, hardware_id(den));
  ASSERT_TRUE(row);
  ASSERT_TRUE(row->device_uid);
  EXPECT_TRUE(row->device_uid->is_main_ps4());
}

TEST_F(PairOverTheInternet, NamesAnUnnamedMainPs4ByItsGeneration) {
  const auto user = signed_in.as("npsso", 7);
  const auto plain = consoles.pair_over_the_internet(
      *user, AccountConsole::main_ps4(), nullptr, stop);
  ASSERT_TRUE(plain);
  EXPECT_EQ(plain.value()->name_source(), ConsoleName::Source::generation);
}

TEST_F(PairOverTheInternet, AnswersRemotePlaySwitchedOffBeforePairing) {
  const auto user = signed_in.as("npsso", 7);
  const auto answered = consoles.pair_over_the_internet(
      *user, listed(0x13, "Den", false), nullptr, stop);
  ASSERT_FALSE(answered);
  EXPECT_EQ(answered.error().reason, PairingReason::remote_play_disabled);
  EXPECT_EQ(pairing.calls, 0);
  EXPECT_TRUE(consoles.everything().empty());
}

TEST_F(PairOverTheInternet, AnswersAHomeUserSignedOutWithoutARequest) {
  const auto home = signed_in.users.add_home_user(AccountId{9}, "Home Player");
  ASSERT_TRUE(home);
  const auto answered = consoles.pair_over_the_internet(
      *home.value(), listed(0x14, "Den"), nullptr, stop);
  ASSERT_FALSE(answered);
  EXPECT_EQ(answered.error().reason, PairingReason::unanswered);
  ASSERT_TRUE(answered.error().unanswered);
  EXPECT_EQ(answered.error().unanswered->reason,
            libchiaking::users::Unanswered::Reason::signed_out);
  EXPECT_EQ(pairing.calls, 0);
  EXPECT_EQ(signed_in.offline.requests, 0);
  EXPECT_TRUE(consoles.everything().empty());
}

TEST_F(PairOverTheInternet, KeepsTheStageOfAFailureAndSavesNothing) {
  const auto user = signed_in.as("npsso", 7);
  pairing.failure =
      PairingFailure{.reason = PairingReason::refused,
                     .http_status = 403,
                     .ps_reason = 0x80108b12,
                     .internet_stage = InternetPairingStage::pairing};
  const auto answered = consoles.pair_over_the_internet(
      *user, listed(0x15, "Den"), nullptr, stop);
  ASSERT_FALSE(answered);
  EXPECT_EQ(answered.error().reason, PairingReason::refused);
  EXPECT_EQ(answered.error().http_status, 403);
  EXPECT_EQ(answered.error().ps_reason, 0x80108b12U);
  EXPECT_EQ(answered.error().internet_stage, InternetPairingStage::pairing);
  EXPECT_EQ(store.console_count(), 0U);
  EXPECT_EQ(store.pairing_count(), 0U);
  EXPECT_TRUE(consoles.everything().empty());
}

TEST_F(PairOverTheInternet, LeavesNothingLiveWhenThePairingCannotBeSaved) {
  const auto user = signed_in.as("npsso", 7);
  store.refuse_pairing_saves = true;
  const auto answered = consoles.pair_over_the_internet(
      *user, listed(0x16, "Den"), nullptr, stop);
  ASSERT_FALSE(answered);
  EXPECT_EQ(answered.error().reason, PairingReason::not_saved);
  EXPECT_EQ(pairing.calls, 1);
  EXPECT_TRUE(consoles.everything().empty());
  EXPECT_EQ(store.pairing_count(), 0U);
}

}  // namespace
}  // namespace consoles_test
