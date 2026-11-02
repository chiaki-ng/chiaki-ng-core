#include "src/core/net/kakusan/pake.hpp"

#include "src/core/utils/bytes/byte_io.hpp"
#include "src/core/utils/bytes/endian.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/hex.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace libchiaking::net::kakusan {

class PakeArithmetic {
 public:
  using SecretPoint = SecretArray<platform::p256_point_size>;
  using Coordinate = std::span<const std::byte, platform::p256_scalar_size>;

  static constexpr auto field_prime =
      text::hex_bytes<platform::p256_scalar_size>(
          "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff");
  static constexpr auto group_order =
      text::hex_bytes<platform::p256_scalar_size>(
          "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551");
  static constexpr auto spake2_m = text::hex_bytes<platform::p256_point_size>(
      "04886e2f97ace46e55ba9dd7242579f2993b64e16ef3dcab95afd497333d8fa12f"
      "5ff355163e43ce224e0b0e65ff02ac8e5c7be09419c785e0ca547d55a12e2d20");
  static constexpr auto spake2_n = text::hex_bytes<platform::p256_point_size>(
      "04d8bbd6c639c62937b04d997f38c3770719c629d7014d49a24b4f98baa1292b49"
      "07d60aa6bfade45008a636337f5168c64d9bd36034808cd564490b1e656edbe7");

  static constexpr std::string_view confirmation_info = "ConfirmationKeys";
  static constexpr platform::ScryptParameters password_scrypt{32768, 8, 1};

  static constexpr auto subtract(
      Coordinate left, Coordinate right,
      std::span<std::byte, platform::p256_scalar_size> difference) noexcept
      -> unsigned {
    unsigned borrow = 0;
    for (std::size_t index = difference.size(); index != 0; --index) {
      const auto value = std::to_integer<unsigned>(left[index - 1]) -
                         std::to_integer<unsigned>(right[index - 1]) - borrow;
      difference[index - 1] = static_cast<std::byte>(value & 0xffU);
      borrow = (value >> 8U) & 1U;
    }
    return borrow;
  }

  static auto negate(
      std::span<const std::byte, platform::p256_point_size> point) noexcept
      -> SecretPoint {
    SecretPoint negated;
    std::ranges::copy(point, negated.begin());
    (void)subtract(
        field_prime, point.subspan<sec1_y_offset, platform::p256_scalar_size>(),
        negated.span().subspan<sec1_y_offset, platform::p256_scalar_size>());
    return negated;
  }

  static auto is_zero(const Scalar& scalar) noexcept -> bool {
    std::uint8_t combined = 0;
    for (const auto byte : scalar)
      combined = static_cast<std::uint8_t>(combined |
                                           std::to_integer<std::uint8_t>(byte));
    return combined == 0;
  }

  static auto party(
      std::string_view label,
      const std::array<std::byte, platform::ed25519_public_key_size>& identity)
      -> std::vector<std::byte> {
    std::vector<std::byte> bytes;
    bytes.reserve(label.size() + 1 + identity.size());
    bytes::Appender writer{bytes};
    writer.text(label);
    writer.u8(0);
    writer.bytes(identity);
    return bytes;
  }

  using Hash512 = SecretArray<platform::sha512_digest_size>;

  static auto sha512(platform::CryptoProvider& crypto,
                     std::span<const std::byte> input) -> Result<Hash512> {
    Hash512 digest;
    const auto hashed = crypto.sha512(input, std::span{digest});
    if (!hashed) return fail(Hash::failure(hashed.error()));
    return digest;
  }

  static auto hmac_sha512(platform::CryptoProvider& crypto,
                          std::span<const std::byte> key,
                          std::span<const std::byte> input) -> Result<Hash512> {
    Hash512 digest;
    const auto authenticated =
        crypto.hmac_sha512(key, input, std::span{digest});
    if (!authenticated) return fail(Hash::failure(authenticated.error()));
    return digest;
  }

  static auto masked_share(platform::CryptoProvider& crypto,
                           const Scalar& password, const Scalar& ephemeral,
                           const Point& mask) -> Result<Point> {
    SecretPoint blinded;
    SecretPoint base;
    Point share{};
    const auto blind = crypto.p256_multiply(password, mask, blinded);
    if (!blind) return fail(Hash::failure(blind.error()));
    const auto multiplied = crypto.p256_base_multiply(ephemeral, base);
    if (!multiplied) return fail(Hash::failure(multiplied.error()));
    const auto added = crypto.p256_add(base, blinded, share);
    if (!added) return fail(Error::pairing_failed);
    return share;
  }
};

