#include "tests/unit/core/cloud/links/services/old_service_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace cloud_test {
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using Cause = play::OldServiceLaunch::Error::Cause;
using LaunchStage = play::OldServiceLaunch::Error::Stage;
using Method = OldServiceRig::Method;
using Rig = OldServiceRig;

class OldServiceLaunchFlow : public ::testing::Test {
 protected:
  auto resolve(bool force = false, bool skip_attributes = false)
      -> Rig::Resolved {
    return Rig::resolve(Rig::config(peer), peer, force, skip_attributes);
  }

  auto resolves_every_step() -> testing::AssertionResult {
    const auto result = resolve();
    if (!result) return testing::AssertionFailure() << "the launch failed";
    if (peer.calls != peer.steps.size())
      return testing::AssertionFailure()
             << peer.calls << " of " << peer.steps.size() << " steps ran";
    return testing::AssertionSuccess();
  }

  ScriptedPeer peer;
};

TEST_F(OldServiceLaunchFlow, ResolvesAHeldStreamingEntitlement) {
  peer.steps = Rig::full_steps(false);
  const auto result = resolve();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().entitlement(), Rig::stream_entitlement);
  EXPECT_EQ(result.value().platform(), libchiaking::cloud::CloudPlatform::ps4);
  EXPECT_EQ(peer.calls, peer.steps.size());
}

TEST_F(OldServiceLaunchFlow, AcquiresAFreeLicenceFirst) {
  peer.steps = Rig::full_steps(true);
  const auto result = resolve();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().entitlement(), Rig::stream_entitlement);
  EXPECT_EQ(result.value().platform(), libchiaking::cloud::CloudPlatform::ps4);
  EXPECT_EQ(peer.calls, peer.steps.size());
}

TEST_F(OldServiceLaunchFlow, ReadsALargeStoreContainer) {
  peer.steps = Rig::full_steps(false);
  std::string padded{Rig::container};
  padded.pop_back();
  padded += ",\"long_desc\":\"" + std::string(48 * 1024, 'd') +
            "\",\"playable_platform\":[\"PS4\"]}\n";
  peer.steps[2].body = padded;
  peer.steps[2].check = [](const Rig::Request& request) {
    EXPECT_GE(request.maximum_response_bytes, std::size_t{2 * 1024 * 1024});
  };
  const auto result = resolve();
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().entitlement(), Rig::stream_entitlement);
}

TEST_F(OldServiceLaunchFlow, TakesAMissingPriceAsFree) {
  peer.steps = Rig::full_steps();
  peer.steps[6].body =
      R"({"header":{"status_code":"0x0000"},"data":{"cart":{"items":[{"sku_id":"SKU-SERVER"}]}}})";
  EXPECT_TRUE(resolves_every_step());
}

TEST_F(OldServiceLaunchFlow, TakesANullPriceAsFree) {
  peer.steps = Rig::full_steps();
  peer.steps[6].body =
      R"({"header":{"status_code":"0x0000"},"data":{"cart":{"total_price_value":null,"items":[{"sku_id":"SKU-SERVER"}]}}})";
  EXPECT_TRUE(resolves_every_step());
}

TEST_F(OldServiceLaunchFlow, ReportsTheDisplayPriceOfAPaidLicence) {
  const std::vector<std::pair<std::string_view, std::string_view>> displays{
      {R"("total_price":"$19.99",)", "$19.99"},
      {R"("total_price":1999,)", "1999"},
      {"", ""}};
  for (const auto& [display, expected] : displays) {
    SCOPED_TRACE(display);
    ScriptedPeer paid;
    paid.steps = Rig::full_steps();
    paid.steps[6].body =
        std::string{R"({"header":{"status_code":"0x0000"},"data":{"cart":{)"} +
        std::string{display} + R"("total_price_value":1999}}})";
    const auto result = Rig::resolve(Rig::config(paid), paid, false);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().cause(), Cause::not_free);
    EXPECT_EQ(result.error().display_price(), expected);
    EXPECT_EQ(play::LaunchFailure::title(result.error()).refused().kind(),
              play::LaunchRefused::Kind::licence_not_free);
  }
}

