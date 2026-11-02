#include "src/core/net/kakusan/short_block.hpp"

#include "libchiaking/detail/secret_array.hpp"

#include "src/core/utils/bytes/crc32.hpp"
#include "src/core/utils/memory/constant_time.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/base64.hpp"
#include "src/core/utils/text/hex.hpp"

#include <format>
#include <iterator>
#include <limits>
#include <utility>

namespace libchiaking::net::kakusan {

auto ShortBlockCodec::derive(platform::CryptoProvider& crypto,
                             const BlockSeed& seed, std::string_view label,
                             BlockKind kind, std::span<std::byte> output)
    -> Result<void> {
  std::vector<std::byte> info;
  info.reserve(label.size() + 2);
  bytes::Appender writer{info};
  writer.text(label);
  writer.u8(0);
  writer.u8(static_cast<std::uint8_t>(kind));
  return crypto
      .hkdf(platform::HkdfDigest::sha256, Hash::bytes_of(seed_salt), seed, info,
            output)
      .transform_error(Hash::failure);
}

auto ShortBlockCodec::credentials(platform::CryptoProvider& crypto,
                                  const BlockSeed& seed, BlockKind kind)
    -> Result<IceCredentials> {
  SecretArray<ufrag_bytes> ufrag;
  SecretArray<password_bytes> password;
  if (const auto derived =
          derive(crypto, seed, "ice-ufrag", kind, ufrag.span());
      !derived)
    return fail(derived.error());
  if (const auto derived =
          derive(crypto, seed, "ice-password", kind, password.span());
      !derived)
    return fail(derived.error());
  SecretString ufrag_text;
  SecretString password_text;
  text::append_hex(ufrag_text, ufrag);
  text::append_hex(password_text, password);
  auto made =
      IceCredentials::make(std::move(ufrag_text), std::move(password_text));
  if (!made) return fail(Error::internal);
  return std::move(made.value());
}

auto ShortBlockCodec::invitation_id(platform::CryptoProvider& crypto,
                                    const BlockSeed& seed)
    -> Result<InvitationId> {
  InvitationId id{};
  if (const auto derived =
          derive(crypto, seed, "invitation_id", BlockKind::invitation, id);
      !derived)
    return fail(derived.error());
  return id;
}

auto ShortBlockCodec::adopt_credentials(platform::CryptoProvider& crypto,
                                        const BlockSeed& seed, BlockKind kind,
                                        CandidateSection section)
    -> Result<IceParameters> {
  auto made = credentials(crypto, seed, kind);
  if (!made) return fail(made.error());
  auto ice = IceParameters::make(std::move(made.value()),
                                 std::move(section.candidates),
                                 section.port_guesses);
  if (!ice) return fail(Error::invalid_block);
  return std::move(ice.value());
}

auto ShortBlockCodec::write_candidates(std::vector<std::byte>& output,
                                       const IceParameters& ice)
    -> Result<void> {
  const auto candidates = ice.candidates();
  bytes::Appender writer{output};
  writer.u8(static_cast<std::uint8_t>(ice.port_guesses()));
  writer.u8(static_cast<std::uint8_t>(candidates.size()));
  std::vector<platform::SocketEndpoint> hosts;
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    const auto& candidate = candidates[index];
    std::uint8_t flags{};
    if (candidate.type == CandidateType::server_reflexive) {
      const auto base = std::ranges::find(hosts, candidate.related);
      if (base == hosts.end()) return fail(Error::invalid_block);
      flags = static_cast<std::uint8_t>(
          srflx_flag | static_cast<std::uint8_t>(base - hosts.begin()));
    } else {
      if (hosts.size() != index) return fail(Error::invalid_block);
      hosts.push_back(candidate.endpoint);
    }
    const auto& address = candidate.endpoint.address;
    writer.u8(address.is_ipv6() ? static_cast<std::uint8_t>(flags | ipv6_flag)
                                : flags);
    writer.bytes(address.bytes());
    writer.u16_be(candidate.endpoint.port);
  }
  return {};
}

auto ShortBlockCodec::read_address(bytes::Reader& reader, bool ipv6)
    -> Result<platform::IpAddress> {
  const auto raw = reader.take(ipv6 ? 16U : 4U);
  if (!raw) return fail(Error::invalid_block);
  return *platform::IpAddress::from_bytes(raw.value());
}

