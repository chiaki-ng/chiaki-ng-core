#include "src/core/net/transport/http/failure.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

namespace {

using libchiaking::net::http::error_name;
using libchiaking::net::http::Failure;
namespace platform = libchiaking::platform;

TEST(HttpFailure, ClassifiesACancelledTransfer) {
  EXPECT_TRUE(
      Failure::transport(platform::HttpError::cancelled).is_cancelled());
}

TEST(HttpFailure, ClassifiesPlatformErrorsByKind) {
  EXPECT_EQ(Failure::transport(platform::HttpError::invalid_url).kind(),
            Failure::Kind::invalid_request);
  EXPECT_EQ(Failure::transport(platform::HttpError::response_too_large).kind(),
            Failure::Kind::too_large);
  EXPECT_EQ(Failure::transport(platform::HttpError::timed_out).kind(),
            Failure::Kind::transport);
}

TEST(HttpFailure, NamesAnInvalidRequest) {
  EXPECT_EQ(Failure::invalid_request().name(), "invalid_request");
}

TEST(HttpFailure, CountsTheAttemptsOfACancellation) {
  EXPECT_EQ(Failure::cancelled(2).attempts(), 2);
}

TEST(HttpFailure, NamesEachPlatformError) {
  EXPECT_EQ(error_name(platform::HttpError::timed_out), "timed_out");
}

}  // namespace
