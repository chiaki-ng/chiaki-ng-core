#include "src/core/psn/graphql/authenticated/operations.hpp"
#include "src/core/psn/graphql/public_web/operations.hpp"
#include "tests/unit/core/psn/graphql/rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace psn_test {
namespace {

using Auth = GraphqlRig::Auth;
using Operation = GraphqlRig::Operation;
using Reply = ClientRig::Reply;
namespace graphql = libchiaking::psn::graphql;

TEST(GraphqlOperations, OnTheWebArePublicAndLabelledForTheWeb) {
  for (const auto& operation : graphql::public_web::operations) {
    SCOPED_TRACE(operation.name);
    EXPECT_EQ(operation.endpoint, graphql::web_endpoint);
    EXPECT_EQ(operation.label,
              "psn.graphql.web." + std::string{operation.name});
    EXPECT_EQ(operation.auth, Auth::none);
  }
}

TEST(GraphqlOperations, ThatAreAuthenticatedRequireACredential) {
  for (const auto& operation : graphql::authenticated::operations) {
    SCOPED_TRACE(operation.name);
    const bool on_web = operation.endpoint == graphql::web_endpoint;
    EXPECT_TRUE(on_web || operation.endpoint == graphql::app_endpoint);
    EXPECT_EQ(operation.label,
              (on_web ? "psn.graphql.web." : "psn.graphql.mobile.") +
                  std::string{operation.name});
    EXPECT_EQ(operation.auth, Auth::required);
  }
}

TEST(GraphqlOperations, AreNamedAsTheServiceNamesThem) {
  EXPECT_EQ(graphql::authenticated::get_subscriptions.endpoint,
            graphql::web_endpoint);
  EXPECT_EQ(graphql::app_endpoint,
            "https://m.np.playstation.com/api/graphql/v1/op");
  EXPECT_EQ(graphql::authenticated::concept_by_id.name, "metGetConceptById");
  EXPECT_EQ(graphql::authenticated::concept_pricing.name,
            "metGetPricingDataByConceptId");
  EXPECT_EQ(graphql::authenticated::concept_by_product.name,
            "metGetConceptByProductIdQuery");
  EXPECT_EQ(graphql::authenticated::get_subscriptions.name, "getSubscriptions");
}

TEST(GraphqlOperations, AreAllVersionOneAndSendable) {
  std::vector<Operation> all{graphql::public_web::operations.begin(),
                             graphql::public_web::operations.end()};
  all.insert(all.end(), graphql::authenticated::operations.begin(),
             graphql::authenticated::operations.end());
  for (const auto& operation : all) {
    SCOPED_TRACE(operation.name);
    EXPECT_EQ(operation.version, 1);
    GraphqlRig rig;
    rig.http.on("GET", GraphqlRig::url_of(operation, "{}"),
                {Reply{200, R"({"data":{}})"}});
    EXPECT_TRUE(rig.send(GraphqlRig::with_auth(operation, Auth::none)));
  }
}

}  // namespace
}  // namespace psn_test
