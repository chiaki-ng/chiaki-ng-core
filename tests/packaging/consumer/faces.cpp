#include <libchiaking/chiaki_ng.hpp>
#include <libchiaking/platform/defaults.hpp>
#include <libchiaking/platform/platform.hpp>
#include <libchiaking/testing/in_memory_console_store.hpp>
#include <libchiaking/testing/in_memory_credential_store.hpp>
#include <libchiaking/testing/in_memory_record_store.hpp>

#include <span>
#include <vector>

namespace {

namespace platform = libchiaking::platform;
namespace play = libchiaking::play;

class Offline final : public platform::HttpProvider {
 public:
  auto perform(const platform::HttpRequest&, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    return libchiaking::unexpected(platform::HttpError::disconnected);
  }
};

class NoCrypto final : public platform::CryptoProvider {
 public:
  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }
  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }
  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }
  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
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
  using libchiaking::ChiakiNg;
  libchiaking::platform::DefaultThreadingProvider threading;
  libchiaking::testing::InMemoryCredentialStore credentials{threading};
  libchiaking::testing::InMemoryConsoleStore consoles{threading};
  libchiaking::testing::InMemoryRecordStore records{threading};
  NoDatacenters datacenters;
  const ChiakiNg::Stores stores{credentials, consoles, datacenters, nullptr,
                                records};

  platform::DefaultPlatformFactory bare;
  const auto unopened = ChiakiNg::open(bare, stores);
  if (unopened || unopened.error() != ChiakiNg::Missing::http) return 1;

  Offline http;
  NoCrypto crypto;
  platform::DefaultPlatformFactory factory{
      platform::PlatformProviders{.crypto = &crypto, .http = &http}};
  auto opened = ChiakiNg::open(factory, stores);
  if (!opened) return 2;
  auto& chiaki = opened.value();
  const auto returned = chiaki.users().welcome_back();
  if (!returned || !returned.value().users.empty() ||
      !chiaki.consoles().remember_saved())
    return 3;
  if (chiaki.play().live() != nullptr ||
      !chiaki.consoles().everything().empty())
    return 4;
#ifdef LIBCHIAKING_CONSUMER_HAS_KAKUSAN
  constexpr auto absent =
      libchiaking::kakusan::Kakusan::Unavailable::device_store_unavailable;
#else
  constexpr auto absent = libchiaking::kakusan::Kakusan::Unavailable::not_built;
#endif
  const auto shared = chiaki.kakusan();
  if (shared || shared.error() != absent) return 5;
  const auto added =
      chiaki.users().add_home_user(libchiaking::AccountId{40}, "Player");
  if (!added) return 6;
  const auto trophies = chiaki.trophies().summary_of(*added.value());
  if (trophies || trophies.error().reason !=
                      libchiaking::users::Unanswered::Reason::signed_out)
    return 7;
  const auto played = libchiaking::NpTitleId::parse("PPSA00001_00");
  if (!played) return 7;
  const auto sets = chiaki.trophies().titles_for(
      *added.value(), std::span<const libchiaking::NpTitleId>{&*played, 1});
  if (sets ||
      sets.error().reason != libchiaking::users::Unanswered::Reason::signed_out)
    return 7;
  if (!chiaki.sign_out(added.value()->account_id()) ||
      !chiaki.users().everyone().empty())
    return 6;
  return 0;
}
