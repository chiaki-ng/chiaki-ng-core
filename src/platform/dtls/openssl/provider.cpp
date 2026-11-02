#include "src/core/utils/memory/c_handle.hpp"
#include "src/core/utils/timing/saturating.hpp"
#include "src/platform/dtls/openssl/staging_lifecycle.hpp"

#include <libchiaking/platform/dtls/openssl/provider.hpp>

#include <libchiaking/detail/secret_array.hpp>
#include <libchiaking/platform/byte_units.hpp>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/objects.h>
#include <openssl/opensslv.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::platform::openssl {

class OpenSslSecureDatagramSession final : public SecureDatagramSession {
 public:
  [[nodiscard]] static auto create(const SecureDatagramProfile& profile)
      -> Expected<std::unique_ptr<SecureDatagramSession>, SecureDatagramError> {
    if (profile.alpn.empty() ||
        profile.alpn.size() > max_secure_datagram_alpn_size ||
        profile.record_limit == 0 || profile.mtu == 0 ||
        profile.mtu >
            static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        profile.max_certificate_size == 0 ||
        profile.handshake_deadline <= Instant::zero() ||
        profile.retransmit_first <= Instant::zero() ||
        profile.retransmit_max < profile.retransmit_first ||
        profile.retransmit_max.count() >
            std::numeric_limits<unsigned int>::max())
      return unexpected(SecureDatagramError::invalid_profile);
    const ErrorScope scope;
    try {
      auto session = std::unique_ptr<OpenSslSecureDatagramSession>{
          new OpenSslSecureDatagramSession(profile)};
      if (auto initialized = session->initialize(); !initialized)
        return unexpected(initialized.error());
      return std::unique_ptr<SecureDatagramSession>{std::move(session)};
    } catch (...) {
      return unexpected(SecureDatagramError::internal);
    }
  }

  auto certificate_digest() const noexcept -> const Digest& override {
    return digest_;
  }

  [[nodiscard]] auto start(const Digest& pinned_peer_digest, Instant now)
      -> Expected<void, SecureDatagramError> override {
    if (state_ == SecureDatagramState::failed) return unexpected(failure_);
    if (state_ != SecureDatagramState::idle)
      return unexpected(SecureDatagramError::invalid_state);
    const ErrorScope scope;
    peer_digest_ = pinned_peer_digest;
    handshake_deadline_ =
        timing::saturating_add(now, profile_.handshake_deadline);
    state_ = SecureDatagramState::handshaking;
    return finish(drive(), now);
  }

  [[nodiscard]] auto receive(std::span<const std::byte> datagram, Instant now)
      -> Expected<void, SecureDatagramError> override {
    if (state_ == SecureDatagramState::failed) return unexpected(failure_);
    if (!running()) return {};
    const ErrorScope scope;
    if (auto expired = expire(now); !expired) return expired;
    if (datagram.empty() || datagram.size() > profile_.mtu) return {};
    if (BIO_write(network_.get(), datagram.data(),
                  static_cast<int>(datagram.size())) !=
        static_cast<int>(datagram.size()))
      return {};
    return finish(drive(), now);
  }

  [[nodiscard]] auto poll(Instant now)
      -> Expected<void, SecureDatagramError> override {
    if (state_ == SecureDatagramState::failed) return unexpected(failure_);
    if (!running()) return {};
    const ErrorScope scope;
    if (auto expired = expire(now); !expired) return expired;
    if (retransmit_deadline_ && now >= *retransmit_deadline_) retransmit(now);
    return finish(drive(), now);
  }

  auto take_datagram() -> std::optional<std::vector<std::byte>> override {
    if (outbound_.empty()) return std::nullopt;
    auto datagram = std::move(outbound_.front());
    outbound_.pop_front();
    return datagram;
  }

  auto take_record() -> std::optional<SecretBytes> override {
    if (inbound_.empty()) return std::nullopt;
    auto record = std::move(inbound_.front());
    inbound_.pop_front();
    return record;
  }