TEST_F(OldServiceLaunchFlow, RefusesACopyNoPlanCoversBeforeTheCheckout) {
  peer.steps = Rig::full_steps();
  peer.steps[2].body = Rig::container_stating(
      R"([{"skuId":"SKU-B","offerAvailability":[{"offerId":"DOWNLOAD"}]},{"skuId":"SKU-A","offerAvailability":[]}])");
  peer.steps.resize(6);
  const auto result = resolve();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().cause(), Cause::outside_streaming_plan);
  EXPECT_EQ(result.error().stage(), LaunchStage::title);
  EXPECT_EQ(peer.calls, std::size_t{6});
  EXPECT_EQ(play::LaunchFailure::title(result.error()).refused().kind(),
            play::LaunchRefused::Kind::outside_streaming_plan);
}

TEST_F(OldServiceLaunchFlow, TakesACoveredOrUnstatedCopyToTheCheckout) {
  for (const auto relevancy :
       {R"([{"skuId":"SKU-A","offerAvailability":[{"offerId":"STREAM"}]}])",
        R"([{"skuId":"SKU-B","offerAvailability":[]}])",
        R"([{"skuId":"SKU-A"}])", R"({})"}) {
    SCOPED_TRACE(relevancy);
    ScriptedPeer covered;
    covered.steps = Rig::full_steps();
    covered.steps[2].body = Rig::container_stating(relevancy);
    EXPECT_TRUE(Rig::resolve(Rig::config(covered), covered, false));
    EXPECT_EQ(covered.calls, covered.steps.size());
  }
}

TEST_F(OldServiceLaunchFlow, LaunchesAHeldCopyNoPlanCovers) {
  peer.steps = Rig::full_steps(false);
  peer.steps[2].body =
      Rig::container_stating(R"([{"skuId":"SKU-A","offerAvailability":[]}])");
  EXPECT_TRUE(resolves_every_step());
}

TEST_F(OldServiceLaunchFlow, AsksForTheMissingAccountDetails) {
  peer.steps = Rig::full_steps(false);
  peer.steps[4].status = 403;
  peer.steps[4].body =
      R"({"error":{"validationErrors":[{"missingElements":[{"name":"BIRTH_DATE"},{"name":"CITY"}]},{"missingElements":[{"name":"REAL_NAME"}]}]}})";
  const auto result = resolve();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().cause(), Cause::privacy_required);
  EXPECT_EQ(result.error().http_status(), 403);
  EXPECT_EQ(peer.calls, std::size_t{5});
  const auto url = result.error().privacy_upgrade_url();
  EXPECT_TRUE(
      url.starts_with("https://id.sonyentertainmentnetwork.com/id/"
                      "upgrade_account_ca/?entry=upgrade_account"))
      << url;
  EXPECT_NE(
      url.find("&redirect_uri=https%3A%2F%2Fpsnow.playstation.com%2Fapp%2F2."
               "2.0%2F133%2F5cdcc037d%2Fgrc-response.html&"),
      std::string_view::npos);
  EXPECT_NE(url.find("&missing_elements=BIRTH_DATE%2CCITY%2CREAL_NAME&"
                     "response_type=code"),
            std::string_view::npos);
  EXPECT_TRUE(url.ends_with("&elements_visibility_upgrade=no_cancel")) << url;
}

