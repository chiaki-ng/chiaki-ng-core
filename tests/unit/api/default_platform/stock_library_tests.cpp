#include "libchiaking/chiaki_ng.hpp"
#include "libchiaking/platform/default.hpp"
#include "libchiaking/testing/in_memory_console_store.hpp"
#include "libchiaking/testing/in_memory_credential_store.hpp"
#include "libchiaking/testing/in_memory_record_store.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <span>
#include <vector>

namespace default_platform_test {
namespace {

using libchiaking::AccountId;

class StockLibrary : public ::testing::Test {
 protected:
  class NoDatacenters final : public libchiaking::play::DatacenterStore {
   public:
    auto load(libchiaking::catalog::ServiceGeneration) -> libchiaking::Expected<
        std::vector<libchiaking::play::DatacenterMeasurement>, Error> override {
      return std::vector<libchiaking::play::DatacenterMeasurement>{};
    }
    auto save(libchiaking::catalog::ServiceGeneration,
              std::span<const libchiaking::play::DatacenterMeasurement>)
        -> libchiaking::Expected<void, Error> override {
      return {};
    }
  };

  libchiaking::platform::DefaultPlatform outside;
  libchiaking::testing::InMemoryCredentialStore credentials{
      outside.threading()};
  libchiaking::testing::InMemoryConsoleStore consoles{outside.threading()};
  libchiaking::testing::InMemoryRecordStore records{outside.threading()};
  NoDatacenters datacenters;
};

TEST_F(StockLibrary, OpensOnTheStockPlatformAndKeepsItOnceCopied) {
  auto opened = libchiaking::ChiakiNg::open(libchiaking::ChiakiNg::Stores{
      credentials, consoles, datacenters, nullptr, records});
  ASSERT_TRUE(opened);
  auto kept = opened.value();
  opened = libchiaking::unexpected(libchiaking::ChiakiNg::Missing::http);
  const auto returned = kept.users().welcome_back();
  ASSERT_TRUE(returned);
  EXPECT_TRUE(returned.value().users.empty());
  EXPECT_TRUE(kept.consoles().remember_saved());
  EXPECT_TRUE(kept.consoles().everything().empty());
  EXPECT_EQ(kept.play().live(), nullptr);
  const auto added =
      kept.users().add_home_user(AccountId{41}, "Living Room Player");
  ASSERT_TRUE(added);
  EXPECT_TRUE(kept.sign_out(added.value()->account_id()));
  EXPECT_TRUE(kept.users().everyone().empty());
}

}  // namespace
}  // namespace default_platform_test
