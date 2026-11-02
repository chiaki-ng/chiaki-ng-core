#include "libchiaking/detail/catalog/catalog.hpp"
#include "libchiaking/detail/consoles/known_consoles.hpp"
#include "libchiaking/detail/play/internet_pairer.hpp"
#include "libchiaking/platform/default.hpp"
#include "libchiaking/testing/in_memory_console_store.hpp"
#include "libchiaking/testing/in_memory_record_store.hpp"
#include "libchiaking/testing/manual_clock.hpp"
#include "libchiaking/testing/service_builder.hpp"
#include "src/core/psn/account/client.hpp"
#include "tests/support/api/fakes.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/support/stop_states.hpp"

#include <gtest/gtest.h>

namespace default_platform_test {
namespace {

using libchiaking::AccountId;
using libchiaking::psn::auth::AppToken;

class ClientsOnTheDefaultAssembly : public ::testing::Test {
 protected:
  class Unwatched final : public libchiaking::consoles::NetworkWatcher {
   public:
    void on_view(const libchiaking::consoles::NetworkView&) override {
      ++views;
    }
    int views{};
  };

  libchiaking::platform::DefaultPlatform platform;
  libchiaking::platform::PlatformFactory& factory{platform.factory()};
};

TEST_F(ClientsOnTheDefaultAssembly, TheAccountClientIsCancelledByItsStop) {
  auto accounts = libchiaking::psn::account::Client::on(
      factory, test_support::Stopped::token());
  ASSERT_TRUE(accounts);
  const auto profile =
      accounts->profile(AppToken{"fake-app-token-3be1"}, AccountId{7});
  ASSERT_FALSE(profile);
  EXPECT_EQ(profile.error().error, libchiaking::psn::account::Error::cancelled);
}

TEST_F(ClientsOnTheDefaultAssembly, TheCatalogIsCancelledByItsStop) {
  const auto user = test_support::play::SignedInPlayer::fresh_for_remote_play(
      "fake-bearer-token-7d40");
  ASSERT_TRUE(factory.supplies(libchiaking::platform::PlatformCapability::http));
  auto& catalog_http = factory.http();
  libchiaking::testing::InMemoryRecordStore records{factory.threading()};
  libchiaking::testing::ManualClock wall;
  libchiaking::users::RequestBudget budget{factory.threading(),
                                           factory.clock()};
  libchiaking::catalog::Catalog catalog{
      libchiaking::testing::ServiceBuilder::build<
          libchiaking::catalog::Catalog>(catalog_http, records,
                                         factory.threading(), factory.clock(),
                                         wall, budget, &factory.diagnostics())};
  const auto product = libchiaking::catalog::ProductId::parse(
      "EP0109-PPSA07043_00-FAKE0043TITLE000");
  ASSERT_TRUE(product);
  const auto link =
      catalog.store_page_of(*user, *product, test_support::Stopped::token());
  ASSERT_FALSE(link);
  EXPECT_TRUE(link.error().is_cancelled());
}

TEST_F(ClientsOnTheDefaultAssembly, TheInternetPairerIsCancelledByItsStop) {
  libchiaking::play::InternetPairer pairer{factory, &platform.log()};
  const auto user = test_support::play::SignedInPlayer::fresh_for_remote_play(
      "fake-bearer-token-7d40");
  const auto paired =
      pairer.pair(*user, libchiaking::consoles::DeviceUid::main_ps4(),
                  libchiaking::consoles::Generation::ps4, nullptr,
                  test_support::Stopped::token());
  ASSERT_FALSE(paired);
  EXPECT_EQ(paired.error().reason,
            libchiaking::consoles::PairingFailure::Reason::cancelled);
}

TEST_F(ClientsOnTheDefaultAssembly, KnownConsolesSearchNothingOnceStopped) {
  libchiaking::testing::InMemoryConsoleStore store{factory.threading()};
  ASSERT_TRUE(
      factory.supplies(libchiaking::platform::PlatformCapability::crypto |
                       libchiaking::platform::PlatformCapability::http));
  auto& crypto = factory.crypto();
  auto& http = factory.http();
  libchiaking::consoles::KnownConsoles consoles{
      libchiaking::testing::ServiceBuilder::build<
          libchiaking::consoles::KnownConsoles>(
          factory.network(), http, crypto, factory.random(), store,
          factory.threading(), factory.clock(), nullptr,
          &factory.diagnostics())};
  Unwatched unwatched;
  const auto found = consoles.find_on_my_network(
      {}, unwatched, test_support::Stopped::token());
  EXPECT_TRUE(found);
  EXPECT_EQ(unwatched.views, 0);
  EXPECT_TRUE(consoles.everything().empty());
}

}  // namespace
}  // namespace default_platform_test