TEST(OldServiceUpgradeUrl, KeepsItsLiteralsAndRawColons) {
  const std::vector<std::string> missing{"BIRTH_DATE", "CITY"};
  const auto error =
      libchiaking::play::OldServiceLaunch::Error::privacy_required(403,
                                                                   missing);
  const auto url = error.privacy_upgrade_url();
  EXPECT_EQ(
      url.substr(url.find("/id/upgrade_account_ca/")),
      "/id/upgrade_account_ca/"
      "?entry=upgrade_account&pr_referer=upgrade&redirect_uri=https%3A%2F%2F"
      "psnow.playstation.com%2Fapp%2F2.2.0%2F133%2F5cdcc037d%2Fgrc-response."
      "html&applicationId=psnow&refererPage=websso&service_logo=ps&tp_console="
      "true&disableLinks=SENLink&renderMode=mobilePortrait&noEVBlock=true&"
      "displayFooter=none&hidePageElements=SENLogo&layout_type=popup&missing_"
      "elements=BIRTH_DATE%2CCITY&response_type=code&service_entity=urn:"
      "service-entity:psn&smcid=pc:psnow&tp_psn=true&tp_social=true&elements_"
      "visibility_upgrade=no_cancel");
}

TEST(OldServiceUpgradeUrl, CapsTheElementsBeforeAndAfterEncoding) {
  const auto elements_of = [](std::vector<std::string> missing) {
    const auto error =
        libchiaking::play::OldServiceLaunch::Error::privacy_required(403,
                                                                     missing);
    const auto url = error.privacy_upgrade_url();
    const std::string_view start = "&missing_elements=";
    const auto from = url.find(start) + start.size();
    return std::string{url.substr(from, url.find("&response_type") - from)};
  };
  EXPECT_EQ(elements_of({std::string(600, 'a')}), std::string(511, 'a'));
  EXPECT_EQ(elements_of({std::string(510, 'a'), "bb", "cc"}),
            std::string(510, 'a') + "%2C");
  const auto spaces = elements_of({std::string(300, ' ')});
  EXPECT_EQ(spaces.size(), 765U);
  EXPECT_TRUE(spaces.ends_with("%20%20"));
  EXPECT_TRUE(libchiaking::play::OldServiceLaunch::Error::privacy_required(
                  403, std::vector<std::string>{})
                  .privacy_upgrade_url()
                  .empty());
}

TEST_F(OldServiceLaunchFlow, OffersNoUpgradeWithoutMissingDetails) {
  peer.steps = Rig::full_steps(false);
  peer.steps[4].status = 403;
  peer.steps[4].body = "{}";
  const auto result = resolve();
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().privacy_upgrade_url().empty());
}

TEST_F(OldServiceLaunchFlow, SkipsTheAccountCheckWhenAsked) {
  peer.steps = Rig::full_steps(false);
  peer.steps.erase(peer.steps.begin() + 4);
  EXPECT_TRUE(resolve(false, true));
  EXPECT_EQ(peer.calls, peer.steps.size());
}

TEST_F(OldServiceLaunchFlow, AuthorizesAnOwnedEntitlementDirectly) {
  peer.steps = Rig::full_steps();
  peer.steps.erase(peer.steps.begin(), peer.steps.end() - 2);
  auto request = Rig::config(peer);
  request.entitlement_id = "OWNED";
  const auto result = Rig::resolve(request, peer, false);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().entitlement(), "OWNED");
  EXPECT_EQ(peer.calls, peer.steps.size());
}

TEST_F(OldServiceLaunchFlow, ResolvesAnOwnedEntitlementWhenForced) {
  peer.steps = Rig::full_steps();
  auto request = Rig::config(peer);
  request.entitlement_id = "OWNED";
  const auto result = Rig::resolve(request, peer, true);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().entitlement(), Rig::stream_entitlement);
  EXPECT_EQ(peer.calls, peer.steps.size());
}

TEST_F(OldServiceLaunchFlow, StopsAtACancellationAfterAnyStep) {
  for (std::size_t stop_at = 0; stop_at < Rig::full_steps().size(); ++stop_at) {
    SCOPED_TRACE(stop_at);
    ScriptedPeer stopped;
    stopped.steps = Rig::full_steps();
    stopped.cancel_after = stop_at;
    const auto result = Rig::resolve(Rig::config(stopped), stopped, false);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().cause(), Cause::cancelled);
    EXPECT_EQ(stopped.calls, stop_at + 1);
  }
}

