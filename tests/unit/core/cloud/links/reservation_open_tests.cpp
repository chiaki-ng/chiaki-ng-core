#include "tests/unit/core/cloud/rig.hpp"

#include <gtest/gtest.h>

#include <string>

namespace cloud_test {
namespace {

TEST(ReservationOpen, RefusesAnUnknownServiceBeforeSending) {
  RecordingHttp http;
  auto waiter = GkRig::recording_waiter();
  Gk client{http};
  const auto opened = play::ServerReservation::open(
      client, {waiter},
      gk::LaunchSpec::prebuilt(static_cast<CloudService>(99),
                               std::string{GkRig::blank_spec}),
      CloudPlatform::ps5);
  EXPECT_TRUE(GkRig::failed(opened, gk::Error::Stage::client_ids,
                            gk::Error::Reason::invalid_service, 0));
  EXPECT_EQ(http.calls, 0U);
}

}  // namespace
}  // namespace cloud_test