class PakeTranscript {
 public:
  static auto create(const Parties& parties,
                     std::span<const std::byte> host_share,
                     std::span<const std::byte> guest_share,
                     std::span<const std::byte> shared,
                     std::span<const std::byte> password)
      -> Result<PakeTranscript> {
    try {
      const auto host =
          PakeArithmetic::party(host_label, parties.host_identity);
      const auto guest =
          PakeArithmetic::party(guest_label, parties.guest_identity);
      const std::array<std::span<const std::byte>, 6> items{
          host, guest, host_share, guest_share, shared, password};
      PakeTranscript record;
      for (const auto item : items) {
        if (!record.append_item(item)) return fail(Error::internal);
      }
      return record;
    } catch (...) {
      return fail(Error::internal);
    }
  }

  PakeTranscript(const PakeTranscript&) = delete;
  PakeTranscript& operator=(const PakeTranscript&) = delete;
  PakeTranscript(PakeTranscript&&) noexcept = default;
  PakeTranscript& operator=(PakeTranscript&&) noexcept = default;
  ~PakeTranscript() = default;

  auto bytes() const noexcept -> std::span<const std::byte> {
    return record_.span();
  }

 private:
  PakeTranscript() = default;

  auto append_item(std::span<const std::byte> item) noexcept
      -> Expected<void, SecretBytes::Error> {
    std::array<std::byte, sizeof(std::uint64_t)> header{};
    bytes::store_le<std::uint64_t>(header, item.size());
    const auto appended = record_.try_append(header);
    if (!appended) return appended;
    return record_.try_append(item);
  }

  SecretBytes record_;
};

auto Exchange::transcript(platform::CryptoProvider& crypto,
                          const Digest256& invitation_hash,
                          const Digest256& reply_hash) -> Result<Transcript> {
  std::vector<std::byte> input;
  input.reserve(transcript_label.size() + 1 + invitation_hash.size() +
                reply_hash.size());
  bytes::Appender writer{input};
  writer.text(transcript_label);
  writer.u8(0);
  writer.bytes(invitation_hash);
  writer.bytes(reply_hash);
  return Hash::sha256(crypto, input);
}

auto Exchange::reduce_wide(
    std::span<const std::byte, wide_scalar_size> wide) noexcept -> Scalar {
  Scalar remainder;
  Scalar reduced;
  for (const auto byte : wide) {
    const auto value = std::to_integer<unsigned>(byte);
    for (unsigned bit = 8; bit != 0; --bit) {
      const auto overflow = std::to_integer<unsigned>(remainder[0]) >> 7U;
      for (std::size_t index = 0; index + 1 < remainder.size(); ++index)
        remainder[index] =
            (remainder[index] << 1U) | (remainder[index + 1] >> 7U);
      remainder[remainder.size() - 1] =
          (remainder[remainder.size() - 1] << 1U) |
          static_cast<std::byte>((value >> (bit - 1U)) & 1U);
      const auto borrow = PakeArithmetic::subtract(
          remainder.span(), PakeArithmetic::group_order, reduced.span());
      const auto keep = (overflow | (borrow ^ 1U)) & 1U;
      const auto mask = static_cast<std::byte>(0U - keep);
      for (std::size_t index = 0; index < remainder.size(); ++index)
        remainder[index] = (reduced[index] & mask) | (remainder[index] & ~mask);
    }
  }
  return remainder;
}

auto Exchange::password_scalar(platform::CryptoProvider& crypto,
                               const Code& host_code, const Code& guest_code,
                               InvitationIdBytes invitation_id)
    -> Result<Scalar> {
  SecretArray<2 * code_size> password{};
  for (std::size_t index = 0; index < code_size; ++index) {
    password[index] = static_cast<std::byte>(host_code.data()[index]);
    password[code_size + index] =
        static_cast<std::byte>(guest_code.data()[index]);
  }
  std::vector<std::byte> salt;
  salt.reserve(password_label.size() + 1 + invitation_id.size());
  bytes::Appender writer{salt};
  writer.text(password_label);
  writer.u8(0);
  writer.bytes(invitation_id);

  SecretArray<wide_scalar_size> wide{};
  const auto stretched =
      crypto.scrypt(password, salt, PakeArithmetic::password_scrypt, wide);
  if (!stretched) {
    return fail(Hash::failure(stretched.error()));
  }
  auto scalar = reduce_wide(
      std::span<const std::byte, wide_scalar_size>{wide.data(), wide.size()});
  if (PakeArithmetic::is_zero(scalar)) return fail(Error::pairing_failed);
  return scalar;
}

auto Exchange::ephemeral(platform::SecureRandomProvider& random)
    -> std::optional<Scalar> {
  for (std::size_t attempt = 0; attempt < ephemeral_draw_attempts; ++attempt) {
    SecretArray<wide_scalar_size> wide;
    if (!random.fill_bytes(wide)) return std::nullopt;
    auto scalar = reduce_wide(
        std::span<const std::byte, wide_scalar_size>{wide.data(), wide.size()});
    if (!PakeArithmetic::is_zero(scalar)) return scalar;
  }
  return std::nullopt;
}