TEST_F(OldServiceLaunchFlow, StopsAtATransportFailureOfAnyStep) {
  for (std::size_t fail_at = 0; fail_at < Rig::full_steps().size(); ++fail_at) {
    SCOPED_TRACE(fail_at);
    ScriptedPeer failing;
    failing.steps = Rig::full_steps();
    failing.fail_at = fail_at;
    EXPECT_FALSE(Rig::resolve(Rig::config(failing), failing, false));
    EXPECT_EQ(failing.calls, fail_at + 1);
  }
}

TEST_F(OldServiceLaunchFlow, SendsNothingOnceStopped) {
  peer.stop->stopped = true;
  EXPECT_FALSE(resolve());
  EXPECT_EQ(peer.calls, std::size_t{0});
}

TEST_F(OldServiceLaunchFlow, RefusesACartThatIsNotFreeOrReadable) {
  for (const auto cart :
       {R"({"total_price_value":1})", R"({"total_price_value":0.1})",
        R"({"total_price_value":"unknown"})", R"([])"}) {
    SCOPED_TRACE(cart);
    ScriptedPeer refused;
    refused.steps = Rig::full_steps();
    refused.steps[6].body = std::string{R"({"data":{"cart":)"} + cart + "}}";
    const auto result = Rig::resolve(Rig::config(refused), refused, false);
    ASSERT_FALSE(result);
    EXPECT_EQ(refused.calls, std::size_t{7});
    EXPECT_EQ(result.error().stage(), LaunchStage::checkout_preview);
  }
}

TEST_F(OldServiceLaunchFlow, TellsAnotherStoreRegionFromAMissingPlan) {
  for (const auto& [code, cause, kind] :
       {std::tuple{"0x0113", Cause::store_region,
                   play::LaunchRefused::Kind::store_region},
        std::tuple{"0x0001", Cause::subscription_required,
                   play::LaunchRefused::Kind::membership_rejected}}) {
    SCOPED_TRACE(code);
    ScriptedPeer refused;
    refused.steps = Rig::full_steps();
    refused.steps[6].body =
        std::string{R"({"header":{"status_code":")"} + code + R"("}})";
    const auto result = Rig::resolve(Rig::config(refused), refused, false);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().stage(), LaunchStage::checkout_preview);
    EXPECT_EQ(result.error().cause(), cause);
    EXPECT_EQ(play::LaunchFailure::title(result.error()).refused().kind(),
              kind);
  }
}

TEST_F(OldServiceLaunchFlow, StopsAtARefusalOfAnyStep) {
  for (const std::size_t bad : {1U, 2U, 4U, 5U, 6U, 7U, 9U}) {
    SCOPED_TRACE(bad);
    ScriptedPeer refused;
    refused.steps = Rig::full_steps();
    refused.steps[bad].status = 403;
    refused.steps[bad].headers.clear();
    refused.steps[bad].body = "{}";
    EXPECT_FALSE(Rig::resolve(Rig::config(refused), refused, false));
    EXPECT_EQ(refused.calls, bad + 1);
  }
}

TEST_F(OldServiceLaunchFlow, BuysOnlyAtAZeroOrAbsentPrice) {
  for (
      const auto data :
      {R"({"cart":{"total_price_value":0,"items":[{"sku_id":"SKU-SERVER"}]}})",
       R"({"cart":{"items":[{"sku_id":"SKU-SERVER"}]}})",
       R"({"cart":{"total_price_value":null,"items":[{"sku_id":"SKU-SERVER"}]}})"}) {
    SCOPED_TRACE(data);
    ScriptedPeer free;
    free.steps = Rig::full_steps();
    free.steps[6].body = Rig::preview_body(data);
    EXPECT_TRUE(Rig::resolve(Rig::config(free), free, false));
    EXPECT_TRUE(Rig::requested_buynow(free));
  }
}