auto ShortBlockCodec::read_candidates(bytes::Reader& reader)
    -> Result<CandidateSection> {
  const auto guesses = reader.u8();
  const auto count = reader.u8();
  if (!guesses || !count || guesses.value() > nat::ice::max_port_guesses ||
      count.value() == 0 || count.value() > max_candidates)
    return fail(Error::invalid_block);
  CandidateSection ice;
  ice.port_guesses = guesses.value();
  std::vector<platform::SocketEndpoint> hosts;
  for (std::size_t index = 0; index < count.value(); ++index) {
    const auto flags = reader.u8();
    if (!flags || (flags.value() & reserved_flag) != 0)
      return fail(Error::invalid_block);
    const auto address = read_address(reader, (flags.value() & ipv6_flag) != 0);
    const auto port = reader.u16_be();
    if (!address || !port) return fail(Error::invalid_block);
    const auto base = static_cast<std::size_t>(flags.value() & base_mask);
    Candidate candidate;
    candidate.endpoint =
        platform::SocketEndpoint{address.value(), port.value()};
    if ((flags.value() & srflx_flag) != 0) {
      if (base >= hosts.size()) return fail(Error::invalid_block);
      candidate.type = CandidateType::server_reflexive;
      candidate.related = hosts[base];
      candidate.priority = priority(srflx_preference, base);
      candidate.foundation = std::format("s{}", base);
    } else {
      if (base != 0 || hosts.size() != index) return fail(Error::invalid_block);
      candidate.priority = priority(host_preference, index);
      candidate.foundation = std::format("h{}", index);
      hosts.push_back(candidate.endpoint);
    }
    ice.candidates.push_back(std::move(candidate));
  }
  return ice;
}

auto ShortBlockCodec::finish(std::vector<std::byte>& bytes)
    -> Result<SecretString> {
  if (bytes.size() + check_size > max_block_bytes)
    return fail(Error::invalid_block);
  const auto check = bytes::crc32(bytes);
  bytes::Appender{bytes}.u32_be(check);
  SecretString text{block_prefix};
  text::base64::append(text, bytes, text::base64::Alphabet::url);
  return text;
}

auto ShortBlockCodec::body_of(std::span<const std::byte> bytes) noexcept
    -> std::span<const std::byte> {
  return bytes.first(bytes.size() - check_size);
}

template <class Body>
auto ShortBlockCodec::sealed(platform::CryptoProvider& crypto, Body body,
                             std::vector<std::byte> bytes)
    -> Result<Sealed<Body>> {
  const auto hash = Hash::sha256(crypto, bytes);
  if (!hash) return fail(hash.error());
  SecretString text{block_prefix};
  text::base64::append(text, bytes, text::base64::Alphabet::url);
  return Sealed<Body>{std::move(body), hash.value(), std::move(text)};
}

auto ShortBlockCodec::seal(platform::CryptoProvider& crypto,
                           const ShortInvitation& invitation)
    -> Result<SealedInvitation> {
  if (invitation.invite_expires_at > std::numeric_limits<std::uint32_t>::max())
    return fail(Error::invalid_block);
  std::vector<std::byte> bytes;
  bytes::Appender writer{bytes};
  writer.u8(static_cast<std::uint8_t>(BlockKind::invitation));
  writer.bytes(invitation.seed);
  writer.u32_be(static_cast<std::uint32_t>(invitation.invite_expires_at));
  writer.u8(invitation.source_protocol);
  writer.u8(invitation.capacity);
  writer.bytes(invitation.dtls_sha256);
  writer.bytes(invitation.identity_hint);
  if (const auto written = write_candidates(bytes, invitation.ice); !written)
    return fail(written.error());
  const auto text = finish(bytes);
  if (!text) return fail(text.error());
  return parse_invitation(crypto, text.value().view());
}

auto ShortBlockCodec::seal(platform::CryptoProvider& crypto,
                           const ShortReply& reply) -> Result<SealedReply> {
  std::vector<std::byte> bytes;
  bytes::Appender writer{bytes};
  writer.u8(static_cast<std::uint8_t>(BlockKind::reply));
  writer.bytes(reply.seed);
  writer.bytes(reply.invite_tag);
  writer.bytes(reply.dtls_sha256);
  writer.bytes(reply.identity_hint);
  if (const auto written = write_candidates(bytes, reply.ice); !written)
    return fail(written.error());
  const auto text = finish(bytes);
  if (!text) return fail(text.error());
  return parse_reply(crypto, text.value().view());
}

auto ShortBlockCodec::decode(std::string_view text)
    -> Result<std::vector<std::byte>> {
  if (text.size() > max_text_size) return fail(Error::invalid_block);
  std::string compact;
  compact.reserve(text.size());
  std::ranges::copy_if(text, std::back_inserter(compact),
                       [](char symbol) { return !text::ascii_space(symbol); });
  const std::string_view view{compact};
  if (!view.starts_with(block_prefix)) return fail(Error::invalid_block);
  const auto encoded = view.substr(block_prefix.size());
  const auto size = text::base64::url_decoded_size(encoded.size());
  if (!size || *size > max_block_bytes || *size <= check_size)
    return fail(Error::invalid_block);
  std::vector<std::byte> bytes(*size);
  if (!text::base64::decode_url_into(encoded, bytes))
    return fail(Error::invalid_block);
  bytes::Reader check{std::span<const std::byte>{bytes}.last(check_size)};
  if (check.u32_be().value() != bytes::crc32(body_of(bytes)))
    return fail(Error::invalid_block);
  return bytes;
}

