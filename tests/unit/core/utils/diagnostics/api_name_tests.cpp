#include "src/core/psn/graphql/authenticated/operations.hpp"
#include "src/core/psn/graphql/public_web/operations.hpp"
#include "src/core/psn/rest/accounts/accounts_client.hpp"
#include "src/core/psn/rest/cloud_assisted_navigation/cloud_assisted_navigation.hpp"
#include "src/core/psn/rest/commerce/commerce_client.hpp"
#include "src/core/psn/rest/gamelist/gamelist.hpp"
#include "src/core/psn/rest/kamaji/kamaji_client.hpp"
#include "src/core/psn/rest/push/push.hpp"
#include "src/core/psn/rest/session_manager/session_manager.hpp"
#include "src/core/psn/rest/store/store_client.hpp"
#include "src/core/psn/rest/trophy/trophies.hpp"
#include "src/core/psn/rest/user_profile/user_profile.hpp"
#include "src/core/utils/diagnostics/api_name.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace diagnostics_test {
namespace {

using libchiaking::diagnostics::ApiName;

class ApiNames : public ::testing::Test {
 protected:
  static constexpr std::string_view unclassified = "Unclassified HTTP API";

  static auto name(std::string_view label, std::string url = {})
      -> std::string {
    libchiaking::platform::HttpRequest request;
    request.label = label;
    request.url = libchiaking::SecretString{url};
    return std::string{ApiName{request}.view()};
  }