auto Exchange::begin(platform::CryptoProvider& crypto, Role role,
                     Scalar password, Scalar ephemeral, const Parties& parties,
                     std::span<const std::byte> associated_data)
    -> Result<Exchange> {
  if (PakeArithmetic::is_zero(password) || PakeArithmetic::is_zero(ephemeral))
    return fail(Error::pairing_failed);
  const auto& mask =
      role == Role::host ? PakeArithmetic::spake2_m : PakeArithmetic::spake2_n;
  auto share = PakeArithmetic::masked_share(crypto, password, ephemeral, mask);
  if (!share) return fail(share.error());
  Secrets secrets{std::move(password), std::move(ephemeral), {}};
  if (!secrets.associated_data.try_assign(associated_data))
    return fail(Error::internal);
  return Exchange{role, share.value(), parties, std::move(secrets)};
}

Exchange::Exchange(Role role, const Point& share, const Parties& parties,
                   Secrets secrets) noexcept
    : role_(role),
      share_(share),
      parties_(parties),
      secrets_(std::move(secrets)) {}

Exchange::Exchange(Exchange&& other) noexcept
    : role_(other.role_),
      share_(other.share_),
      parties_(other.parties_),
      secrets_(std::exchange(other.secrets_, std::nullopt)) {}

Exchange& Exchange::operator=(Exchange&& other) noexcept {
  if (this != &other) {
    role_ = other.role_;
    share_ = other.share_;
    parties_ = other.parties_;
    secrets_ = std::exchange(other.secrets_, std::nullopt);
  }
  return *this;
}

auto Exchange::complete(platform::CryptoProvider& crypto,
                        std::span<const std::byte> peer_share)
    -> Result<Confirmations> {
  if (!secrets_ || peer_share.size() != platform::p256_point_size)
    return fail(Error::pairing_failed);
  const auto secrets = std::exchange(secrets_, std::nullopt);

  const auto& peer_mask =
      role_ == Role::host ? PakeArithmetic::spake2_n : PakeArithmetic::spake2_m;
  PakeArithmetic::SecretPoint blinded;
  PakeArithmetic::SecretPoint unmasked;
  PakeArithmetic::SecretPoint shared;
  const auto blind =
      crypto.p256_multiply(secrets->password, peer_mask, blinded);
  if (!blind) return fail(Hash::failure(blind.error()));
  const auto removed = crypto.p256_add(
      peer_share,
      PakeArithmetic::negate(
          std::span<const std::byte, platform::p256_point_size>{blinded}),
      unmasked);
  if (!removed) return fail(Error::pairing_failed);
  const auto agreed =
      crypto.p256_multiply(secrets->ephemeral, unmasked, shared);
  if (!agreed) return fail(Error::pairing_failed);

  Point peer{};
  std::copy(peer_share.begin(), peer_share.end(), peer.begin());
  const auto& host_share = role_ == Role::host ? share_ : peer;
  const auto& guest_share = role_ == Role::host ? peer : share_;

  auto built = PakeTranscript::create(parties_, host_share, guest_share, shared,
                                      secrets->password);
  if (!built) return fail(built.error());
  auto record = std::move(built).value();
  Confirmations confirmations;
  auto hashed = PakeArithmetic::sha512(crypto, record.bytes());
  if (!hashed) return fail(hashed.error());
  const auto half = hashed.value().size() / 2;
  std::ranges::copy(std::span<const std::byte>{hashed.value()}.first(half),
                    confirmations.shared_key.begin());

  SecretBytes info;
  if (!info.try_append(Hash::bytes_of(PakeArithmetic::confirmation_info)) ||
      !info.try_append(secrets->associated_data.span()))
    return fail(Error::internal);
  SecretArray<platform::sha512_digest_size> expanded;
  if (const auto derived =
          crypto.hkdf(platform::HkdfDigest::sha512, {},
                      std::span<const std::byte>{hashed.value()}.subspan(half),
                      info.span(), expanded.span());
      !derived)
    return fail(Hash::failure(derived.error()));
  const auto keys = std::span<const std::byte>{expanded};
  const auto host_mac =
      PakeArithmetic::hmac_sha512(crypto, keys.first(half), record.bytes());
  const auto guest_mac =
      PakeArithmetic::hmac_sha512(crypto, keys.subspan(half), record.bytes());
  if (!host_mac || !guest_mac) return fail(Error::internal);
  std::ranges::copy(host_mac.value(), confirmations.host.begin());
  std::ranges::copy(guest_mac.value(), confirmations.guest.begin());
  return confirmations;
}

}  // namespace libchiaking::net::kakusan
