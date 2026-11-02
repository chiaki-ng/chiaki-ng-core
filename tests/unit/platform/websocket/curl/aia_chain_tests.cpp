#include "src/platform/websocket/curl/aia_chain.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace platform_curl_test {
namespace {

namespace detail = libchiaking::platform::detail;

class CurlAiaChain : public testing::Test {
 protected:
  struct KeyDeleter {
    void operator()(EVP_PKEY* value) const noexcept { EVP_PKEY_free(value); }
  };
  struct CertificateDeleter {
    void operator()(X509* value) const noexcept { X509_free(value); }
  };
  struct NameDeleter {
    void operator()(X509_NAME* value) const noexcept { X509_NAME_free(value); }
  };
  using Key = std::unique_ptr<EVP_PKEY, KeyDeleter>;
  using Certificate = std::unique_ptr<X509, CertificateDeleter>;
  using Name = std::unique_ptr<X509_NAME, NameDeleter>;
  using Bytes = std::vector<std::byte>;

  static constexpr const char* intermediate_url =
      "http://issuer.example.test/intermediate.der";

  void SetUp() override {
    ASSERT_TRUE(root_key);
    ASSERT_TRUE(intermediate_key);
    ASSERT_TRUE(leaf_key);
    ASSERT_TRUE(foreign_key);
    root = issue("Synthetic Root", root_key.get(), nullptr, root_key.get(),
                 true, nullptr, 1);
    intermediate = issue("Synthetic Intermediate", intermediate_key.get(),
                         root.get(), root_key.get(), true, nullptr, 2);
    leaf = issue("push.example.test", leaf_key.get(), intermediate.get(),
                 intermediate_key.get(), false, intermediate_url, 3);
    direct = issue("direct.example.test", leaf_key.get(), root.get(),
                   root_key.get(), false, nullptr, 4);
    foreign = issue("Synthetic Foreign", foreign_key.get(), nullptr,
                    foreign_key.get(), true, nullptr, 5);
    leaf_der = der(leaf.get());
    intermediate_der = der(intermediate.get());
    trust.roots.push_back(der(root.get()));
  }

  static void add_extension(X509* certificate, X509* issuer, int nid,
                            const char* value) {
    X509V3_CTX context;
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
    X509_EXTENSION* extension =
        X509V3_EXT_conf_nid(nullptr, &context, nid, value);
    ASSERT_NE(extension, nullptr);
    EXPECT_EQ(X509_add_ext(certificate, extension, -1), 1);
    X509_EXTENSION_free(extension);
  }

  static Certificate issue(const char* name, EVP_PKEY* key, X509* issuer,
                           EVP_PKEY* issuer_key, bool authority,
                           const char* issuer_url, long serial) {
    Certificate certificate{X509_new()};
    EXPECT_TRUE(certificate);
    if (!certificate) return certificate;
    EXPECT_EQ(X509_set_version(certificate.get(), 2), 1);
    EXPECT_EQ(
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), serial), 1);
    X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -86400);
    X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 86400);
    const Name subject{X509_NAME_new()};
    EXPECT_TRUE(subject);
    if (!subject) return certificate;
    EXPECT_EQ(X509_NAME_add_entry_by_txt(
                  subject.get(), "CN", MBSTRING_ASC,
                  reinterpret_cast<const unsigned char*>(name), -1, -1, 0),
              1);
    EXPECT_EQ(X509_set_subject_name(certificate.get(), subject.get()), 1);
    X509* signer = issuer ? issuer : certificate.get();
    EXPECT_EQ(
        X509_set_issuer_name(certificate.get(), X509_get_subject_name(signer)),
        1);
    EXPECT_EQ(X509_set_pubkey(certificate.get(), key), 1);
    add_extension(certificate.get(), signer, NID_basic_constraints,
                  authority ? "critical,CA:TRUE" : "CA:FALSE");
    if (authority)
      add_extension(certificate.get(), signer, NID_key_usage,
                    "critical,keyCertSign,cRLSign");
    if (issuer_url) {
      const std::string access = std::string("caIssuers;URI:") + issuer_url;
      add_extension(certificate.get(), signer, NID_info_access, access.c_str());
    }
    EXPECT_GT(X509_sign(certificate.get(), issuer_key, EVP_sha256()), 0);
    return certificate;
  }

  static Bytes der(X509* certificate) {
    const int size = i2d_X509(certificate, nullptr);
    EXPECT_GT(size, 0);
    if (size <= 0) return {};
    Bytes out(static_cast<std::size_t>(size));
    auto* cursor = reinterpret_cast<unsigned char*>(out.data());
    EXPECT_EQ(i2d_X509(certificate, &cursor), size);
    return out;
  }

  static Bytes pem(X509* certificate) {
    BIO* bio = BIO_new(BIO_s_mem());
    EXPECT_NE(bio, nullptr);
    if (bio == nullptr) return {};
    EXPECT_EQ(PEM_write_bio_X509(bio, certificate), 1);
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    const auto* bytes = reinterpret_cast<const std::byte*>(data);
    Bytes out(bytes, bytes + size);
    BIO_free(bio);
    return out;
  }

  static std::optional<Bytes> unreachable(const std::string&) {
    return std::nullopt;
  }

  Key root_key{EVP_EC_gen("P-256")};
  Key intermediate_key{EVP_EC_gen("P-256")};
  Key leaf_key{EVP_EC_gen("P-256")};
  Key foreign_key{EVP_EC_gen("P-256")};
  Certificate root;
  Certificate intermediate;
  Certificate leaf;
  Certificate direct;
  Certificate foreign;
  Bytes leaf_der;
  Bytes intermediate_der;
  detail::AiaTrust trust;
};