TEST_F(OldServiceLaunchFlow, BuysNothingAtAnyOtherPrice) {
  for (const auto data : {R"({"cart":{"total_price_value":1}})",
                          R"({"cart":{"total_price_value":0.01}})",
                          R"({"cart":{"total_price_value":"4.99"}})",
                          R"({"cart":{"total_price_value":"free"}})",
                          R"({"cart":{"total_price_value":true}})",
                          R"({"cart":{"total_price_value":false}})",
                          R"({"cart":{"total_price_value":{"amount":0}}})",
                          R"({"cart":{"total_price_value":[0]}})",
                          R"({"cart":{"total_price_value":""}})",
                          R"({"cart":{"total_price_value":"0 "}})",
                          R"({"cart":{"total_price_value":"nan"}})",
                          R"({"cart":null})", R"({"cart":[]})", R"({})"}) {
    SCOPED_TRACE(data);
    ScriptedPeer paid;
    paid.steps = Rig::full_steps();
    paid.steps[6].body = Rig::preview_body(data);
    const auto result = Rig::resolve(Rig::config(paid), paid, false);
    ASSERT_FALSE(result);
    EXPECT_FALSE(Rig::requested_buynow(paid));
    EXPECT_EQ(paid.calls, std::size_t{7});
    EXPECT_TRUE(result.error().cause() == Cause::not_free ||
                result.error().cause() == Cause::invalid_response)
        << testing::PrintToString(result.error().cause());
  }
}

TEST_F(OldServiceLaunchFlow, FallsBackFromAForeignContainer) {
  peer.steps = Rig::full_steps();
  auto request = Rig::config(peer);
  request.foreign_catalog = true;
  request.store_country = "HU";
  request.store_language = "hu";
  const ScriptedExchange first{Method::get, "/container/HU/hu/19/", 404, {}, {},
                               {}};
  peer.steps.insert(peer.steps.begin() + 2, first);
  EXPECT_TRUE(Rig::resolve(request, peer, false));
  EXPECT_EQ(peer.calls, std::size_t{11});
}

TEST_F(OldServiceLaunchFlow, ResolvesAForeignClassicWithoutItsEntitlement) {
  peer.steps = Rig::full_steps(false);
  peer.steps[5].status = 404;
  auto classic = Rig::config(peer);
  classic.foreign_catalog = true;
  classic.game_identifier = "EP0000-NPEB12345_00-TITLE";
  classic.store_country = "HU";
  classic.store_language = "hu";
  EXPECT_TRUE(Rig::resolve(classic, peer, false));
  EXPECT_EQ(peer.calls, std::size_t{8});
}

TEST_F(OldServiceLaunchFlow, SkipsTheCheckoutOfAForeignTitleThatRefusesIt) {
  peer.steps = Rig::full_steps();
  peer.steps[6].status = 403;
  peer.steps.erase(peer.steps.begin() + 7);
  auto foreign = Rig::config(peer);
  foreign.foreign_catalog = true;
  EXPECT_TRUE(Rig::resolve(foreign, peer, false));
  EXPECT_EQ(peer.calls, std::size_t{9});
}

class TitleSteps final : public play::LaunchObserver {
 public:
  void on_progress(const play::LaunchProgress&, remote_play::FailureStage stage,
                   std::string_view text) override {
    EXPECT_EQ(stage, remote_play::FailureStage::cloud_title);
    seen.emplace_back(text);
  }

  std::vector<std::string> seen;
};

TEST_F(OldServiceLaunchFlow, NamesEveryStepInItsProgress) {
  peer.steps = Rig::full_steps();
  TitleSteps steps;
  EXPECT_TRUE(Rig::resolve(
      Rig::config(peer), peer, false, false,
      play::LaunchReport{
          steps, libchiaking::catalog::ServiceGeneration::old_service}));
  EXPECT_EQ(
      steps.seen,
      (std::vector<std::string>{
          "Cloud Auth - Step 1 of 5", "Cloud Auth - Step 1 of 5",
          "Resolving Game - Step 2 of 5", "Checking License - Step 3 of 5",
          "Acquiring License - Step 3 of 5", "Authorizing - Step 4 of 5",
          "Creating Session - Step 5 of 5"}));
}

}  // namespace
}  // namespace cloud_test