  [[nodiscard]] auto send(std::span<const std::byte> record)
      -> Expected<void, SecureDatagramError> override {
    if (state_ == SecureDatagramState::failed) return unexpected(failure_);
    if (state_ != SecureDatagramState::established)
      return unexpected(SecureDatagramError::invalid_state);
    if (record.empty() || record.size() > max_record_size_)
      return unexpected(SecureDatagramError::invalid_argument);
    const ErrorScope scope;
    if (sent_records_ >= profile_.record_limit)
      return fail_with(SecureDatagramError::record_limit);
    const int written =
        SSL_write(ssl_.get(), record.data(), static_cast<int>(record.size()));
    if (written != static_cast<int>(record.size())) {
      const int error = SSL_get_error(ssl_.get(), written);
      return fail_with(error == SSL_ERROR_SSL
                           ? SecureDatagramError::peer_violation
                           : SecureDatagramError::internal);
    }
    ++sent_records_;
    return collect();
  }

  [[nodiscard]] auto export_secret(std::string_view label,
                                   std::span<const std::byte> context,
                                   std::size_t size)
      -> Expected<SecretBytes, SecureDatagramError> override {
    if (state_ == SecureDatagramState::failed) return unexpected(failure_);
    if (state_ != SecureDatagramState::established)
      return unexpected(SecureDatagramError::invalid_state);
    if (size == 0) return unexpected(SecureDatagramError::invalid_argument);
    const ErrorScope scope;
    SecretBytes secret;
    if (!secret.try_resize(size))
      return unexpected(SecureDatagramError::internal);
    static constexpr unsigned char empty_context = 0;
    const auto* context_bytes =
        context.empty()
            ? &empty_context
            : reinterpret_cast<const unsigned char*>(context.data());
    if (SSL_export_keying_material(
            ssl_.get(), reinterpret_cast<unsigned char*>(secret.span().data()),
            size, label.data(), label.size(), context_bytes, context.size(),
            1) != 1)
      return unexpected(SecureDatagramError::internal);
    return secret;
  }

  auto state() const noexcept -> SecureDatagramState override { return state_; }

  auto max_record_size() const noexcept -> std::size_t override {
    return state_ == SecureDatagramState::established ? max_record_size_ : 0;
  }

  auto next_deadline() const noexcept -> std::optional<Instant> override {
    if (state_ == SecureDatagramState::handshaking) {
      if (retransmit_deadline_)
        return std::min(*retransmit_deadline_, handshake_deadline_);
      return handshake_deadline_;
    }
    if (state_ == SecureDatagramState::established) return retransmit_deadline_;
    return std::nullopt;
  }

  void close() noexcept override {
    if (ssl_ && state_ == SecureDatagramState::established) {
      const ErrorScope scope;
      static_cast<void>(SSL_shutdown(ssl_.get()));
      static_cast<void>(drain());
    }
    if (state_ != SecureDatagramState::failed)
      state_ = SecureDatagramState::closed;
    inbound_.clear();
    release();
  }

 private:
  static constexpr long min_openssl_version = 0x30200000L;
  static_assert(
      OPENSSL_VERSION_NUMBER >= min_openssl_version,
      "the OpenSSL secure datagram provider needs OpenSSL 3.2 or newer for "
      "datagram BIO pairs");

  static constexpr std::uint64_t retransmit_backoff_factor = 2;
  static constexpr std::size_t max_retransmit_copies = 10;
  static constexpr unsigned int library_timer_elapsed_us = 1;
  static constexpr unsigned int library_timer_parked_us =
      std::numeric_limits<unsigned int>::max();

  using Flight = std::vector<std::vector<std::byte>>;
  static constexpr std::uint16_t expected_cipher_suite = 0xc02b;
  static constexpr const char* cipher_list = "ECDHE-ECDSA-AES128-GCM-SHA256";
  static constexpr const char* group_list = "P-256";
  static constexpr const char* signature_list = "ECDSA+SHA256";
  static constexpr const char* certificate_name = "rcp";
  static constexpr std::size_t bio_buffer_size = 256 * bytes_per_kib;
  static constexpr std::size_t read_buffer_size = SSL3_RT_MAX_PLAIN_LENGTH;
  static constexpr int serial_bits = 127;
  static constexpr long certificate_backdate_s = 24L * 60L * 60L;
  static constexpr long certificate_lifetime_s = 30L * 24L * 60L * 60L;

