#include "tests/unit/core/net/kakusan/datagram_security_rig.hpp"

#include "src/core/net/kakusan/datagram_security.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "tests/support/enum_printing.hpp"

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace net_kakusan_test {
namespace {

namespace kakusan = libchiaking::net::kakusan;

class RawPeerRig : public DatagramSecurityRig {
 public:
  static constexpr int no_alert = -1;
  static constexpr int alert_close_notify = 0;
  static constexpr int alert_handshake_failure = 40;
  static constexpr int alert_bad_certificate = 42;
  static constexpr int alert_protocol_version = 70;

  struct ContextDeleter {
    void operator()(SSL_CTX* context) const noexcept { SSL_CTX_free(context); }
  };
  struct SslDeleter {
    void operator()(SSL* ssl) const noexcept { SSL_free(ssl); }
  };
  struct BioDeleter {
    void operator()(BIO* bio) const noexcept { BIO_free(bio); }
  };
  struct PkeyDeleter {
    void operator()(EVP_PKEY* key) const noexcept { EVP_PKEY_free(key); }
  };
  struct CertificateDeleter {
    void operator()(X509* certificate) const noexcept {
      X509_free(certificate);
    }
  };
  struct NameDeleter {
    void operator()(X509_NAME* name) const noexcept { X509_NAME_free(name); }
  };

  struct Identity {
    std::unique_ptr<EVP_PKEY, PkeyDeleter> key;
    std::unique_ptr<X509, CertificateDeleter> certificate;
    Digest digest{};
  };

  struct Options {
    bool client{true};
    bool certificate{true};
    bool offer_alpn{true};
    bool select_alpn{true};
    bool extended_master_secret{true};
    bool chain{false};
    int max_version{DTLS1_2_VERSION};
    std::string cipher{"ECDHE-ECDSA-AES128-GCM-SHA256"};
    std::string groups{"P-256"};
    std::string curve{"P-256"};
    std::size_t comment_size{0};
  };

  struct Alerts {
    int received{-1};
    int sent{-1};
  };

  struct Peer {
    std::unique_ptr<SSL_CTX, ContextDeleter> context;
    std::unique_ptr<SSL, SslDeleter> ssl;
    std::unique_ptr<BIO, BioDeleter> network;
    Identity identity;
    Identity extra;
    std::unique_ptr<Alerts> alerts{std::make_unique<Alerts>()};
  };

  struct Result {
    std::optional<Error> error;
    State state{State::idle};
    bool raw_finished{false};
    Alerts alerts;
  };

  static auto identity(const std::string& curve, std::size_t comment_size)
      -> Identity {
    Identity made{
        std::unique_ptr<EVP_PKEY, PkeyDeleter>{EVP_EC_gen(curve.c_str())},
        std::unique_ptr<X509, CertificateDeleter>{X509_new()},
        {}};
    EXPECT_TRUE(made.key);
    EXPECT_TRUE(made.certificate);
    if (!made.key || !made.certificate) return made;
    X509* certificate = made.certificate.get();
    EXPECT_EQ(X509_set_version(certificate, X509_VERSION_3), 1);
    EXPECT_EQ(ASN1_INTEGER_set(X509_get_serialNumber(certificate), 7), 1);
    EXPECT_NE(X509_gmtime_adj(X509_getm_notBefore(certificate), -60), nullptr);
    EXPECT_NE(X509_gmtime_adj(X509_getm_notAfter(certificate), 3600), nullptr);
    const std::unique_ptr<X509_NAME, NameDeleter> name{X509_NAME_new()};
    EXPECT_TRUE(name);
    EXPECT_EQ(X509_NAME_add_entry_by_txt(
                  name.get(), "CN", MBSTRING_ASC,
                  reinterpret_cast<const unsigned char*>("raw"), -1, -1, 0),
              1);
    EXPECT_EQ(X509_set_subject_name(certificate, name.get()), 1);
    EXPECT_EQ(X509_set_issuer_name(certificate, name.get()), 1);
    EXPECT_EQ(X509_set_pubkey(certificate, made.key.get()), 1);
    if (comment_size > 0) {
      const std::string text(comment_size, 'c');
      ASN1_IA5STRING* comment = ASN1_IA5STRING_new();
      EXPECT_NE(comment, nullptr);
      EXPECT_EQ(
          ASN1_STRING_set(comment, text.data(), static_cast<int>(text.size())),
          1);
      EXPECT_EQ(X509_add1_ext_i2d(certificate, NID_netscape_comment, comment, 0,
                                  X509V3_ADD_DEFAULT),
                1);
      ASN1_IA5STRING_free(comment);
    }
    EXPECT_GT(X509_sign(certificate, made.key.get(), EVP_sha256()), 0);
    const int size = i2d_X509(certificate, nullptr);
    EXPECT_GT(size, 0);
    if (size <= 0) return made;
    std::vector<unsigned char> der(static_cast<std::size_t>(size));
    unsigned char* cursor = der.data();
    EXPECT_EQ(i2d_X509(certificate, &cursor), size);
    unsigned int written = 0;
    EXPECT_EQ(EVP_Digest(der.data(), der.size(),
                         reinterpret_cast<unsigned char*>(made.digest.data()),
                         &written, EVP_sha256(), nullptr),
              1);
    EXPECT_EQ(written, made.digest.size());
    return made;
  }

  static auto select(SSL*, const unsigned char** selected,
                     unsigned char* selected_length,
                     const unsigned char* offered, unsigned int offered_length,
                     void*) -> int {
    const std::string_view wanted = kakusan::application_protocol;
    unsigned int offset = 0;
    while (offset < offered_length) {
      const unsigned int length = offered[offset];
      ++offset;
      if (length == wanted.size() &&
          std::memcmp(offered + offset, wanted.data(), length) == 0) {
        *selected = offered + offset;
        *selected_length = static_cast<unsigned char>(length);
        return SSL_TLSEXT_ERR_OK;
      }
      offset += length;
    }
    return SSL_TLSEXT_ERR_NOACK;
  }

  static void alert(const SSL* ssl, int where, int value) {
    auto* alerts = static_cast<Alerts*>(SSL_get_app_data(ssl));
    if ((where & SSL_CB_READ_ALERT) == SSL_CB_READ_ALERT &&
        alerts->received < 0)
      alerts->received = value & 0xff;
    if ((where & SSL_CB_WRITE_ALERT) == SSL_CB_WRITE_ALERT && alerts->sent < 0)
      alerts->sent = value & 0xff;
  }

  static auto make_raw(const Options& options) -> Peer {
    Peer peer;
    peer.context.reset(SSL_CTX_new(options.client ? DTLS_client_method()
                                                  : DTLS_server_method()));
    SSL_CTX* context = peer.context.get();
    EXPECT_NE(context, nullptr);
    if (context == nullptr) return peer;
    SSL_CTX_set_security_level(context, 0);
    EXPECT_EQ(SSL_CTX_set_min_proto_version(context, DTLS1_VERSION), 1);
    EXPECT_EQ(SSL_CTX_set_max_proto_version(context, options.max_version), 1);
    EXPECT_EQ(SSL_CTX_set_cipher_list(context, options.cipher.c_str()), 1);
    EXPECT_EQ(SSL_CTX_set1_groups_list(context, options.groups.c_str()), 1);
    SSL_CTX_set_options(context, SSL_OP_NO_QUERY_MTU | SSL_OP_NO_TICKET);
    if (!options.extended_master_secret)
      SSL_CTX_set_options(context, SSL_OP_NO_EXTENDED_MASTER_SECRET);
    SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
    if (!options.client && options.select_alpn)
      SSL_CTX_set_alpn_select_cb(context, &RawPeerRig::select, nullptr);
    if (options.certificate) {
      peer.identity = identity(options.curve, options.comment_size);
      EXPECT_EQ(
          SSL_CTX_use_certificate(context, peer.identity.certificate.get()), 1);
      EXPECT_EQ(SSL_CTX_use_PrivateKey(context, peer.identity.key.get()), 1);
      if (options.chain) {
        peer.extra = identity("P-256", 0);
        EXPECT_EQ(
            SSL_CTX_add1_chain_cert(context, peer.extra.certificate.get()), 1);
      }
    }
    peer.ssl.reset(SSL_new(context));
    SSL* ssl = peer.ssl.get();
    EXPECT_NE(ssl, nullptr);
    if (ssl == nullptr) return peer;
    if (options.client && options.offer_alpn) {
      std::string protocols;
      protocols.push_back(
          static_cast<char>(kakusan::application_protocol.size()));
      protocols.append(kakusan::application_protocol);
      const auto* encoded =
          reinterpret_cast<const unsigned char*>(protocols.data());
      EXPECT_EQ(SSL_set_alpn_protos(
                    ssl, encoded, static_cast<unsigned int>(protocols.size())),
                0);
    }
    BIO* inner = nullptr;
    BIO* outer = nullptr;
    EXPECT_EQ(BIO_new_bio_dgram_pair(&inner, 0, &outer, 0), 1);
    peer.network.reset(outer);
    SSL_set_bio(ssl, inner, inner);
    EXPECT_GT(SSL_set_mtu(ssl, static_cast<long>(kakusan::max_datagram_size)),
              0);
    SSL_set_app_data(ssl, peer.alerts.get());
    SSL_set_info_callback(ssl, &RawPeerRig::alert);
    if (options.client)
      SSL_set_connect_state(ssl);
    else
      SSL_set_accept_state(ssl);
    return peer;
  }

  static void drive(Peer& peer) {
    SSL* ssl = peer.ssl.get();
    if (SSL_is_init_finished(ssl) != 1)
      static_cast<void>(SSL_do_handshake(ssl));
    if (SSL_is_init_finished(ssl) == 1) {
      std::array<unsigned char, 2048> buffer{};
      while (SSL_read(ssl, buffer.data(), static_cast<int>(buffer.size())) >
             0) {
      }
    }
    ERR_clear_error();
  }

  static auto drain_raw(Peer& peer) -> Batch {
    Batch batch;
    while (true) {
      const std::size_t pending = BIO_ctrl_pending(peer.network.get());
      if (pending == 0) return batch;
      EXPECT_LE(pending, kakusan::max_datagram_size);
      Datagram datagram(pending);
      EXPECT_EQ(BIO_read(peer.network.get(), datagram.data(),
                         static_cast<int>(pending)),
                static_cast<int>(pending));
      batch.push_back(std::move(datagram));
    }
  }

  static void write_raw(Peer& raw, const Datagram& datagram) {
    EXPECT_EQ(BIO_write(raw.network.get(), datagram.data(),
                        static_cast<int>(datagram.size())),
              static_cast<int>(datagram.size()));
  }

  static auto exchange_raw(kakusan::DatagramSecurity& ours, Peer& raw,
                           std::uint64_t now) -> std::optional<Error> {
    std::optional<Error> error;
    drive(raw);
    std::size_t sent_by_raw = 0;
    for (int round = 0; round < 64; ++round) {
      const auto from_raw = drain_raw(raw);
      const auto from_ours = drain(ours);
      sent_by_raw += from_raw.size();
      if (from_raw.empty() && from_ours.empty()) {
        EXPECT_GT(sent_by_raw, 0U);
        return error;
      }
      for (const auto& datagram : from_raw) {
        const auto received = ours.receive(datagram, now);
        if (!received && !error) error = received.error();
      }
      for (const auto& datagram : from_ours) write_raw(raw, datagram);
      drive(raw);
    }
    ADD_FAILURE() << "the raw exchange did not settle within 64 rounds";
    return error;
  }

  static auto run_raw(const Options& options) -> Result {
    auto raw = make_raw(options);
    auto ours = make(options.client ? Role::server : Role::client);
    Result result;
    if (!raw.ssl || !ours) {
      ADD_FAILURE() << "the peers could not be made";
      return result;
    }
    const auto now = real_now();
    EXPECT_TRUE(ours->start(raw.identity.digest, now));
    result.error = exchange_raw(*ours, raw, now);
    result.state = ours->state();
    result.raw_finished = SSL_is_init_finished(raw.ssl.get()) == 1;
    result.alerts = *raw.alerts;
    if (result.state == State::established) {
      EXPECT_TRUE(ours->send(pattern(12, 59)));
      for (const auto& datagram : drain(*ours)) write_raw(raw, datagram);
      std::array<unsigned char, 64> buffer{};
      EXPECT_EQ(SSL_read(raw.ssl.get(), buffer.data(),
                         static_cast<int>(buffer.size())),
                12);
    } else {
      expect_failed(*ours);
    }
    return result;
  }
};

class RawPeer : public RawPeerRig {};

TEST_F(RawPeer, CompletesTheProfileAsAClient) {
  const auto control = run_raw(Options{});
  EXPECT_EQ(control.state, State::established);
  EXPECT_TRUE(control.raw_finished);
}

TEST_F(RawPeer, CompletesTheProfileAsAServer) {
  Options server_options;
  server_options.client = false;
  const auto control = run_raw(server_options);
  EXPECT_EQ(control.state, State::established);
  EXPECT_TRUE(control.raw_finished);
}

class Rejection {
 public:
  std::string name;
  RawPeerRig::Options options;
  int received{};
  int sent{};
};

class RawPeerOutsideTheProfile
    : public RawPeerRig,
      public ::testing::WithParamInterface<Rejection> {
 public:
  static auto rejections() -> std::vector<Rejection> {
    std::vector<Rejection> all;
    const auto add = [&all](std::string name, int received, int sent,
                            auto change) {
      Options options;
      change(options);
      all.push_back(
          Rejection{std::move(name), std::move(options), received, sent});
    };
    add("ClientWithoutCertificate", alert_handshake_failure, no_alert,
        [](Options& options) { options.certificate = false; });
    add("ClientWithoutAlpn", alert_close_notify, no_alert,
        [](Options& options) { options.offer_alpn = false; });
    add("ServerIgnoringAlpn", alert_close_notify, no_alert,
        [](Options& options) {
          options.client = false;
          options.select_alpn = false;
        });
    add("ClientWithoutExtendedMasterSecret", alert_close_notify, no_alert,
        [](Options& options) { options.extended_master_secret = false; });
    add("ServerWithoutExtendedMasterSecret", alert_close_notify, no_alert,
        [](Options& options) {
          options.client = false;
          options.extended_master_secret = false;
        });
    add("ClientOfferingAes256Only", alert_handshake_failure, no_alert,
        [](Options& options) {
          options.cipher = "ECDHE-ECDSA-AES256-GCM-SHA384";
        });
    add("ServerOfferingAes256Only", no_alert, alert_handshake_failure,
        [](Options& options) {
          options.client = false;
          options.cipher = "ECDHE-ECDSA-AES256-GCM-SHA384";
        });
    add("ClientSpeakingDtls10", alert_protocol_version, no_alert,
        [](Options& options) {
          options.max_version = DTLS1_VERSION;
          options.cipher = "ECDHE-ECDSA-AES128-SHA";
        });
    add("ClientOfferingX25519Only", alert_handshake_failure, no_alert,
        [](Options& options) { options.groups = "X25519"; });
    add("ClientWithAP384Certificate", alert_bad_certificate, no_alert,
        [](Options& options) { options.curve = "P-384"; });
    add("ServerHoldingAP384Certificate", no_alert, alert_handshake_failure,
        [](Options& options) {
          options.client = false;
          options.curve = "P-384";
        });
    add("ClientWithAnOversizedCertificate", alert_bad_certificate, no_alert,
        [](Options& options) {
          options.comment_size = kakusan::max_certificate_size;
        });
    add("ClientSendingACertificateChain", alert_bad_certificate, no_alert,
        [](Options& options) { options.chain = true; });
    return all;
  }

  static auto name_of(const ::testing::TestParamInfo<Rejection>& info)
      -> std::string {
    return info.param.name;
  }
};

TEST_P(RawPeerOutsideTheProfile, IsRejectedWithTheExpectedAlerts) {
  const auto& rejection = GetParam();
  const auto result = run_raw(rejection.options);
  RecordProperty("alert_received_by_raw_peer",
                 std::to_string(result.alerts.received));
  RecordProperty("alert_sent_by_raw_peer", std::to_string(result.alerts.sent));
  EXPECT_EQ(result.state, State::failed);
  EXPECT_EQ(result.error, Error::protocol);
  EXPECT_EQ(result.alerts.received, rejection.received);
  EXPECT_EQ(result.alerts.sent, rejection.sent);
}

INSTANTIATE_TEST_SUITE_P(
    Profile, RawPeerOutsideTheProfile,
    ::testing::ValuesIn(RawPeerOutsideTheProfile::rejections()),
    RawPeerOutsideTheProfile::name_of);

}  // namespace
}  // namespace net_kakusan_test
