#include "libchiaking/platform/defaults.hpp"
#include "libchiaking/catalog.hpp"
#include "libchiaking/chiaki_ng.hpp"
#include "libchiaking/consoles.hpp"
#include "libchiaking/consoles_types.hpp"
#include "libchiaking/platform/platform.hpp"
#include "libchiaking/platform/stock_providers.hpp"
#include "libchiaking/play.hpp"
#include "libchiaking/play_types.hpp"
#include "libchiaking/testing/in_memory_console_store.hpp"
#include "libchiaking/testing/in_memory_credential_store.hpp"
#include "libchiaking/testing/in_memory_record_store.hpp"
#include "libchiaking/users.hpp"
#include "libchiaking/version.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace play = libchiaking::play;
namespace consoles = libchiaking::consoles;

class DiscardedLog final : public libchiaking::platform::DiagnosticsProvider {
 public:
  void write(libchiaking::platform::LogRecord) noexcept override {}
};

template <class T>
concept Cpp20 = std::same_as<T, int>;

static_assert(Cpp20<int>);

class OfflineHttp final : public libchiaking::platform::HttpProvider {
 public:
  [[nodiscard]] auto perform(const libchiaking::platform::HttpRequest&,
                             const libchiaking::platform::StopToken&)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    return libchiaking::unexpected(
        libchiaking::platform::HttpError::disconnected);
  }
};

class NoCrypto final : public libchiaking::platform::CryptoProvider {
 public:
  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }
  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<std::byte>)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }
  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }
  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void,
                               libchiaking::platform::CryptoError> override {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::backend_failure);
  }
};

class NoDatacenters final : public play::DatacenterStore {
 public:
  auto load(libchiaking::catalog::ServiceGeneration)
      -> libchiaking::Expected<std::vector<play::DatacenterMeasurement>,
                               Error> override {
    return std::vector<play::DatacenterMeasurement>{};
  }
  auto save(libchiaking::catalog::ServiceGeneration,
            std::span<const play::DatacenterMeasurement>)
      -> libchiaking::Expected<void, Error> override {
    return {};
  }
};

}  // namespace

int main() {
  if (!libchiaking::Version::current().text().starts_with(LIBCHIAKING_CONSUMER_PACKAGE_VERSION)) return 30;
  OfflineHttp offline;
  NoCrypto crypto;
  libchiaking::platform::DefaultThreadingProvider threading;
  libchiaking::testing::InMemoryCredentialStore credentials{threading};
  libchiaking::testing::InMemoryConsoleStore saved_consoles{threading};
  libchiaking::testing::InMemoryRecordStore records{threading};
  NoDatacenters datacenters;
  const libchiaking::ChiakiNg::Stores stores{credentials, saved_consoles,
                                             datacenters, nullptr, records};
  libchiaking::platform::DefaultPlatformFactory offline_platform{
      libchiaking::platform::PlatformProviders{.crypto = &crypto,
                                               .http = &offline}};
  auto opened = libchiaking::ChiakiNg::open(offline_platform, stores);
  if (!opened) return 20;
  auto& users = opened.value().users();
  if (!credentials.save(libchiaking::users::SavedSignIn{
          libchiaking::AccountId{1}, "player",
          libchiaking::users::Npsso{"NPSSO"}}) ||
      !users.welcome_back())
    return 21;
  const auto user = users.find(libchiaking::AccountId{1});
  if (!user ||
      user->sign_in_state() !=
          libchiaking::users::SignInState::store_access_lapsed ||
      user->membership().tier())
    return 22;
  auto& catalog = opened.value().catalog();
  const auto product = libchiaking::catalog::ProductId::parse(
      "EP0109-PPSA07043_00-FAKE0043TITLE000");
  if (!product || catalog.can_launch(*user, *product).is_allowed()) return 5;
  if (libchiaking::controllers::SeatNumber::count != 4 ||
      !libchiaking::controllers::SeatNumber::first().is_first())
    return 6;
  const auto hardware_id = consoles::HardwareId::parse("AA:BB:CC:DD:EE:FF");
  if (!hardware_id || hardware_id->text() != "aabbccddeeff") return 7;
  const std::array<std::string, 1> subnets{"192.0.2.0/24"};
  const auto ranges = consoles::SearchRanges::parse(subnets, true);
  if (!ranges || ranges.value().target_count() != 254) return 8;
  if (consoles::Firmware::from(consoles::RegistrationTarget::ps5_1) !=
      consoles::Firmware{consoles::Firmware::Family::ps5_1})
    return 9;
  if (catalog.explain(*user, *product).is_listed()) return 10;
  const libchiaking::controllers::ControllerState sample;
  if (sample.touches[0].id != -1 || sample.touches[1].id != -1 ||
      sample.accel_y != 1.0F || sample.orient_w != 1.0F)
    return 14;
  libchiaking::platform::DefaultPlatformFactory platform;
  if (play::Controllers::default_depth != 64 ||
      play::Seats::default_depth != 16)
    return 17;
  if (play::LoginPin::default_await.count() != 10) return 18;
  DiscardedLog log;
  libchiaking::play::InternetPairer pairer{platform, &log};
  libchiaking::platform::StopSource pairing_stop;
  static_cast<void>(pairing_stop.request_stop());
  const auto unpaired =
      pairer.pair(*user, consoles::DeviceUid::main_ps4(),
                  consoles::Generation::ps4, nullptr, pairing_stop.token());
  if (unpaired ||
      unpaired.error().reason != consoles::PairingFailure::Reason::cancelled)
    return 19;
#if defined(LIBCHIAKING_CONSUMER_HAS_CURL)
  libchiaking::platform::CurlHttpProvider http{threading};
  libchiaking::platform::DefaultPlatformFactory online_platform{
      libchiaking::platform::PlatformProviders{.crypto = &crypto,
                                               .http = &http}};
  auto connected = libchiaking::ChiakiNg::open(online_platform, stores);
  if (!connected) return 13;
  auto& online = connected.value().catalog();
  libchiaking::platform::StopSource left;
  static_cast<void>(left.request_stop());
  if (online.store_page_of(*user, *product, left.token())) return 11;
  if (libchiaking::consoles::InstalledTitle::create({})) return 12;
#endif
  return 0;
}