  static auto listed(std::string_view label) -> bool {
    return std::ranges::any_of(ApiName::known(), [label](const auto& entry) {
      return entry.label == label;
    });
  }
};

TEST_F(ApiNames, NameStreamCallsFromTheirLabels) {
  EXPECT_EQ(name("psn.session_manager.create_remote_play_session"),
            "PSN Create Remote Play session");
  EXPECT_EQ(name("psn.session_manager.observe_sessions"),
            "PSN Read Remote Play session");
  EXPECT_EQ(name("psn.session_manager.leave_session"),
            "PSN Release Remote Play membership");
  EXPECT_EQ(name("psn.cloud_assisted_navigation.start_remote_play"),
            "PSN Start PS5 Remote Play");
  EXPECT_EQ(name("psn.user_profile.wake_ps4"), "PSN Wake PS4");
  EXPECT_EQ(name("gk.client_ids"), "Cloud Get client IDs");
  EXPECT_EQ(name("gk.datacenters"), "Cloud List datacenters");
  EXPECT_EQ(name("gk.select"), "Cloud Select datacenter");
  EXPECT_EQ(name("gk.allocate"), "Cloud Allocate instance");
  EXPECT_EQ(name("psn.auth.commerce_token"), "PS OAuth authorization");
  EXPECT_EQ(name("psn.kamaji.buynow_preview"),
            "LegacyCloudStream Preview entitlement resolution");
  EXPECT_EQ(name("psn.kamaji.buynow"), "LegacyCloudStream Resolve entitlement");
  EXPECT_EQ(name("psn.kamaji.container"),
            "LegacyCloudStream Resolve catalog title");
  EXPECT_EQ(name("psn.commerce.internal_entitlements"),
            "LegacyCloudStream List owned entitlements");
  EXPECT_EQ(name("psn.commerce.internal_entitlement"),
            "LegacyCloudStream Get owned entitlements");
  EXPECT_EQ(name("psn.auth.legacy_cloud_stream_code"),
            "PS OAuth authorization");
  EXPECT_EQ(name("psn.auth.authorize_check"), "Cloud Check authorization");
  EXPECT_EQ(name("psn.kamaji.stores"),
            "LegacyCloudStream Resolve catalog title");
}

TEST_F(ApiNames, NameGraphqlOperations) {
  EXPECT_EQ(name("psn.graphql.mobile.metGetConceptById"),
            "PSN Get title concept");
  EXPECT_EQ(name("psn.graphql.mobile.metGetPricingDataByConceptId"),
            "PSN Get title pricing");
  EXPECT_EQ(name("psn.graphql.mobile.metGetConceptByProductIdQuery"),
            "PSN Find title concept by product");
  EXPECT_EQ(name("psn.graphql.web.categoryGridRetrieve"),
            "PS Plus Get catalog category");
  EXPECT_EQ(name("psn.graphql.web.getSubscriptions"), "PSN Get subscriptions");
  EXPECT_EQ(name("psn.graphql.web.futureOperation"), "PSN Web GraphQL query");
  EXPECT_EQ(name("psn.graphql.mobile.futureOperation"),
            "PSN Mobile GraphQL query");
}

TEST_F(ApiNames, LeaveUnknownLabelsUnclassified) {
  EXPECT_EQ(name("psn.auth.future"), unclassified);
  EXPECT_EQ(name("test.unknown"), unclassified);
}

TEST_F(ApiNames, ClassifyEveryKnownLabel) {
  for (const auto& entry : ApiName::known())
    EXPECT_NE(name(entry.label), unclassified) << entry.label;
}

TEST_F(ApiNames, ListEveryLabelTheClientsSend) {
  namespace rest = libchiaking::psn::rest;
  for (const std::string_view label :
       {std::string_view{"psn.session_manager.create_remote_play_session"},
        std::string_view{"psn.session_manager.observe_sessions"},
        std::string_view{"psn.session_manager.leave_session"},
        std::string_view{"psn.session_manager.send_session_message"},
        std::string_view{"psn.push.server_address"},
        rest::commerce_entitlements_label,
        rest::commerce_entitlement_label,
        std::string_view{"psn.cloud_assisted_navigation.list_clients"},
        std::string_view{"psn.cloud_assisted_navigation.launch_title"},
        std::string_view{"psn.cloud_assisted_navigation.start_remote_play"},
        std::string_view{"psn.cloud_assisted_navigation.poll_command"},
        std::string_view{"psn.gamelist.played_titles"},
        rest::Trophies::summary_label,
        rest::Trophies::titles_label,
        rest::Trophies::groups_label,
        rest::Trophies::earned_groups_label,
        rest::Trophies::trophies_label,
        rest::Trophies::earned_trophies_label,
        std::string_view{"psn.game_media.capture"},
        std::string_view{"psn.game_media.download_address"},
        std::string_view{"psn.settings.setting"},
        std::string_view{"psn.user_profile.profile_base_url"},
        std::string_view{"psn.user_profile.wake_ps4"},
        rest::store_product_page_label,
        rest::store_games_lists_label,
        rest::accounts_validate_attributes_label,
        rest::kamaji_create_session_label,
        rest::kamaji_stores_label,
        rest::kamaji_container_label,
        rest::kamaji_category_label,
        rest::kamaji_buynow_preview_label,
        rest::kamaji_buynow_label,
        std::string_view{"gk.client_ids"},
        std::string_view{"gk.config"},
        std::string_view{"gk.start"},
        std::string_view{"gk.authorize"},
        std::string_view{"gk.lock"},
        std::string_view{"gk.datacenters"},
        std::string_view{"gk.select"},
        std::string_view{"gk.allocate"},
        std::string_view{"psn.auth.commerce_token"},
        std::string_view{"psn.auth.legacy_cloud_stream_code"},
        std::string_view{"psn.auth.gk_client_code"},
        std::string_view{"psn.auth.gk_server_code"},
        std::string_view{"psn.auth.authorize_check"}})
    EXPECT_TRUE(listed(label)) << label;
}

TEST_F(ApiNames, ListEveryGraphqlOperation) {
  namespace graphql = libchiaking::psn::graphql;
  for (const auto& operation : graphql::public_web::operations)
    EXPECT_TRUE(listed(operation.label)) << operation.label;
  for (const auto& operation : graphql::authenticated::operations)
    EXPECT_TRUE(listed(operation.label)) << operation.label;
}

TEST_F(ApiNames, NameUnlabelledCallsByTheirUrl) {
  EXPECT_EQ(name({},
                 "https://raw.githubusercontent.com/pradt2/always-online-stun/"
                 "master/valid_hosts.txt"),
            "STUN Get preferred servers");
  EXPECT_EQ(name({},
                 "https://web.np.playstation.com/api/sessionManager/v1/"
                 "remotePlaySessions"),
            unclassified);
}

}  // namespace
}  // namespace diagnostics_test
