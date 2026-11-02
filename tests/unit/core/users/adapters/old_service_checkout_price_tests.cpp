#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/users/adapters/old_service_rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace users_test {
namespace {

using Step = OldServiceRig::Step;
using StoreReason = libchiaking::psn::rest::StoreError::Reason;

TEST(OldServiceCheckoutPrice, IsFreeWhenMissing) {
  for (const std::string_view price : {"", R"("total_price_value":null,)"}) {
    SCOPED_TRACE(price);
    OldServicePlayer player;
    ScriptedPeer peer;
    peer.steps = OldServiceRig::full_steps();
    peer.steps[6].body =
        std::string{R"({"header":{"status_code":"0x0000"},"data":{"cart":{)"} +
        std::string{price} + R"("items":[{"sku_id":"SKU-SERVER"}]}}})";
    const auto launched = OldServiceRig::launch(peer, *player.user);
    EXPECT_TRUE(launched.finished());
    EXPECT_EQ(peer.calls, peer.steps.size());
    ASSERT_TRUE(launched.preview);
    EXPECT_TRUE(launched.preview->free);
    EXPECT_EQ(launched.preview->sku, "SKU-SERVER");
  }
}

TEST(OldServiceCheckoutPrice, CarriesTheDisplayPriceOfAPricedPreview) {
  for (const std::string_view display :
       {R"("total_price":"$19.99",)", R"("total_price":1999,)", ""}) {
    SCOPED_TRACE(display);
    OldServicePlayer player;
    ScriptedPeer peer;
    peer.steps = OldServiceRig::full_steps();
    peer.steps[6].body =
        std::string{R"({"header":{"status_code":"0x0000"},"data":{"cart":{)"} +
        std::string{display} + R"("total_price_value":1999}}})";
    const auto launched = OldServiceRig::launch(peer, *player.user);
    EXPECT_FALSE(launched.finished());
    EXPECT_FALSE(launched.refusal);
    ASSERT_TRUE(launched.preview);
    EXPECT_FALSE(launched.preview->free);
    const std::string_view expected =
        display.empty()                               ? ""
        : display.find('$') != std::string_view::npos ? "$19.99"
                                                      : "1999";
    EXPECT_EQ(launched.preview->display_price, expected);
    EXPECT_EQ(peer.calls, 7U);
    EXPECT_FALSE(OldServiceRig::requested_buynow(peer));
  }
}

TEST(OldServiceCheckoutPrice, BuysAtAZeroOrAbsentPrice) {
  for (
      const std::string_view data :
      {R"({"cart":{"total_price_value":0,"items":[{"sku_id":"SKU-SERVER"}]}})",
       R"({"cart":{"items":[{"sku_id":"SKU-SERVER"}]}})",
       R"({"cart":{"total_price_value":null,"items":[{"sku_id":"SKU-SERVER"}]}})"}) {
    SCOPED_TRACE(data);
    OldServicePlayer player;
    ScriptedPeer peer;
    peer.steps = OldServiceRig::full_steps();
    peer.steps[6].body = OldServiceRig::preview_body(data);
    const auto launched = OldServiceRig::launch(peer, *player.user);
    EXPECT_TRUE(launched.finished());
    EXPECT_TRUE(OldServiceRig::requested_buynow(peer));
  }
}

TEST(OldServiceCheckoutPrice, BuysNothingAtAnyOtherPrice) {
  for (const std::string_view data :
       {R"({"cart":{"total_price_value":1}})",
        R"({"cart":{"total_price_value":0.01}})",
        R"({"cart":{"total_price_value":"4.99"}})",
        R"({"cart":{"total_price_value":"free"}})",
        R"({"cart":{"total_price_value":true}})",
        R"({"cart":{"total_price_value":false}})",
        R"({"cart":{"total_price_value":{"amount":0}}})",
        R"({"cart":{"total_price_value":[0]}})",
        R"({"cart":{"total_price_value":""}})",
        R"({"cart":{"total_price_value":"0 "}})",
        R"({"cart":{"total_price_value":"nan"}})", R"({"cart":null})",
        R"({"cart":[]})", R"({})"}) {
    SCOPED_TRACE(data);
    OldServicePlayer player;
    ScriptedPeer peer;
    peer.steps = OldServiceRig::full_steps();
    peer.steps[6].body = OldServiceRig::preview_body(data);
    const auto launched = OldServiceRig::launch(peer, *player.user);
    EXPECT_FALSE(launched.finished());
    EXPECT_FALSE(OldServiceRig::requested_buynow(peer));
    EXPECT_EQ(peer.calls, 7U);
    if (launched.refusal) {
      EXPECT_EQ(launched.refusal->step(), Step::preview);
      ASSERT_TRUE(launched.refusal->store());
      EXPECT_EQ(launched.refusal->store()->reason(), StoreReason::malformed);
    } else {
      ASSERT_TRUE(launched.preview);
      EXPECT_FALSE(launched.preview->free);
    }
  }
}

}  // namespace
}  // namespace users_test