auto ShortBlockCodec::parse_invitation(platform::CryptoProvider& crypto,
                                       std::string_view text)
    -> Result<SealedInvitation> {
  auto bytes = decode(text);
  if (!bytes) return fail(bytes.error());
  bytes::Reader reader{body_of(bytes.value())};
  const auto head = reader.u8();
  const auto seed = read_array<BlockSeed>(reader);
  const auto expires = reader.u32_be();
  const auto source = reader.u8();
  const auto capacity = reader.u8();
  if (!head ||
      head.value() != static_cast<std::uint8_t>(BlockKind::invitation) ||
      !seed || !expires || !source || !capacity ||
      source.value() < lowest_source || source.value() > highest_source ||
      capacity.value() == 0 || expires.value() < invitation_lifetime_seconds)
    return fail(Error::invalid_block);
  const auto dtls = read_array<Digest256>(reader);
  const auto hint = read_array<IdentityHint>(reader);
  if (!dtls || !hint) return fail(Error::invalid_block);
  auto ice = read_candidates(reader);
  if (!ice) return fail(ice.error());
  if (reader.remaining() != 0) return fail(Error::invalid_block);
  auto adopted = adopt_credentials(crypto, *seed, BlockKind::invitation,
                                   std::move(ice).value());
  if (!adopted) return fail(adopted.error());
  ShortInvitation invitation{*seed,
                             expires.value(),
                             source.value(),
                             capacity.value(),
                             *dtls,
                             *hint,
                             std::move(adopted).value(),
                             {}};
  if (const auto derived =
          derive(crypto, invitation.seed, "invitation_id",
                 BlockKind::invitation, invitation.invitation_id);
      !derived)
    return fail(derived.error());
  return sealed(crypto, std::move(invitation), std::move(bytes).value());
}

auto ShortBlockCodec::parse_reply(platform::CryptoProvider& crypto,
                                  std::string_view text)
    -> Result<SealedReply> {
  auto bytes = decode(text);
  if (!bytes) return fail(bytes.error());
  bytes::Reader reader{body_of(bytes.value())};
  const auto head = reader.u8();
  const auto seed = read_array<BlockSeed>(reader);
  const auto tag = read_array<InviteTag>(reader);
  const auto dtls = read_array<Digest256>(reader);
  const auto hint = read_array<IdentityHint>(reader);
  if (!head || head.value() != static_cast<std::uint8_t>(BlockKind::reply) ||
      !seed || !tag || !dtls || !hint)
    return fail(Error::invalid_block);
  auto ice = read_candidates(reader);
  if (!ice) return fail(ice.error());
  if (reader.remaining() != 0) return fail(Error::invalid_block);
  auto adopted = adopt_credentials(crypto, *seed, BlockKind::reply,
                                   std::move(ice).value());
  if (!adopted) return fail(adopted.error());
  ShortReply reply{*seed, *tag, *dtls, *hint, std::move(adopted).value(), {}};
  if (const auto derived = derive(crypto, reply.seed, "reply_nonce",
                                  BlockKind::reply, reply.reply_nonce);
      !derived)
    return fail(derived.error());
  return sealed(crypto, std::move(reply), std::move(bytes).value());
}

auto ShortInvitation::clone() const -> ShortInvitation {
  return ShortInvitation{seed,        invite_expires_at, source_protocol,
                         capacity,    dtls_sha256,       identity_hint,
                         ice.clone(), invitation_id};
}

auto ShortReply::clone() const -> ShortReply {
  return ShortReply{seed,          invite_tag,  dtls_sha256,
                    identity_hint, ice.clone(), reply_nonce};
}

auto ShortBlockCodec::tag_of(const Digest256& invitation_hash) noexcept
    -> InviteTag {
  InviteTag tag{};
  std::ranges::copy(std::span{invitation_hash}.first(tag.size()), tag.begin());
  return tag;
}

auto ShortBlockCodec::hint_of(const IdentityKey& key) noexcept -> IdentityHint {
  IdentityHint hint{};
  std::ranges::copy(std::span{key}.first(hint.size()), hint.begin());
  return hint;
}

auto ShortBlockCodec::answers(const SealedInvitation& invitation,
                              const SealedReply& reply) noexcept -> bool {
  return memory::constant_time_equal(tag_of(invitation.hash),
                                     reply.body.invite_tag);
}

}  // namespace libchiaking::net::kakusan
