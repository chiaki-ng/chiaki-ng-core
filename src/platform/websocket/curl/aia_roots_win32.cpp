#include "aia_chain.hpp"

#include <wincrypt.h>
#include <windows.h>

namespace libchiaking::platform::detail {

auto AiaChain::native_roots() -> std::vector<std::vector<std::byte>> {
  std::vector<std::vector<std::byte>> roots;
  HCERTSTORE store = CertOpenSystemStoreA(0, "ROOT");
  if (store == nullptr) return roots;
  try {
    PCCERT_CONTEXT certificate = nullptr;
    while ((certificate = CertEnumCertificatesInStore(store, certificate)) !=
           nullptr) {
      const auto* first =
          reinterpret_cast<const std::byte*>(certificate->pbCertEncoded);
      roots.emplace_back(first, first + certificate->cbCertEncoded);
    }
  } catch (...) {
    roots.clear();
  }
  CertCloseStore(store, 0);
  return roots;
}

}  // namespace libchiaking::platform::detail
