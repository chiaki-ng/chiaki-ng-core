#include "libchiaking/platform/defaults.hpp"
#include "libchiaking/kakusan.hpp"
#include "libchiaking/chiaki_ng.hpp"
#include "libchiaking/platform/platform.hpp"
#include "libchiaking/platform/stock_providers.hpp"
#include "libchiaking/testing/in_memory_console_store.hpp"
#include "libchiaking/testing/in_memory_credential_store.hpp"
#include "libchiaking/testing/in_memory_record_store.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace {

namespace kakusan = libchiaking::kakusan;
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

class MemoryStore final : public kakusan::DeviceStore {
 public:
  auto load_identity()
      -> libchiaking::Expected<std::optional<kakusan::SavedIdentity>,
                               Error> override {
    if (!identity_) return std::optional<kakusan::SavedIdentity>{};
    return std::optional<kakusan::SavedIdentity>{identity_->clone()};
  }
  auto save_identity(const kakusan::SavedIdentity& identity)
      -> libchiaking::Expected<void, Error> override {
    identity_.emplace(identity.clone());
    return {};
  }
  auto load_devices()
      -> libchiaking::Expected<std::vector<kakusan::SavedDevice>,
                               Error> override {
    return std::vector<kakusan::SavedDevice>{};
  }
  auto save(const kakusan::SavedDevice&)
      -> libchiaking::Expected<void, Error> override {
    return {};
  }
  auto remove(const kakusan::IdentityKey&)
      -> libchiaking::Expected<void, Error> override {
    return {};
  }

 private:
  std::optional<kakusan::SavedIdentity> identity_;
};

}  // namespace

int main() {
  using libchiaking::ChiakiNg;
  MemoryStore store;
  Offline offline;
  platform::DefaultThreadingProvider threading;
  libchiaking::testing::InMemoryCredentialStore credentials{threading};
  libchiaking::testing::InMemoryConsoleStore consoles{threading};
  libchiaking::testing::InMemoryRecordStore records{threading};
  NoDatacenters datacenters;
  const ChiakiNg::Stores stores{credentials, consoles, datacenters, &store,
                                records};
  NoCrypto no_crypto;
  platform::DefaultPlatformFactory bare{
      platform::PlatformProviders{.crypto = &no_crypto, .http = &offline}};
  auto plain = ChiakiNg::open(bare, stores);
  if (!plain) return 11;
  const auto absent = plain.value().kakusan();
  if (absent ||
      absent.error() != kakusan::Kakusan::Unavailable::no_secure_transport)
    return 9;
#if defined(LIBCHIAKING_CONSUMER_HAS_DTLS)
  platform::openssl::OpenSslCryptoProvider crypto;
  platform::openssl::OpenSslSecureDatagramProvider secure_datagram;
  platform::DefaultPlatformFactory secured{
      platform::PlatformProviders{.crypto = &crypto,
                                  .http = &offline,
                                  .secure_datagram = &secure_datagram}};
  auto opened = ChiakiNg::open(secured, stores);
  if (!opened) return 12;
  auto root = opened.value().kakusan();
  if (!root) return 10;
  auto reopened = ChiakiNg::open(secured, stores);
  if (!reopened) return 12;
  const auto again = reopened.value().kakusan();
  if (!again ||
      again.value().this_device().key() != root.value().this_device().key())
    return 1;
  if (root.value().read("RCP1:x")) return 2;
  if (kakusan::SharingRules::create(4, 0)) return 3;
  if (!kakusan::SharingRules::create(4, 1)) return 4;
  if (!root.value().remembered().all().empty()) return 5;
#endif
  if (kakusan::SharedSession::diagnostic_limit != 512 ||
      kakusan::VisitOutcome::refusal_base != 5000)
    return 8;
  return 0;
}
