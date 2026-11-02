#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
#include "src/core/psn/session.hpp"
#include "tests/support/catalog/idle_catalog.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/support/users/signed_in.hpp"
#include "tests/unit/core/play/adapters/links/link_configuration_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>

namespace play_test {

using test_support::catalog::IdleCatalog;
using test_support::play::SignedInPlayer;
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::catalog::ProductId;
using libchiaking::cloud::CloudService;
using libchiaking::users::DeviceId;
using link_configurations::Configurations;
using play::CloudLinkConfiguration;
using remote_play::ControllerPreference;
using remote_play::StreamError;
using namespace std::string_view_literals;

static_assert(libchiaking::psn::Session::npsso_limit == 4096);

TEST(LinkConfigurationInCloud, AcceptsALaunch) {
  EXPECT_TRUE(Configurations::accepts(Configurations::cloud()));
}

TEST(LinkConfigurationInCloud, DoesNotAdmitAnNpssoWithSeparators) {
  for (const auto npsso : {"tok;en"sv, "tok,en"sv, "tok\ren"sv, "tok\nen"sv,
                           "tok en"sv, "tok\0en"sv}) {
    SCOPED_TRACE(testing::PrintToString(std::string{npsso}));
    EXPECT_FALSE(SignedInPlayer::admissible(SignedInPlayer::saved(npsso)));
  }
}

TEST(LinkConfigurationInCloud, AdmitsAnNpssoUpToTheLimit) {
  EXPECT_FALSE(SignedInPlayer::admissible(SignedInPlayer::saved(
      std::string(libchiaking::psn::Session::npsso_limit + 1, 'n'))));
  auto longest = Configurations::cloud();
  longest.user =
      SignedInPlayer::fresh(SignedInPlayer::saved(std::string(4096, 'a')));
  auto checked = std::move(longest).validated();
  ASSERT_TRUE(checked);
  EXPECT_NE(checked.value().user, nullptr);
  EXPECT_FALSE(SignedInPlayer::admissible(
      SignedInPlayer::saved(std::string(4097, 'a'))));
}

TEST(LinkConfigurationInCloud, RefusesAMissingUser) {
  auto configuration = Configurations::cloud();
  configuration.user.reset();
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationInCloud, RefusesAHomeUser) {
  auto configuration = Configurations::cloud();
  configuration.user = Configurations::home_user();
  ASSERT_NE(configuration.user, nullptr);
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationInCloud, RefusesAMissingProductOrCatalog) {
  EXPECT_FALSE(ProductId::parse("ent\0ty"sv));
  EXPECT_FALSE(ProductId::parse(""sv));
  auto configuration = Configurations::cloud();
  configuration.product.reset();
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
  configuration = Configurations::cloud();
  configuration.catalog = nullptr;
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationInCloud, RefusesATimeZoneThatIsMissingOrHasANul) {
  auto configuration = Configurations::cloud();
  configuration.time_zone = "UTC\0+00:00"sv;
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
  configuration = Configurations::cloud();
  configuration.time_zone.clear();
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationInCloud, RefusesAnOversizedTimeZone) {
  auto configuration = Configurations::cloud();
  configuration.time_zone.assign(CloudLinkConfiguration::locale_text_limit + 1,
                                 'c');
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationInCloud, RefusesABitrateThatIsNotPositive) {
  auto configuration = Configurations::cloud();
  configuration.bitrate_kbps = 0;
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
  configuration = Configurations::cloud();
  configuration.bitrate_kbps = -1;
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationInCloud, AcceptsAnyResolution) {
  auto configuration = Configurations::cloud();
  configuration.resolution = 999;
  EXPECT_TRUE(Configurations::accepts(std::move(configuration)));
}

TEST(LinkConfigurationInCloud, AcceptsTitlesOfBothServices) {
  for (const auto title :
       {IdleCatalog::ps5_title, IdleCatalog::old_service_title}) {
    SCOPED_TRACE(title);
    auto configuration = Configurations::cloud();
    configuration.product = IdleCatalog::title(title);
    EXPECT_TRUE(Configurations::accepts(std::move(configuration)));
  }
}

TEST(LinkConfigurationInCloud, AcceptsAUserWithADevice) {
  auto configuration = Configurations::cloud();
  configuration.user = SignedInPlayer::fresh(SignedInPlayer::saved(
      SignedInPlayer::npsso,
      "0000000700410080000102030405060708090a0b0c0d0e0f"));
  EXPECT_TRUE(Configurations::accepts(std::move(configuration)));
}

TEST(LinkConfigurationInCloud, ParsesDeviceAndProductIdsStrictly) {
  EXPECT_FALSE(
      DeviceId::parse("0000000700410080000102030405060708090a0b0c0d0e0"));
  EXPECT_FALSE(
      DeviceId::parse("0000000700410080000102030405060708090a0b0c0d0e0g"));
  EXPECT_FALSE(ProductId::parse(std::string(ProductId::limit + 1, 'g')));
}

TEST(LinkConfigurationInCloud, AcceptsAskingForTheDatacenterAtLaunch) {
  auto configuration = Configurations::cloud();
  const auto ask = play::DatacenterPreference::ask_at_launch(
      play::DatacenterPreference::maximum_wait);
  ASSERT_TRUE(ask);
  configuration.datacenter = ask.value();
  EXPECT_TRUE(Configurations::accepts(std::move(configuration)));
}

TEST(LinkConfigurationInCloud, AcceptsOnlyKnownFeedbackModes) {
  auto configuration = Configurations::cloud();
  configuration.feedback = static_cast<remote_play::FeedbackMode>(2);
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
  configuration = Configurations::cloud();
  configuration.feedback = remote_play::FeedbackMode::switch_compatibility;
  EXPECT_TRUE(Configurations::accepts(std::move(configuration)));
}

class LinkConfigurationInCloudSignedIn : public ::testing::Test {
 protected:
  void SetUp() override {
    session_ = signed_in_.as("SYNTHETIC_TOKEN", 1);
    other_ = signed_in_.as("OTHER_TOKEN", 2);
    ASSERT_TRUE(session_);
    ASSERT_TRUE(other_);
  }

