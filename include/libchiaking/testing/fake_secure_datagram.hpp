/// @file
/// @brief Declares FakeSecureDatagramProvider, a secure datagram layer with no
/// real cryptography.
#pragma once

#include "libchiaking/platform/dtls/provider.hpp"
#include "libchiaking/testing/split_mix.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::testing {

/// @brief Stands in for platform::SecureDatagramProvider without a DTLS
/// library.
///
/// Sessions it opens follow the platform::SecureDatagramSession contract: a
/// client and a server shake hands by exchanging datagrams, each checks the
/// other's certificate digest against the pinned one and that the ALPN matches,
/// lost handshake flights are retransmitted with a doubling interval, the
/// handshake deadline and the record limit are enforced, and a repeated or
/// altered record is dropped. Time comes only from the instants the caller
/// passes in, so a test controls every timeout.
///
/// A test gives the provider to both ends, carries the datagrams each session
/// produces to the other (for instance over an InMemoryNetwork), and uses
/// lose_datagrams() to drop traffic and opened() to count sessions.
///
/// @warning Nothing here is secure. Digests, keys and exported secrets come
///     from a deterministic mixing function and records are only masked. Fake
///     sessions interoperate with each other and with nothing else.
///
/// @note Digests are deterministic: a session's digest depends only on the
///     provider's seed and on how many sessions the provider had opened before
///     it. The provider and its sessions do no locking.
class FakeSecureDatagramProvider final
    : public platform::SecureDatagramProvider {
 public:
  /// @brief Passed to lose_datagrams() to drop outbound datagrams
  /// indefinitely.
  static constexpr std::size_t every_datagram =
      std::numeric_limits<std::size_t>::max();

  /// @brief Creates a provider with a seed no other provider in the process
  /// has.
  ///
  /// Seeds are taken from a process-wide counter, so sessions from separately
  /// constructed providers get different digests.
  FakeSecureDatagramProvider() : FakeSecureDatagramProvider(++instances_) {}

  /// @brief Creates a provider whose session digests are fixed by a seed.
  ///
  /// @param seed Chooses the digests. Two providers built with the same seed
  ///     hand out the same sequence of digests.
  explicit FakeSecureDatagramProvider(std::uint64_t seed)
      : shared_(std::make_shared<Shared>(seed)) {}

  /// @brief Drops the next datagrams the provider's sessions try to send.
  ///
  /// The count is shared by every session this provider has opened and covers
  /// handshake flights, records and alerts alike. A dropped datagram is
  /// discarded before it is queued, so the session believes it was sent.
  ///
  /// @param count How many datagrams to drop. every_datagram drops all of
  ///     them until this is called again; zero stops dropping.
  void lose_datagrams(std::size_t count) noexcept { shared_->lost = count; }

  /// @brief Reports how many sessions open() has created.
  ///
  /// @return The number of sessions opened since the provider was created.
  auto opened() const noexcept -> std::size_t { return shared_->opened; }

  /// @brief Opens an idle fake session for one end of a connection.
  ///
  /// @param profile The role, ALPN, MTU, timings and record limit to use. The
  ///     session keeps a copy.
  /// @return A new session in the idle state with its own certificate digest.
  /// @retval platform::SecureDatagramError::invalid_profile The ALPN is empty
  ///     or longer than platform::max_secure_datagram_alpn_size, the record
  ///     limit is zero, the MTU is not larger than the 37 byte record
  ///     overhead, the handshake deadline or first retransmit interval is not
  ///     positive, or the longest retransmit interval is shorter than the
  ///     first.
  [[nodiscard]] auto open(const platform::SecureDatagramProfile& profile)
      -> Expected<std::unique_ptr<platform::SecureDatagramSession>,
                  platform::SecureDatagramError> override {
    if (profile.alpn.empty() ||
        profile.alpn.size() > platform::max_secure_datagram_alpn_size ||
        profile.record_limit == 0 || profile.mtu <= Session::record_overhead ||
        profile.handshake_deadline <= Session::Instant::zero() ||
        profile.retransmit_first <= Session::Instant::zero() ||
        profile.retransmit_max < profile.retransmit_first)
      return unexpected(platform::SecureDatagramError::invalid_profile);
    ++shared_->opened;
    return std::unique_ptr<platform::SecureDatagramSession>{
        std::make_unique<Session>(profile, shared_)};
  }

 private:
  /// @brief State the provider and every session it opened share.
  struct Shared {
    /// @brief Starts the shared state.
    ///
    /// @param chosen The provider's seed.
    explicit Shared(std::uint64_t chosen) noexcept : seed(chosen) {}

    /// @brief Chooses every digest the provider's sessions get.
    std::uint64_t seed;
    /// @brief How many sessions have been opened.
    std::size_t opened{};
    /// @brief How many more outbound datagrams to drop, or every_datagram.
    std::size_t lost{};
  };

  /// @brief Deterministic stand-in for a hash and a key derivation.
  ///
  /// It folds inputs into its state with FNV-1a steps and draws words with
  /// the SplitMix64 step, so equal inputs always give equal output. Nothing
  /// about it is secure.
  class Mixer {
   public:
    /// @brief Starts from a seed.
    ///
    /// @param seed The starting value; equal seeds give equal output.
    explicit Mixer(std::uint64_t seed) noexcept : state_(seed ^ offset) {}

    /// @brief Folds in a byte string, length first.
    ///
    /// @param bytes The bytes to fold in.
    /// @return This mixer, for chaining.
    auto absorb(std::span<const std::byte> bytes) noexcept -> Mixer& {
      absorb(static_cast<std::uint64_t>(bytes.size()));
      for (const auto byte : bytes) step(std::to_integer<std::uint64_t>(byte));
      return *this;
    }

    /// @brief Folds in text as its bytes, length first.
    ///
    /// @param text The text to fold in.
    /// @return This mixer, for chaining.
    auto absorb(std::string_view text) noexcept -> Mixer& {
      return absorb(std::as_bytes(std::span{text.data(), text.size()}));
    }

    /// @brief Folds in a number, low byte first.
    ///
    /// @param value The number to fold in.
    /// @return This mixer, for chaining.
    auto absorb(std::uint64_t value) noexcept -> Mixer& {
      for (int shift = 0; shift < 64; shift += 8)
        step((value >> shift) & 0xffU);
      return *this;
    }

    /// @brief Draws the next word.
    ///
    /// @return A word that depends on everything folded in and drawn so
    ///     far.
    auto next() noexcept -> std::uint64_t {
      state_ += SplitMix64::golden_gamma;
      return SplitMix64::mix(state_);
    }

    /// @brief Overwrites a buffer with drawn words, low byte first.
    ///
    /// Each call starts on a fresh word; bytes left over from the last word
    /// are discarded.
    ///
    /// @param output The buffer to fill.
    void fill(std::span<std::byte> output) noexcept {
      std::uint64_t word = 0;
      for (std::size_t index = 0; index < output.size(); ++index) {
        if (index % 8 == 0) word = next();
        output[index] =
            static_cast<std::byte>((word >> (8 * (index % 8))) & 0xffU);
      }
    }

   private:
    static constexpr std::uint64_t offset = 0xcbf29ce484222325ULL;
    static constexpr std::uint64_t prime = 0x100000001b3ULL;

    void step(std::uint64_t value) noexcept {
      state_ = (state_ ^ value) * prime;
    }

    std::uint64_t state_;
  };

  /// @brief One end of a fake secure datagram connection.
  ///
  /// It follows the platform::SecureDatagramSession contract with masking in
  /// place of encryption. It does no locking: use it from one thread at a
  /// time. Nothing blocks; time comes only from the instants passed in.
  class Session final : public platform::SecureDatagramSession {
   public:
    /// @brief Bytes a record adds: a 3 byte frame header, an 8 byte
    /// sequence number, an 8 byte tag and 18 bytes of padding.
    static constexpr std::size_t record_overhead = 37;

    /// @brief Creates an idle session with its own digest.
    ///
    /// @param profile The role, ALPN, MTU, timings and record limit;
    ///     already checked by open().
    /// @param shared The provider's shared state. The digest is drawn from
    ///     its seed and its count of opened sessions.
    Session(const platform::SecureDatagramProfile& profile,
            std::shared_ptr<Shared> shared)
        : profile_(profile), shared_(std::move(shared)) {
      Mixer{shared_->seed}
          .absorb(static_cast<std::uint64_t>(shared_->opened))
          .fill(digest_);
    }

    /// @brief See platform::SecureDatagramSession::certificate_digest.
    ///
    /// @return This session's digest, fixed at construction.
    auto certificate_digest() const noexcept -> const Digest& override {
      return digest_;
    }

    /// @brief Starts the handshake. See
    /// platform::SecureDatagramSession::start.
    ///
    /// A client queues its hello at once; a server waits for the client's.
    ///
    /// @param pinned_peer_digest The digest the peer must present.
    /// @param now The current instant; the handshake deadline counts from
    ///     it.
    /// @return Nothing once handshaking.
    /// @retval platform::SecureDatagramError::invalid_state The session was
    ///     already started.
    ///
    /// Any other failure is the error that failed the session earlier.
    [[nodiscard]] auto start(const Digest& pinned_peer_digest, Instant now)
        -> Expected<void, platform::SecureDatagramError> override {
      if (state_ == platform::SecureDatagramState::failed)
        return unexpected(failure_);
      if (state_ != platform::SecureDatagramState::idle)
        return unexpected(platform::SecureDatagramError::invalid_state);
      pinned_ = pinned_peer_digest;
      handshake_deadline_ = now + profile_.handshake_deadline;
      state_ = platform::SecureDatagramState::handshaking;
      if (client()) begin_flight(hello(), now);
      return {};
    }

    /// @brief Handles one datagram from the peer. See
    /// platform::SecureDatagramSession::receive.
    ///
    /// Datagrams that are too long, malformed, repeated or altered are
    /// dropped without an error.
    ///
    /// @param datagram The datagram as received.
    /// @param now The current instant.
    /// @return Nothing unless the datagram failed the session.
    /// @retval platform::SecureDatagramError::handshake_timeout The
    ///     handshake deadline has passed.
    /// @retval platform::SecureDatagramError::peer_not_pinned The peer's
    ///     digest is not the pinned one.
    /// @retval platform::SecureDatagramError::handshake_failed The ALPN
    ///     does not match, or the peer sent a fatal alert.
    /// @retval platform::SecureDatagramError::record_limit More records
    ///     arrived than the profile allows.
    /// @retval platform::SecureDatagramError::internal A record could not
    ///     be stored.
    ///
    /// Any other failure is the error that failed the session earlier.
    [[nodiscard]] auto receive(std::span<const std::byte> datagram, Instant now)
        -> Expected<void, platform::SecureDatagramError> override {
      if (state_ == platform::SecureDatagramState::failed)
        return unexpected(failure_);
      if (!running()) return {};
      if (auto expired = expire(now); !expired) return expired;
      if (datagram.size() > profile_.mtu) return {};
      auto rest = datagram;
      while (rest.size() >= frame_header && running()) {
        const auto length = (std::to_integer<std::size_t>(rest[1]) << 8U) |
                            std::to_integer<std::size_t>(rest[2]);
        if (length > rest.size() - frame_header) return {};
        const auto type = std::to_integer<std::uint8_t>(rest[0]);
        if (auto handled =
                dispatch(type, rest.subspan(frame_header, length), now);
            !handled)
          return handled;
        rest = rest.subspan(frame_header + length);
      }
      return {};
    }

    /// @brief Retransmits a lost flight and enforces the handshake
    /// deadline. See platform::SecureDatagramSession::poll.
    ///
    /// @param now The current instant.
    /// @return Nothing unless the session failed.
    /// @retval platform::SecureDatagramError::handshake_timeout The
    ///     handshake deadline has passed.
    ///
    /// Any other failure is the error that failed the session earlier.
    [[nodiscard]] auto poll(Instant now)
        -> Expected<void, platform::SecureDatagramError> override {
      if (state_ == platform::SecureDatagramState::failed)
        return unexpected(failure_);
      if (!running()) return {};
      if (auto expired = expire(now); !expired) return expired;
      if (retransmit_deadline_ && now >= *retransmit_deadline_) {
        transmit(flight_);
        retransmit_interval_ =
            std::min(retransmit_interval_ * 2, profile_.retransmit_max);
        retransmit_deadline_ = now + retransmit_interval_;
      }
      return {};
    }

    /// @brief See platform::SecureDatagramSession::take_datagram.
    ///
    /// @return The oldest datagram waiting to be sent, or nothing when none
    ///     waits.
    auto take_datagram() -> std::optional<std::vector<std::byte>> override {
      if (outbound_.empty()) return std::nullopt;
      auto datagram = std::move(outbound_.front());
      outbound_.pop_front();
      return datagram;
    }

    /// @brief See platform::SecureDatagramSession::take_record.
    ///
    /// @return The oldest record received from the peer, or nothing when
    ///     none waits.
    auto take_record() -> std::optional<SecretBytes> override {
      if (inbound_.empty()) return std::nullopt;
      auto record = std::move(inbound_.front());
      inbound_.pop_front();
      return record;
    }

    /// @brief Seals and queues one record. See
    /// platform::SecureDatagramSession::send.
    ///
    /// @param record The plaintext, at most max_record_size() bytes.
    /// @return Nothing once the record is queued.
    /// @retval platform::SecureDatagramError::invalid_state The session is
    ///     not established.
    /// @retval platform::SecureDatagramError::invalid_argument The record
    ///     is empty or too long.
    /// @retval platform::SecureDatagramError::record_limit The profile's
    ///     record limit was reached; the session has failed.
    ///
    /// Any other failure is the error that failed the session earlier.
    [[nodiscard]] auto send(std::span<const std::byte> record)
        -> Expected<void, platform::SecureDatagramError> override {
      if (state_ == platform::SecureDatagramState::failed)
        return unexpected(failure_);
      if (state_ != platform::SecureDatagramState::established)
        return unexpected(platform::SecureDatagramError::invalid_state);
      if (record.empty() || record.size() > max_record_size())
        return unexpected(platform::SecureDatagramError::invalid_argument);
      if (sent_ >= profile_.record_limit)
        return fail_with(platform::SecureDatagramError::record_limit);
      const auto sequence = sent_++;
      auto datagram =
          frame(record_type, record_overhead - frame_header + record.size());
      append(datagram, sequence);
      append(datagram, seal(sequence, record, client()));
      datagram.resize(record_overhead);
      datagram.insert(datagram.end(), record.begin(), record.end());
      mask(sequence, std::span{datagram}.subspan(record_overhead), client());
      transmit(std::move(datagram));
      return {};
    }

    /// @brief Derives keying material. See
    /// platform::SecureDatagramSession::export_secret.
    ///
    /// Both ends derive the same bytes from the same label and context.
    ///
    /// @param label The exporter label.
    /// @param context The exporter context.
    /// @param size How many bytes to derive.
    /// @return The derived bytes.
    /// @retval platform::SecureDatagramError::invalid_state The session is
    ///     not established.
    /// @retval platform::SecureDatagramError::invalid_argument @p size is
    ///     zero.
    /// @retval platform::SecureDatagramError::internal The bytes could not
    ///     be allocated.
    ///
    /// Any other failure is the error that failed the session earlier.
    [[nodiscard]] auto export_secret(std::string_view label,
                                     std::span<const std::byte> context,
                                     std::size_t size)
        -> Expected<SecretBytes, platform::SecureDatagramError> override {
      if (state_ == platform::SecureDatagramState::failed)
        return unexpected(failure_);
      if (state_ != platform::SecureDatagramState::established)
        return unexpected(platform::SecureDatagramError::invalid_state);
      if (size == 0)
        return unexpected(platform::SecureDatagramError::invalid_argument);
      SecretBytes secret;
      if (!secret.try_resize(size))
        return unexpected(platform::SecureDatagramError::internal);
      Mixer{key_}
          .absorb(exporter_tag)
          .absorb(label)
          .absorb(context)
          .fill(secret.span());
      return secret;
    }

    /// @brief See platform::SecureDatagramSession::state.
    ///
    /// @return Where the session is in its life.
    auto state() const noexcept -> platform::SecureDatagramState override {
      return state_;
    }

    /// @brief See platform::SecureDatagramSession::max_record_size.
    ///
    /// @return The MTU less record_overhead once established; zero before.
    auto max_record_size() const noexcept -> std::size_t override {
      return state_ == platform::SecureDatagramState::established
                 ? profile_.mtu - record_overhead
                 : 0;
    }

    /// @brief See platform::SecureDatagramSession::next_deadline.
    ///
    /// @return The earlier of the retransmit and handshake deadlines while
    ///     handshaking; nothing otherwise.
    auto next_deadline() const noexcept -> std::optional<Instant> override {
      if (state_ != platform::SecureDatagramState::handshaking)
        return std::nullopt;
      if (retransmit_deadline_)
        return std::min(*retransmit_deadline_, handshake_deadline_);
      return handshake_deadline_;
    }

    /// @brief Closes the session, sending a close alert when established.
    /// See platform::SecureDatagramSession::close.
    void close() noexcept override {
      if (state_ == platform::SecureDatagramState::established)
        transmit(alert(close_level));
      if (state_ != platform::SecureDatagramState::failed)
        state_ = platform::SecureDatagramState::closed;
      inbound_.clear();
      retransmit_deadline_.reset();
    }

   private:
    static constexpr std::size_t frame_header = 3;
    static constexpr std::uint8_t alert_type = 21;
    static constexpr std::uint8_t handshake_type = 22;
    static constexpr std::uint8_t record_type = 23;
    static constexpr std::uint8_t hello_kind = 1;
    static constexpr std::uint8_t finished_kind = 2;
    static constexpr std::uint8_t close_level = 1;
    static constexpr std::uint8_t fatal_level = 2;
    static constexpr std::uint64_t exporter_tag = 0x6578706f72746572ULL;
    static constexpr std::uint64_t record_tag = 0x7265636f72640000ULL;
    static constexpr std::uint64_t mask_tag = 0x6d61736b00000000ULL;
    static constexpr std::uint64_t finished_tag = 0x66696e6973686564ULL;

    auto client() const noexcept -> bool {
      return profile_.role == platform::SecureDatagramRole::client;
    }

    auto running() const noexcept -> bool {
      return state_ == platform::SecureDatagramState::handshaking ||
             state_ == platform::SecureDatagramState::established;
    }

    static void append(std::vector<std::byte>& output, std::uint64_t value) {
      for (int shift = 56; shift >= 0; shift -= 8)
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }

    static auto read(std::span<const std::byte> bytes) noexcept
        -> std::uint64_t {
      std::uint64_t value = 0;
      for (const auto byte : bytes.first(8))
        value = (value << 8U) | std::to_integer<std::uint64_t>(byte);
      return value;
    }

    static auto frame(std::uint8_t type, std::size_t body_size)
        -> std::vector<std::byte> {
      std::vector<std::byte> datagram;
      datagram.reserve(frame_header + body_size);
      datagram.push_back(std::byte{type});
      datagram.push_back(static_cast<std::byte>((body_size >> 8U) & 0xffU));
      datagram.push_back(static_cast<std::byte>(body_size & 0xffU));
      return datagram;
    }

    [[nodiscard]] auto dispatch(std::uint8_t type,
                                std::span<const std::byte> body, Instant now)
        -> Expected<void, platform::SecureDatagramError> {
      switch (type) {
        case handshake_type:
          return on_handshake(body, now);
        case record_type:
          return on_record(body);
        case alert_type:
          return on_alert(body);
        default:
          return {};
      }
    }

    auto hello() const -> std::vector<std::byte> {
      auto datagram =
          frame(handshake_type, 2 + platform::secure_datagram_digest_size +
                                    profile_.alpn.size());
      datagram.push_back(std::byte{hello_kind});
      datagram.insert(datagram.end(), digest_.begin(), digest_.end());
      datagram.push_back(static_cast<std::byte>(profile_.alpn.size()));
      for (const auto symbol : profile_.alpn)
        datagram.push_back(static_cast<std::byte>(symbol));
      return datagram;
    }

    auto finished(bool from_client) const -> std::vector<std::byte> {
      auto datagram = frame(handshake_type, 1 + 8);
      datagram.push_back(std::byte{finished_kind});
      append(datagram, proof(from_client));
      return datagram;
    }

    static auto alert(std::uint8_t level) -> std::vector<std::byte> {
      auto datagram = frame(alert_type, 1);
      datagram.push_back(std::byte{level});
      return datagram;
    }

    auto proof(bool from_client) const noexcept -> std::uint64_t {
      return Mixer{key_}
          .absorb(finished_tag)
          .absorb(std::uint64_t{from_client})
          .next();
    }

    auto seal(std::uint64_t sequence, std::span<const std::byte> record,
              bool sender) const noexcept -> std::uint64_t {
      return Mixer{key_}
          .absorb(record_tag)
          .absorb(std::uint64_t{sender})
          .absorb(sequence)
          .absorb(record)
          .next();
    }

    void mask(std::uint64_t sequence, std::span<std::byte> payload,
              bool sender) const {
      std::vector<std::byte> stream(payload.size());
      Mixer{key_}
          .absorb(mask_tag)
          .absorb(std::uint64_t{sender})
          .absorb(sequence)
          .fill(stream);
      for (std::size_t index = 0; index < payload.size(); ++index)
        payload[index] ^= stream[index];
    }

    void transmit(std::vector<std::byte> datagram) {
      if (shared_->lost > 0) {
        if (shared_->lost != every_datagram) --shared_->lost;
        return;
      }
      outbound_.push_back(std::move(datagram));
    }

    void begin_flight(std::vector<std::byte> flight, Instant now) {
      flight_ = std::move(flight);
      transmit(flight_);
      retransmit_interval_ = profile_.retransmit_first;
      retransmit_deadline_ = now + retransmit_interval_;
    }

    void derive_key(const Digest& peer) noexcept {
      const auto& first = client() ? digest_ : peer;
      const auto& second = client() ? peer : digest_;
      key_ = Mixer{0}.absorb(first).absorb(second).next();
    }

    [[nodiscard]] auto on_handshake(std::span<const std::byte> body,
                                    Instant now)
        -> Expected<void, platform::SecureDatagramError> {
      if (body.empty()) return {};
      const auto kind = std::to_integer<std::uint8_t>(body[0]);
      if (kind == hello_kind) return on_hello(body.subspan(1), now);
      if (kind == finished_kind) return on_finished(body.subspan(1));
      return {};
    }

    [[nodiscard]] auto on_hello(std::span<const std::byte> body, Instant now)
        -> Expected<void, platform::SecureDatagramError> {
      if (state_ != platform::SecureDatagramState::handshaking ||
          body.size() < platform::secure_datagram_digest_size + 1)
        return {};
      Digest peer{};
      std::ranges::copy(body.first(peer.size()), peer.begin());
      const auto alpn = body.subspan(peer.size() + 1);
      if (std::to_integer<std::size_t>(body[peer.size()]) != alpn.size())
        return {};
      if (peer != pinned_) {
        transmit(alert(fatal_level));
        return fail_with(platform::SecureDatagramError::peer_not_pinned);
      }
      const auto wanted =
          std::as_bytes(std::span{profile_.alpn.data(), profile_.alpn.size()});
      if (!std::ranges::equal(alpn, wanted)) {
        transmit(alert(fatal_level));
        return fail_with(platform::SecureDatagramError::handshake_failed);
      }
      derive_key(peer);
      keyed_ = true;
      begin_flight(client() ? finished(true) : hello(), now);
      return {};
    }

    [[nodiscard]] auto on_finished(std::span<const std::byte> body)
        -> Expected<void, platform::SecureDatagramError> {
      if (!keyed_ || body.size() != 8 || read(body) != proof(!client()))
        return {};
      if (state_ == platform::SecureDatagramState::handshaking) {
        state_ = platform::SecureDatagramState::established;
        retransmit_deadline_.reset();
        if (!client()) transmit(finished(false));
        return {};
      }
      if (!client()) transmit(finished(false));
      return {};
    }

    [[nodiscard]] auto on_record(std::span<const std::byte> body)
        -> Expected<void, platform::SecureDatagramError> {
      constexpr std::size_t header = record_overhead - frame_header;
      if (state_ != platform::SecureDatagramState::established ||
          body.size() <= header)
        return {};
      const auto sequence = read(body);
      const auto tag = read(body.subspan(8));
      const auto sealed = body.subspan(header);
      std::vector<std::byte> payload{sealed.begin(), sealed.end()};
      mask(sequence, payload, !client());
      if (tag != seal(sequence, payload, !client()) ||
          !seen_.insert(sequence).second)
        return {};
      if (received_ >= profile_.record_limit)
        return fail_with(platform::SecureDatagramError::record_limit);
      ++received_;
      auto record = SecretBytes::from_bytes(payload);
      if (!record) return fail_with(platform::SecureDatagramError::internal);
      inbound_.push_back(std::move(record).value());
      return {};
    }

    [[nodiscard]] auto on_alert(std::span<const std::byte> body)
        -> Expected<void, platform::SecureDatagramError> {
      if (body.size() != 1) return {};
      const auto level = std::to_integer<std::uint8_t>(body[0]);
      if (level == fatal_level &&
          state_ == platform::SecureDatagramState::handshaking)
        return fail_with(platform::SecureDatagramError::handshake_failed);
      if (level == close_level &&
          state_ == platform::SecureDatagramState::established) {
        state_ = platform::SecureDatagramState::closed;
        retransmit_deadline_.reset();
      }
      return {};
    }

    [[nodiscard]] auto expire(Instant now)
        -> Expected<void, platform::SecureDatagramError> {
      if (state_ == platform::SecureDatagramState::handshaking &&
          now >= handshake_deadline_)
        return fail_with(platform::SecureDatagramError::handshake_timeout);
      return {};
    }

    auto fail_with(platform::SecureDatagramError error)
        -> Unexpected<platform::SecureDatagramError> {
      state_ = platform::SecureDatagramState::failed;
      failure_ = error;
      inbound_.clear();
      retransmit_deadline_.reset();
      return unexpected(error);
    }

    platform::SecureDatagramProfile profile_;
    std::shared_ptr<Shared> shared_;
    Digest digest_{};
    Digest pinned_{};
    platform::SecureDatagramState state_{platform::SecureDatagramState::idle};
    platform::SecureDatagramError failure_{
        platform::SecureDatagramError::internal};
    std::deque<std::vector<std::byte>> outbound_;
    std::deque<SecretBytes> inbound_;
    std::vector<std::byte> flight_;
    std::set<std::uint64_t> seen_;
    std::optional<Instant> retransmit_deadline_;
    Instant retransmit_interval_{};
    Instant handshake_deadline_{};
    std::uint64_t key_{};
    std::uint64_t sent_{};
    std::uint64_t received_{};
    bool keyed_{};
  };

  static inline std::atomic<std::uint64_t> instances_{0};

  std::shared_ptr<Shared> shared_;
};

}  // namespace libchiaking::testing