TEST_F(CurlAiaChain, ReadsTheIssuerUrlOfALeaf) {
  EXPECT_EQ(detail::AiaChain::issuer_url(leaf_der),
            std::string(intermediate_url));
}

TEST_F(CurlAiaChain, FindsNoIssuerUrlInACertificateWithoutOne) {
  EXPECT_FALSE(detail::AiaChain::issuer_url(intermediate_der));
}

TEST_F(CurlAiaChain, ConvertsAPemCertificateToDer) {
  EXPECT_EQ(detail::AiaChain::certificate_der(pem(intermediate.get())),
            intermediate_der);
}

TEST_F(CurlAiaChain, RefusesATruncatedCertificate) {
  EXPECT_FALSE(detail::AiaChain::certificate_der(
      Bytes{std::byte{0x30}, std::byte{0x01}}));
}

TEST_F(CurlAiaChain, RecoversTheIntermediateServedAsDerOrPem) {
  for (const bool as_pem : {false, true}) {
    SCOPED_TRACE(as_pem ? "served as PEM" : "served as DER");
    std::vector<std::string> fetched;
    const auto recovered = detail::AiaChain::recover_intermediates(
        leaf_der,
        [&](const std::string& url) -> std::optional<Bytes> {
          fetched.push_back(url);
          if (url != intermediate_url) return std::nullopt;
          return as_pem ? pem(intermediate.get()) : intermediate_der;
        },
        trust);
    ASSERT_TRUE(recovered);
    ASSERT_FALSE(recovered->empty());
    ASSERT_EQ(fetched.size(), 1U);
    EXPECT_EQ(fetched.front(), intermediate_url);
    const auto* bytes = reinterpret_cast<const std::byte*>(recovered->data());
    EXPECT_EQ(detail::AiaChain::certificate_der(
                  Bytes(bytes, bytes + recovered->size())),
              intermediate_der);
  }
}

TEST_F(CurlAiaChain, FailsWhenTheIssuerCannotBeFetched) {
  EXPECT_FALSE(
      detail::AiaChain::recover_intermediates(leaf_der, unreachable, trust));
}

TEST_F(CurlAiaChain, NeedsNothingForALeafSignedByATrustedRoot) {
  const auto complete = detail::AiaChain::recover_intermediates(
      der(direct.get()), unreachable, trust);
  ASSERT_TRUE(complete);
  EXPECT_TRUE(complete->empty());
}

TEST_F(CurlAiaChain, RejectsAnIntermediateThatNoTrustedRootSigned) {
  detail::AiaTrust untrusted;
  untrusted.roots.push_back(der(foreign.get()));
  const auto rejected = detail::AiaChain::recover_intermediates(
      leaf_der,
      [&](const std::string&) -> std::optional<Bytes> {
        return intermediate_der;
      },
      untrusted);
  EXPECT_FALSE(rejected);
}

}  // namespace
}  // namespace platform_curl_test