  auto session() const -> const std::shared_ptr<libchiaking::users::PsUser>& {
    return session_;
  }
  auto other() const -> const std::shared_ptr<libchiaking::users::PsUser>& {
    return other_;
  }

 private:
  test_support::users::SignedIn signed_in_;
  std::shared_ptr<libchiaking::users::PsUser> session_;
  std::shared_ptr<libchiaking::users::PsUser> other_;
};

TEST_F(LinkConfigurationInCloudSignedIn, KeepsAnotherSignedInUser) {
  auto configuration = Configurations::cloud();
  configuration.user = other();
  auto checked = std::move(configuration).validated();
  ASSERT_TRUE(checked);
  EXPECT_EQ(checked.value().user, other());
}

TEST_F(LinkConfigurationInCloudSignedIn, StillChecksTheRestForAnotherUser) {
  auto configuration = Configurations::cloud();
  configuration.user = other();
  configuration.bitrate_kbps = 0;
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
  configuration = Configurations::cloud();
  configuration.user = other();
  configuration.product.reset();
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
}

TEST_F(LinkConfigurationInCloudSignedIn, KeepsTheSessionUserAndAccount) {
  auto configuration = Configurations::cloud();
  configuration.user = session();
  auto filled = std::move(configuration).validated();
  ASSERT_TRUE(filled);
  EXPECT_EQ(filled.value().user, session());
  ASSERT_NE(filled.value().user, nullptr);
  EXPECT_EQ(filled.value().user->account_id(), libchiaking::AccountId{1});
}

TEST(LinkConfigurationInCloud, AcceptsACompleteAllocation) {
  EXPECT_TRUE(Configurations::allocation().valid());
}

TEST(LinkConfigurationInCloud, RefusesAnAllocationMissingAField) {
  auto value = Configurations::allocation();
  value.server_ip.clear();
  EXPECT_FALSE(value.valid());
  value = Configurations::allocation();
  value.handshake_key.clear();
  EXPECT_FALSE(value.valid());
  value = Configurations::allocation();
  value.launch_spec.clear();
  EXPECT_FALSE(value.valid());
}

TEST(LinkConfigurationInCloud, RefusesALaunchSpecWithANulOrOversized) {
  auto value = Configurations::allocation();
  value.launch_spec.assign("launch\0spec"sv);
  EXPECT_FALSE(value.valid());
  value = Configurations::allocation();
  value.launch_spec.assign(
      std::string(play::CloudAllocation::launch_spec_limit + 1, 's'));
  EXPECT_FALSE(value.valid());
}

TEST(LinkConfigurationInCloud, RefusesAnAllocationForAnUnknownService) {
  auto value = Configurations::allocation();
  value.service = static_cast<CloudService>(7);
  EXPECT_FALSE(value.valid());
}

TEST(LinkConfigurationInCloud, AcceptsAnAllocationWithoutALaunch) {
  CloudLinkConfiguration configuration;
  configuration.allocation = Configurations::allocation();
  EXPECT_TRUE(Configurations::accepts(std::move(configuration)));
  configuration = CloudLinkConfiguration{};
  configuration.allocation = Configurations::allocation();
  configuration.allocation->server_ip.clear();
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationInCloud, PicksTheChosenController) {
  auto streamed = Configurations::cloud();
  EXPECT_TRUE(streamed.enable_dualsense_ctrl);
  EXPECT_TRUE(streamed.prefer_controller(ControllerPreference::dualshock4));
  EXPECT_FALSE(streamed.enable_dualsense_ctrl);
  EXPECT_TRUE(
      streamed.prefer_controller(ControllerPreference::console_default));
  EXPECT_TRUE(streamed.enable_dualsense_ctrl);
  EXPECT_TRUE(streamed.prefer_controller(ControllerPreference::dualsense));
  EXPECT_TRUE(streamed.enable_dualsense_ctrl);
}

TEST(LinkConfigurationInCloud, KeepsSkippingTheAccountCheck) {
  auto title = Configurations::cloud();
  title.product = IdleCatalog::title(IdleCatalog::old_service_title);
  title.skip_account_attributes_check = true;
  auto checked = std::move(title).validated();
  ASSERT_TRUE(checked);
  EXPECT_TRUE(checked.value().skip_account_attributes_check);
}

TEST(LinkConfigurationInCloud, RefusesAnOverLongHandshakeKey) {
  auto over_long_key = Configurations::cloud();
  over_long_key.allocation = Configurations::allocation();
  over_long_key.allocation->handshake_key.assign(std::string(64, 'A'));
  EXPECT_FALSE(over_long_key.allocation->valid());
  EXPECT_EQ(Configurations::refusal(std::move(over_long_key)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationInCloud, RefusesAMalformedHandshakeKey) {
  for (const auto key :
       {"AAECAwQFBgcICQoLDA0O"sv, "AAECAwQFBgcICQoLDA0ODxA="sv,
        "not base64 at all!!"sv, "AAECAwQF\0gcICQoLDA0ODw=="sv}) {
    SCOPED_TRACE(testing::PrintToString(std::string{key}));
    auto malformed = Configurations::cloud();
    malformed.allocation = Configurations::allocation();
    malformed.allocation->handshake_key.assign(key);
    EXPECT_FALSE(malformed.allocation->valid());
    EXPECT_EQ(Configurations::refusal(std::move(malformed)),
              StreamError::invalid_configuration);
  }
}

TEST(LinkConfigurationInCloud, AcceptsASixteenByteHandshakeKey) {
  auto exact = Configurations::cloud();
  exact.allocation = Configurations::allocation();
  exact.allocation->handshake_key.assign("AAAAAAAAAAAAAAAAAAAAAA==");
  auto checked = std::move(exact).validated();
  ASSERT_TRUE(checked);
  ASSERT_TRUE(checked.value().allocation);
  EXPECT_TRUE(checked.value().allocation->valid());
}

}  // namespace
}  // namespace play_test
