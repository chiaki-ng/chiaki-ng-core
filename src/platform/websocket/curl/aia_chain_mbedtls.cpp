#include "aia_chain.hpp"

#include <mbedtls/asn1.h>
#include <mbedtls/pem.h>
#include <mbedtls/x509_crt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace libchiaking::platform::detail {

class AiaChain::Certificates {
 public:
  class Crt {
   public:
    Crt() noexcept { mbedtls_x509_crt_init(&value_); }
    ~Crt() { mbedtls_x509_crt_free(&value_); }
    Crt(const Crt&) = delete;
    auto operator=(const Crt&) -> Crt& = delete;

    [[nodiscard]] auto get() noexcept -> mbedtls_x509_crt* { return &value_; }
    [[nodiscard]] auto empty() const noexcept -> bool {
      return value_.raw.p == nullptr;
    }
    [[nodiscard]] auto der() const -> std::vector<std::byte> {
      const auto* begin = reinterpret_cast<const std::byte*>(value_.raw.p);
      return {begin, begin + value_.raw.len};
    }
    [[nodiscard]] auto self_signed() const noexcept -> bool {
      return value_.subject_raw.len == value_.issuer_raw.len &&
             std::memcmp(value_.subject_raw.p, value_.issuer_raw.p,
                         value_.subject_raw.len) == 0;
    }

   private:
    mbedtls_x509_crt value_;
  };

  [[nodiscard]] static auto add(Crt& chain, std::span<const std::byte> der)
      -> bool {
    return !der.empty() &&
           mbedtls_x509_crt_parse_der(chain.get(), bytes(der), der.size()) == 0;
  }

  [[nodiscard]] static auto parse_pem(Crt& chain,
                                      std::span<const std::byte> encoded)
      -> bool {
    std::string text(reinterpret_cast<const char*>(encoded.data()),
                     encoded.size());
    return mbedtls_x509_crt_parse(
               chain.get(), reinterpret_cast<const unsigned char*>(text.c_str()),
               text.size() + 1) >= 0 &&
           !chain.empty();
  }

  [[nodiscard]] static auto issuers_of(std::span<const std::byte> der)
      -> std::optional<std::string> {
    Crt certificate;
    std::optional<std::string> url;
    if (der.empty() ||
        mbedtls_x509_crt_parse_der_with_ext_cb(certificate.get(), bytes(der),
                                               der.size(), 1, &on_extension,
                                               &url) != 0)
      return std::nullopt;
    return url;
  }

  [[nodiscard]] static auto load_anchors(const AiaTrust& trust, Crt& anchors)
      -> bool {
    for (const auto& root : trust.roots) (void)add(anchors, root);
    if (!trust.roots.empty()) return !anchors.empty();
    if (!trust.ca_file.empty())
      (void)mbedtls_x509_crt_parse_file(anchors.get(), trust.ca_file.c_str());
    if (!trust.ca_path.empty())
      (void)mbedtls_x509_crt_parse_path(anchors.get(), trust.ca_path.c_str());
    return !anchors.empty();
  }

  [[nodiscard]] static auto delegates(const AiaTrust& trust) noexcept -> bool {
    return trust.roots.empty() && trust.ca_file.empty() && trust.ca_path.empty();
  }

  [[nodiscard]] static auto verifies(
      const AiaTrust& trust, std::span<const std::byte> leaf,
      const std::vector<std::vector<std::byte>>& found) -> bool {
    if (delegates(trust)) return !found.empty() && links(leaf, found);
    Crt anchors;
    Crt chain;
    if (!load_anchors(trust, anchors) || !add(chain, leaf)) return false;
    for (const auto& der : found) (void)add(chain, der);
    std::uint32_t flags = 0;
    return mbedtls_x509_crt_verify(chain.get(), anchors.get(), nullptr, nullptr,
                                   &flags, nullptr, nullptr) == 0;
  }

  [[nodiscard]] static auto intermediates(
      const std::vector<std::vector<std::byte>>& found)
      -> std::optional<std::string> {
    std::string pem;
    for (const auto& der : found) {
      Crt certificate;
      if (!add(certificate, der)) return std::nullopt;
      if (certificate.self_signed()) continue;
      auto encoded = encode_pem(der);
      if (!encoded) return std::nullopt;
      pem += *encoded;
    }
    if (pem.empty()) return std::nullopt;
    return pem;
  }

 private:
  static constexpr std::array<unsigned char, 8> info_access_oid{
      0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x01, 0x01};
  static constexpr std::array<unsigned char, 8> ca_issuers_oid{
      0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x30, 0x02};
  static constexpr int sequence_tag =
      MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE;
  static constexpr int uri_tag = MBEDTLS_ASN1_CONTEXT_SPECIFIC | 6;
  static constexpr std::string_view pem_header =
      "-----BEGIN CERTIFICATE-----\n";
  static constexpr std::string_view pem_footer = "-----END CERTIFICATE-----\n";

