#include "aia_chain.hpp"
#include "src/core/utils/memory/c_handle.hpp"

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstring>
#include <limits>
#include <memory>

namespace libchiaking::platform::detail {
class AiaChain::Certificates {
 public:
  struct StackDeleter {
    void operator()(STACK_OF(X509) * value) const noexcept {
      sk_X509_pop_free(value, X509_free);
    }
  };
  using X509Ptr = memory::CHandle<X509, X509_free>;
  using StorePtr = memory::CHandle<X509_STORE, X509_STORE_free>;
  using StoreContextPtr = memory::CHandle<X509_STORE_CTX, X509_STORE_CTX_free>;
  using StackPtr = std::unique_ptr<STACK_OF(X509), StackDeleter>;
  using BioPtr = memory::CHandle<BIO, BIO_free>;
  using InfoAccessPtr =
      memory::CHandle<AUTHORITY_INFO_ACCESS, AUTHORITY_INFO_ACCESS_free>;

  static auto parse_der(std::span<const std::byte> der) -> X509Ptr {
    if (der.empty() ||
        der.size() > static_cast<std::size_t>(std::numeric_limits<long>::max()))
      return {};
    const auto* cursor = reinterpret_cast<const unsigned char*>(der.data());
    return X509Ptr{d2i_X509(nullptr, &cursor, static_cast<long>(der.size()))};
  }

  static auto load_trust(const AiaTrust& trust) -> StorePtr {
    StorePtr store{X509_STORE_new()};
    if (!store) return {};
    if (!trust.roots.empty()) {
      std::size_t added = 0;
      for (const auto& root : trust.roots) {
        auto certificate = parse_der(root);
        if (certificate &&
            X509_STORE_add_cert(store.get(), certificate.get()) == 1)
          ++added;
      }
      return added == 0 ? StorePtr{} : std::move(store);
    }
    if (trust.ca_file.empty() && trust.ca_path.empty()) {
      if (X509_STORE_set_default_paths(store.get()) != 1) return {};
      return store;
    }
    const char* file = trust.ca_file.empty() ? nullptr : trust.ca_file.c_str();
    const char* path = trust.ca_path.empty() ? nullptr : trust.ca_path.c_str();
    if (X509_STORE_load_locations(store.get(), file, path) != 1) return {};
    return store;
  }

  [[nodiscard]] static auto verify(
      X509_STORE* store, X509* leaf,
      const std::vector<std::vector<std::byte>>& found,
      std::string* intermediates) -> bool {
    StackPtr untrusted{sk_X509_new_null()};
    StoreContextPtr context{X509_STORE_CTX_new()};
    if (!untrusted || !context) return false;
    for (const auto& der : found) {
      auto certificate = parse_der(der);
      if (!certificate) continue;
      if (sk_X509_push(untrusted.get(), certificate.get()) == 0) return false;
      (void)certificate.release();
    }
    if (X509_STORE_CTX_init(context.get(), store, leaf, untrusted.get()) != 1)
      return false;
    if (X509_verify_cert(context.get()) != 1) return false;
    if (!intermediates) return true;
    BioPtr bio{BIO_new(BIO_s_mem())};
    if (!bio) return false;
    STACK_OF(X509)* chain = X509_STORE_CTX_get0_chain(context.get());
    for (int index = 1; index < sk_X509_num(chain) - 1; ++index) {
      if (PEM_write_bio_X509(bio.get(), sk_X509_value(chain, index)) != 1)
        return false;
    }
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio.get(), &data);
    intermediates->assign(data != nullptr && size > 0 ? data : "",
                          size > 0 ? static_cast<std::size_t>(size) : 0U);
    return true;
  }
};

auto AiaChain::issuer_url(std::span<const std::byte> certificate_der)
    -> std::optional<std::string> {
  auto certificate = Certificates::parse_der(certificate_der);
  if (!certificate) return std::nullopt;
  Certificates::InfoAccessPtr access{static_cast<AUTHORITY_INFO_ACCESS*>(
      X509_get_ext_d2i(certificate.get(), NID_info_access, nullptr, nullptr))};
  if (!access) return std::nullopt;
  for (int index = 0; index < sk_ACCESS_DESCRIPTION_num(access.get());
       ++index) {
    const ACCESS_DESCRIPTION* description =
        sk_ACCESS_DESCRIPTION_value(access.get(), index);
    if (!description || OBJ_obj2nid(description->method) != NID_ad_ca_issuers ||
        !description->location || description->location->type != GEN_URI)
      continue;
    const ASN1_IA5STRING* uri =
        description->location->d.uniformResourceIdentifier;
    const int length = ASN1_STRING_length(uri);
    const unsigned char* data = ASN1_STRING_get0_data(uri);
    if (length <= 0 || !data) continue;
    std::string url(reinterpret_cast<const char*>(data),
                    static_cast<std::size_t>(length));
    if (url.find('\0') != std::string::npos) continue;
    return url;
  }
  return std::nullopt;
}

auto AiaChain::certificate_der(std::span<const std::byte> encoded)
    -> std::optional<std::vector<std::byte>> {
  if (encoded.empty()) return std::nullopt;
  if (encoded.front() == der_sequence_tag) {
    if (!Certificates::parse_der(encoded)) return std::nullopt;
    return std::vector<std::byte>(encoded.begin(), encoded.end());
  }
  if (encoded.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max()))
    return std::nullopt;
  Certificates::BioPtr bio{
      BIO_new_mem_buf(encoded.data(), static_cast<int>(encoded.size()))};
  if (!bio) return std::nullopt;
  Certificates::X509Ptr certificate{
      PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr)};
  if (!certificate) return std::nullopt;
  const int size = i2d_X509(certificate.get(), nullptr);
  if (size <= 0) return std::nullopt;
  std::vector<std::byte> der(static_cast<std::size_t>(size));
  auto* cursor = reinterpret_cast<unsigned char*>(der.data());
  if (i2d_X509(certificate.get(), &cursor) != size) return std::nullopt;
  return der;
}

auto AiaChain::recover_intermediates(std::span<const std::byte> leaf_der,
                                     const AiaFetch& fetch,
                                     const AiaTrust& trust)
    -> std::optional<std::string> {
  auto leaf = Certificates::parse_der(leaf_der);
  if (!leaf || !fetch) return std::nullopt;
  auto store = Certificates::load_trust(trust);
  if (!store) return std::nullopt;
  std::vector<std::vector<std::byte>> found;
  std::vector<std::byte> current(leaf_der.begin(), leaf_der.end());
  for (std::size_t hop = 0; hop <= max_hops; ++hop) {
    if (Certificates::verify(store.get(), leaf.get(), found, nullptr)) {
      if (found.empty()) return std::string{};
      std::string intermediates;
      if (!Certificates::verify(store.get(), leaf.get(), found,
                                &intermediates) ||
          intermediates.empty())
        return std::nullopt;
      return intermediates;
    }
    if (hop == max_hops) break;
    auto url = issuer_url(current);
    if (!url ||
        (url->rfind("http://", 0) != 0 && url->rfind("https://", 0) != 0))
      break;
    auto fetched = fetch(*url);
    if (!fetched) break;
    auto der = certificate_der(*fetched);
    if (!der) break;
    current = *der;
    found.push_back(std::move(*der));
  }
  return std::nullopt;
}

}  // namespace libchiaking::platform::detail