  class ErrorScope {
   public:
    ErrorScope() noexcept { ERR_clear_error(); }
    ~ErrorScope() { ERR_clear_error(); }
    ErrorScope(const ErrorScope&) = delete;
    auto operator=(const ErrorScope&) -> ErrorScope& = delete;
  };

  using Context = memory::CHandle<SSL_CTX, SSL_CTX_free>;
  using Ssl = memory::CHandle<SSL, SSL_free>;
  using Bio = memory::CHandle<BIO, BIO_free>;
  using Pkey = memory::CHandle<EVP_PKEY, EVP_PKEY_free>;
  using Certificate = memory::CHandle<X509, X509_free>;
  using Bn = memory::CHandle<BIGNUM, BN_clear_free>;
  using Name = memory::CHandle<X509_NAME, X509_NAME_free>;

  explicit OpenSslSecureDatagramSession(const SecureDatagramProfile& profile)
      : profile_(profile), retransmit_copies_(retransmit_copies(profile)) {}

  [[nodiscard]] auto digest_certificate(const X509* certificate) const
      -> std::optional<Digest> {
    const int size = i2d_X509(certificate, nullptr);
    if (size <= 0 ||
        static_cast<std::size_t>(size) > profile_.max_certificate_size)
      return std::nullopt;
    std::vector<unsigned char> der(static_cast<std::size_t>(size));
    unsigned char* cursor = der.data();
    if (i2d_X509(certificate, &cursor) != size) return std::nullopt;
    Digest digest{};
    unsigned int written = 0;
    if (EVP_Digest(der.data(), der.size(),
                   reinterpret_cast<unsigned char*>(digest.data()), &written,
                   EVP_sha256(), nullptr) != 1 ||
        written != digest.size())
      return std::nullopt;
    return digest;
  }

  static auto is_p256(const EVP_PKEY* key) noexcept -> bool {
    if (key == nullptr || EVP_PKEY_is_a(key, "EC") != 1) return false;
    std::array<char, 64> group{};
    std::size_t length = 0;
    if (EVP_PKEY_get_group_name(key, group.data(), group.size(), &length) !=
            1 ||
        length >= group.size())
      return false;
    return OBJ_sn2nid(group.data()) == NID_X9_62_prime256v1 ||
           EC_curve_nist2nid(group.data()) == NID_X9_62_prime256v1;
  }