  [[nodiscard]] static auto bytes(std::span<const std::byte> data) noexcept
      -> const unsigned char* {
    return reinterpret_cast<const unsigned char*>(data.data());
  }

  [[nodiscard]] static auto matches(const unsigned char* data,
                                    std::size_t length,
                                    std::span<const unsigned char> oid) noexcept
      -> bool {
    return length == oid.size() && std::equal(oid.begin(), oid.end(), data);
  }

  [[nodiscard]] static auto ca_issuers(const unsigned char* cursor,
                                       const unsigned char* end)
      -> std::optional<std::string> {
    std::size_t length = 0;
    if (mbedtls_asn1_get_tag(const_cast<unsigned char**>(&cursor), end, &length,
                             sequence_tag) != 0)
      return std::nullopt;
    while (cursor < end) {
      if (mbedtls_asn1_get_tag(const_cast<unsigned char**>(&cursor), end,
                               &length, sequence_tag) != 0)
        return std::nullopt;
      const unsigned char* const next = cursor + length;
      std::size_t oid_length = 0;
      if (mbedtls_asn1_get_tag(const_cast<unsigned char**>(&cursor), next,
                               &oid_length, MBEDTLS_ASN1_OID) != 0)
        return std::nullopt;
      const bool issuers = matches(cursor, oid_length, ca_issuers_oid);
      cursor += oid_length;
      std::size_t uri_length = 0;
      if (issuers &&
          mbedtls_asn1_get_tag(const_cast<unsigned char**>(&cursor), next,
                               &uri_length, uri_tag) == 0 &&
          uri_length > 0) {
        std::string url(reinterpret_cast<const char*>(cursor), uri_length);
        if (url.find('\0') == std::string::npos) return url;
      }
      cursor = next;
    }
    return std::nullopt;
  }

  static auto on_extension(void* context, const mbedtls_x509_crt*,
                           const mbedtls_x509_buf* oid, int,
                           const unsigned char* cursor,
                           const unsigned char* end) -> int {
    auto& url = *static_cast<std::optional<std::string>*>(context);
    if (!url && matches(oid->p, oid->len, info_access_oid))
      url = ca_issuers(cursor, end);
    return 0;
  }

  [[nodiscard]] static auto signs(std::span<const std::byte> child,
                                  std::span<const std::byte> parent) -> bool {
    Crt subject;
    Crt issuer;
    if (!add(subject, child) || !add(issuer, parent)) return false;
    std::uint32_t flags = 0;
    return mbedtls_x509_crt_verify(subject.get(), issuer.get(), nullptr,
                                   nullptr, &flags, nullptr, nullptr) == 0;
  }

  [[nodiscard]] static auto links(
      std::span<const std::byte> leaf,
      const std::vector<std::vector<std::byte>>& found) -> bool {
    std::span<const std::byte> child = leaf;
    for (const auto& parent : found) {
      if (!signs(child, parent)) return false;
      child = parent;
    }
    return true;
  }

  [[nodiscard]] static auto encode_pem(std::span<const std::byte> der)
      -> std::optional<std::string> {
    std::string out(der.size() * 2 + pem_header.size() + pem_footer.size(),
                    '\0');
    std::size_t written = 0;
    if (mbedtls_pem_write_buffer(pem_header.data(), pem_footer.data(),
                                 bytes(der), der.size(),
                                 reinterpret_cast<unsigned char*>(out.data()),
                                 out.size(), &written) != 0 ||
        written == 0)
      return std::nullopt;
    out.resize(written - 1);
    return out;
  }
};

auto AiaChain::issuer_url(std::span<const std::byte> certificate_der)
    -> std::optional<std::string> {
  return Certificates::issuers_of(certificate_der);
}

auto AiaChain::certificate_der(std::span<const std::byte> encoded)
    -> std::optional<std::vector<std::byte>> {
  if (encoded.empty()) return std::nullopt;
  Certificates::Crt certificate;
  const bool parsed = encoded.front() == der_sequence_tag
                          ? Certificates::add(certificate, encoded)
                          : Certificates::parse_pem(certificate, encoded);
  if (!parsed) return std::nullopt;
  return certificate.der();
}

auto AiaChain::recover_intermediates(std::span<const std::byte> leaf_der,
                                     const AiaFetch& fetch,
                                     const AiaTrust& trust)
    -> std::optional<std::string> {
  Certificates::Crt leaf;
  if (!fetch || !Certificates::add(leaf, leaf_der)) return std::nullopt;
  std::vector<std::vector<std::byte>> found;
  std::vector<std::byte> current(leaf_der.begin(), leaf_der.end());
  for (std::size_t hop = 0; hop <= max_hops; ++hop) {
    if (Certificates::verifies(trust, leaf_der, found)) {
      if (found.empty()) return std::string{};
      return Certificates::intermediates(found);
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