  [[nodiscard]] auto adopt_identity(SSL_CTX* context)
      -> Expected<void, SecureDatagramError> {
    const Pkey key{EVP_EC_gen("P-256")};
    const Certificate owned{X509_new()};
    const Bn serial{BN_new()};
    if (!key || !owned || !serial)
      return unexpected(SecureDatagramError::unsupported);
    X509* certificate = owned.get();
    if (X509_set_version(certificate, X509_VERSION_3) != 1 ||
        BN_rand(serial.get(), serial_bits, BN_RAND_TOP_ANY,
                BN_RAND_BOTTOM_ANY) != 1 ||
        BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(certificate)) ==
            nullptr ||
        X509_gmtime_adj(X509_getm_notBefore(certificate),
                        -certificate_backdate_s) == nullptr ||
        X509_gmtime_adj(X509_getm_notAfter(certificate),
                        certificate_lifetime_s) == nullptr)
      return unexpected(SecureDatagramError::internal);
    const Name name{X509_NAME_new()};
    if (!name ||
        X509_NAME_add_entry_by_txt(
            name.get(), "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>(certificate_name), -1, -1,
            0) != 1 ||
        X509_set_subject_name(certificate, name.get()) != 1 ||
        X509_set_issuer_name(certificate, name.get()) != 1 ||
        X509_set_pubkey(certificate, key.get()) != 1 ||
        X509_sign(certificate, key.get(), EVP_sha256()) <= 0)
      return unexpected(SecureDatagramError::internal);
    const auto digest = digest_certificate(certificate);
    if (!digest) return unexpected(SecureDatagramError::internal);
    digest_ = *digest;
    if (SSL_CTX_use_certificate(context, certificate) != 1 ||
        SSL_CTX_use_PrivateKey(context, key.get()) != 1 ||
        SSL_CTX_check_private_key(context) != 1)
      return unexpected(SecureDatagramError::internal);
    return {};
  }

  [[nodiscard]] auto initialize() -> Expected<void, SecureDatagramError> {
    const bool client = profile_.role == SecureDatagramRole::client;
    context_.reset(
        SSL_CTX_new(client ? DTLS_client_method() : DTLS_server_method()));
    if (!context_) return unexpected(SecureDatagramError::internal);
    SSL_CTX* context = context_.get();
    if (SSL_CTX_set_min_proto_version(context, DTLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(context, DTLS1_2_VERSION) != 1 ||
        SSL_CTX_set_cipher_list(context, cipher_list) != 1 ||
        SSL_CTX_set1_groups_list(context, group_list) != 1 ||
        SSL_CTX_set1_sigalgs_list(context, signature_list) != 1 ||
        SSL_CTX_set1_client_sigalgs_list(context, signature_list) != 1)
      return unexpected(SecureDatagramError::unsupported);
    SSL_CTX_set_options(context, SSL_OP_NO_TICKET | SSL_OP_NO_RENEGOTIATION |
                                     SSL_OP_NO_QUERY_MTU |
                                     SSL_OP_NO_COMPRESSION);
    SSL_CTX_clear_options(
        context, SSL_OP_COOKIE_EXCHANGE | SSL_OP_NO_EXTENDED_MASTER_SECRET |
                     SSL_OP_LEGACY_SERVER_CONNECT |
                     SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION |
                     SSL_OP_ALLOW_CLIENT_RENEGOTIATION);
    SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_verify(context,
                       client
                           ? SSL_VERIFY_PEER
                           : SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                       nullptr);
    SSL_CTX_set_cert_verify_callback(
        context, &OpenSslSecureDatagramSession::verify_peer, this);
    if (!client)
      SSL_CTX_set_alpn_select_cb(
          context, &OpenSslSecureDatagramSession::select_protocol, this);
    if (auto adopted = adopt_identity(context); !adopted) return adopted;

    ssl_.reset(SSL_new(context));
    if (!ssl_) return unexpected(SecureDatagramError::internal);
    SSL* ssl = ssl_.get();
    if (SSL_set_app_data(ssl, this) != 1)
      return unexpected(SecureDatagramError::internal);
    if (client) {
      std::string protocols;
      protocols.push_back(static_cast<char>(profile_.alpn.size()));
      protocols.append(profile_.alpn);
      const auto* encoded =
          reinterpret_cast<const unsigned char*>(protocols.data());
      if (SSL_set_alpn_protos(ssl, encoded,
                              static_cast<unsigned int>(protocols.size())) != 0)
        return unexpected(SecureDatagramError::internal);
    }
    BIO* inner_raw = nullptr;
    BIO* outer_raw = nullptr;
    const int paired = BIO_new_bio_dgram_pair(&inner_raw, bio_buffer_size,
                                              &outer_raw, bio_buffer_size);
    Bio inner{inner_raw};
    network_.reset(outer_raw);
    if (paired != 1) return unexpected(SecureDatagramError::internal);
    auto* adopted = inner.release();
    SSL_set_bio(ssl, adopted, adopted);
    if (SSL_set_mtu(ssl, static_cast<long>(profile_.mtu)) <= 0)
      return unexpected(SecureDatagramError::internal);
    DTLS_set_timer_cb(ssl, &OpenSslSecureDatagramSession::retransmit_timer);
    if (client)
      SSL_set_connect_state(ssl);
    else
      SSL_set_accept_state(ssl);
    return {};
  }

  static auto retransmit_timer(SSL* ssl, unsigned int previous_us)
      -> unsigned int {
    auto* self =
        static_cast<OpenSslSecureDatagramSession*>(SSL_get_app_data(ssl));
    return self->stage_retransmission(previous_us == 0);
  }

  auto stage_retransmission(bool new_flight) noexcept -> unsigned int {
    try {
      if (new_flight) {
        staged_.clear();
        staging_.begin_flight();
        return library_timer_elapsed_us;
      }
      if (const auto drained = drain(); !drained)
        staging_failure_ = drained.error();
      staged_.emplace_back();
      staging_.stage();
    } catch (...) {
      staging_failure_ = SecureDatagramError::internal;
    }
    return staged_.size() < retransmit_copies_ ? library_timer_elapsed_us
                                               : library_timer_parked_us;
  }

  static auto retransmit_copies(const SecureDatagramProfile& profile) noexcept
      -> std::size_t {
    std::size_t copies = 0;
    auto interval = profile.retransmit_first;
    auto due = interval;
    while (copies < max_retransmit_copies && due < profile.handshake_deadline) {
      ++copies;
      interval = backed_off(interval, profile.retransmit_max);
      due = timing::saturating_add(due, interval);
    }
    return std::max<std::size_t>(copies, 1);
  }

  static auto backed_off(Instant interval, Instant longest) noexcept
      -> Instant {
    const auto doubled =
        interval >
                longest / static_cast<Instant::rep>(retransmit_backoff_factor)
            ? longest
            : interval * static_cast<Instant::rep>(retransmit_backoff_factor);
    return std::min(doubled, longest);
  }

  void retransmit(Instant now) {
    if (staged_.empty()) {
      retransmit_deadline_.reset();
      return;
    }
    for (auto& datagram : staged_.front())
      outbound_.push_back(std::move(datagram));
    staged_.pop_front();
    retransmit_interval_ =
        backed_off(retransmit_interval_, profile_.retransmit_max);
    retransmit_deadline_ = timing::saturating_add(now, retransmit_interval_);
  }

  [[nodiscard]] static auto verify_peer(X509_STORE_CTX* store, void* argument)
      -> int {
    auto* self = static_cast<OpenSslSecureDatagramSession*>(argument);
    if (self->verify(store)) {
      X509_STORE_CTX_set_error(store, X509_V_OK);
      return 1;
    }
    X509_STORE_CTX_set_error(store, X509_V_ERR_CERT_REJECTED);
    return 0;
  }

  [[nodiscard]] static auto select_protocol(SSL*,
                                            const unsigned char** selected,
                                            unsigned char* selected_length,
                                            const unsigned char* offered,
                                            unsigned int offered_length,
                                            void* argument) -> int {
    const auto* self =
        static_cast<const OpenSslSecureDatagramSession*>(argument);
    const std::string_view wanted = self->profile_.alpn;
    unsigned int offset = 0;
    while (offset < offered_length) {
      const unsigned int length = offered[offset];
      ++offset;
      if (length == 0 || length > offered_length - offset)
        return SSL_TLSEXT_ERR_ALERT_FATAL;
      if (length == wanted.size() &&
          std::memcmp(offered + offset, wanted.data(), length) == 0) {
        *selected = offered + offset;
        *selected_length = static_cast<unsigned char>(length);
        return SSL_TLSEXT_ERR_OK;
      }
      offset += length;
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
  }

  auto pinned(const X509* certificate) const -> bool {
    const auto digest = digest_certificate(certificate);
    return digest && CRYPTO_memcmp(digest->data(), peer_digest_.data(),
                                   peer_digest_.size()) == 0;
  }

  [[nodiscard]] auto verify(X509_STORE_CTX* store) -> bool {
    if (state_ != SecureDatagramState::handshaking) return false;
    X509* leaf = X509_STORE_CTX_get0_cert(store);
    STACK_OF(X509)* chain = X509_STORE_CTX_get0_untrusted(store);
    if (leaf == nullptr || chain == nullptr || sk_X509_num(chain) != 1 ||
        X509_cmp(sk_X509_value(chain, 0), leaf) != 0 ||
        !is_p256(X509_get0_pubkey(leaf)))
      return false;
    if (!pinned(leaf)) {
      pin_rejected_ = true;
      return false;
    }
    return true;
  }

  [[nodiscard]] auto verify_established() -> bool {
    SSL* ssl = ssl_.get();
    const SSL_CIPHER* cipher = SSL_get_current_cipher(ssl);
    if (SSL_version(ssl) != DTLS1_2_VERSION || cipher == nullptr ||
        SSL_CIPHER_get_protocol_id(cipher) != expected_cipher_suite ||
        SSL_get_extms_support(ssl) != 1 ||
        SSL_get_negotiated_group(ssl) != NID_X9_62_prime256v1)
      return false;
    int signature_type = NID_undef;
    int signature_digest = NID_undef;
    if (SSL_get_peer_signature_type_nid(ssl, &signature_type) != 1 ||
        signature_type != EVP_PKEY_EC ||
        SSL_get_peer_signature_nid(ssl, &signature_digest) != 1 ||
        signature_digest != NID_sha256)
      return false;
    const unsigned char* protocol = nullptr;
    unsigned int protocol_length = 0;
    SSL_get0_alpn_selected(ssl, &protocol, &protocol_length);
    if (protocol == nullptr || protocol_length != profile_.alpn.size() ||
        std::memcmp(protocol, profile_.alpn.data(), protocol_length) != 0)
      return false;
    const X509* peer = SSL_get0_peer_certificate(ssl);
    if (peer == nullptr || SSL_get_verify_result(ssl) != X509_V_OK ||
        !pinned(peer))
      return false;
    const std::size_t data_mtu = DTLS_get_data_mtu(ssl);
    if (data_mtu == 0 || data_mtu >= profile_.mtu) return false;
    max_record_size_ = data_mtu;
    return true;
  }

  [[nodiscard]] auto drive() -> Expected<void, SecureDatagramError> {
    if (state_ == SecureDatagramState::handshaking) {
      const int result = SSL_do_handshake(ssl_.get());
      if (result != 1) {
        const int error = SSL_get_error(ssl_.get(), result);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
          return collect();
        if (error == SSL_ERROR_SYSCALL)
          return fail_with(SecureDatagramError::internal);
        return fail_with(pin_rejected_ ? SecureDatagramError::peer_not_pinned
                                       : SecureDatagramError::handshake_failed);
      }
      if (!verify_established()) {
        static_cast<void>(SSL_shutdown(ssl_.get()));
        return fail_with(SecureDatagramError::handshake_failed);
      }
      state_ = SecureDatagramState::established;
    }
    if (state_ == SecureDatagramState::established) return read_records();
    return collect();
  }

  [[nodiscard]] auto read_records() -> Expected<void, SecureDatagramError> {
    while (true) {
      SecretArray<read_buffer_size> read_buffer;
      const int read = SSL_read(ssl_.get(), read_buffer.data(),
                                static_cast<int>(read_buffer.size()));
      if (read > 0) {
        const auto size = static_cast<std::size_t>(read);
        if (received_records_ >= profile_.record_limit)
          return fail_with(SecureDatagramError::record_limit);
        ++received_records_;
        auto record = SecretBytes::from_bytes(
            std::span<const std::byte>{read_buffer.data(), size});
        if (!record) return fail_with(SecureDatagramError::internal);
        try {
          inbound_.emplace_back(std::move(record).value());
        } catch (...) {
          return fail_with(SecureDatagramError::internal);
        }
        continue;
      }
      const int error = SSL_get_error(ssl_.get(), read);
      if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
        return collect();
      if (error == SSL_ERROR_ZERO_RETURN) {
        auto collected = collect();
        state_ = SecureDatagramState::closed;
        release();
        return collected;
      }
      return fail_with(error == SSL_ERROR_SSL
                           ? SecureDatagramError::peer_violation
                           : SecureDatagramError::internal);
    }
  }

  [[nodiscard]] auto drain() -> Expected<void, SecureDatagramError> {
    if (!network_) return {};
    while (true) {
      const std::size_t pending = BIO_ctrl_pending(network_.get());
      if (pending == 0) return {};
      if (pending > profile_.mtu)
        return unexpected(SecureDatagramError::internal);
      std::vector<std::byte> datagram(pending);
      const int read =
          BIO_read(network_.get(), datagram.data(), static_cast<int>(pending));
      if (read <= 0) return unexpected(SecureDatagramError::internal);
      datagram.resize(static_cast<std::size_t>(read));
      if (staging_.staging())
        staged_.back().push_back(std::move(datagram));
      else
        outbound_.push_back(std::move(datagram));
    }
  }

  [[nodiscard]] auto collect() -> Expected<void, SecureDatagramError> {
    if (const auto drained = drain(); !drained)
      return fail_with(drained.error());
    return {};
  }

  [[nodiscard]] auto expire(Instant now)
      -> Expected<void, SecureDatagramError> {
    if (state_ == SecureDatagramState::handshaking &&
        now >= handshake_deadline_)
      return fail_with(SecureDatagramError::handshake_timeout);
    return {};
  }

  [[nodiscard]] auto finish(Expected<void, SecureDatagramError> result,
                            Instant now)
      -> Expected<void, SecureDatagramError> {
    staging_.unstage();
    const bool started = staging_.take_flight();
    if (const auto staged = std::exchange(staging_failure_, std::nullopt);
        staged && state_ != SecureDatagramState::failed)
      return fail_with(*staged);
    if (!ssl_ || !running() || !flight_pending()) {
      staged_.clear();
      retransmit_deadline_.reset();
      return result;
    }
    if (started) {
      retransmit_interval_ = profile_.retransmit_first;
      retransmit_deadline_ = timing::saturating_add(now, retransmit_interval_);
    }
    return result;
  }

  auto flight_pending() const noexcept -> bool {
    timeval unused{};
    return DTLSv1_get_timeout(ssl_.get(), &unused) == 1;
  }

  auto running() const noexcept -> bool {
    return state_ == SecureDatagramState::handshaking ||
           state_ == SecureDatagramState::established;
  }

  auto fail_with(SecureDatagramError error) -> Unexpected<SecureDatagramError> {
    if (state_ != SecureDatagramState::failed) static_cast<void>(drain());
    state_ = SecureDatagramState::failed;
    failure_ = error;
    inbound_.clear();
    release();
    return unexpected(error);
  }

  void release() noexcept {
    ssl_.reset();
    network_.reset();
    context_.reset();
    staged_.clear();
    staging_.unstage();
    retransmit_deadline_.reset();
    max_record_size_ = 0;
  }

  SecureDatagramProfile profile_;
  Context context_;
  Bio network_;
  Ssl ssl_;
  Digest digest_{};
  Digest peer_digest_{};
  SecureDatagramState state_{SecureDatagramState::idle};
  SecureDatagramError failure_{SecureDatagramError::internal};
  std::deque<std::vector<std::byte>> outbound_;
  std::deque<SecretBytes> inbound_;
  std::deque<Flight> staged_;
  std::size_t retransmit_copies_{1};
  std::optional<Instant> retransmit_deadline_;
  Instant retransmit_interval_{};
  Instant handshake_deadline_{};
  StagingLifecycle staging_;
  std::optional<SecureDatagramError> staging_failure_;
  std::uint64_t sent_records_{0};
  std::uint64_t received_records_{0};
  std::size_t max_record_size_{0};
  bool pin_rejected_{false};
};

auto OpenSslSecureDatagramProvider::open(const SecureDatagramProfile& profile)
    -> Expected<std::unique_ptr<SecureDatagramSession>, SecureDatagramError> {
  return OpenSslSecureDatagramSession::create(profile);
}

}  // namespace libchiaking::platform::openssl
