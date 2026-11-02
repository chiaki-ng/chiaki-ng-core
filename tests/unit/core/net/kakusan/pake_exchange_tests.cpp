#include "src/core/net/kakusan/pake.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/fixtures.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace net_kakusan_test {
namespace {

using namespace libchiaking::net::kakusan;
using test_support::Bytes;
using test_support::kakusan::Fixtures;

static_assert(!std::is_copy_constructible_v<SharedKey>);
static_assert(!std::is_copy_constructible_v<Confirmations>);

class PakeExchange : public ::testing::Test {
 protected:
  struct Outcome {
    Result<Confirmations> host;
    Result<Confirmations> guest;
  };

  static auto scalar(std::string_view text) -> Scalar {
    Scalar owned;
    std::ranges::copy(Bytes::unhex_array<32>(text), owned.begin());
    return owned;
  }

  auto text(std::string_view key) const -> std::string {
    return Fixtures::string(document, key);
  }

  auto begin(Role role) -> std::optional<Exchange> {
    auto exchange = Exchange::begin(
        crypto, role, password.clone(),
        (role == Role::host ? host_scalar : guest_scalar).clone(), parties,
        associated);
    if (!exchange) {
      ADD_FAILURE() << "the exchange does not begin";
      return std::nullopt;
    }
    return std::move(exchange).value();
  }

  auto run(const Scalar& host_password, const Scalar& guest_password,
           const Parties& host_parties, const Parties& guest_parties,
           std::span<const std::byte> host_data,
           std::span<const std::byte> guest_data) -> Outcome {
    auto host = Exchange::begin(crypto, Role::host, host_password.clone(),
                                host_scalar.clone(), host_parties, host_data);
    auto guest =
        Exchange::begin(crypto, Role::guest, guest_password.clone(),
                        guest_scalar.clone(), guest_parties, guest_data);
    EXPECT_TRUE(host);
    EXPECT_TRUE(guest);
    if (!host || !guest)
      return Outcome{fail(Error::internal), fail(Error::internal)};
    const auto host_share = host.value().share();
    const auto guest_share = guest.value().share();
    return Outcome{host.value().complete(crypto, guest_share),
                   guest.value().complete(crypto, host_share)};
  }

  static auto agree(const Outcome& outcome) -> bool {
    return outcome.host && outcome.guest &&
           std::ranges::equal(outcome.host.value().host,
                              outcome.guest.value().host) &&
           std::ranges::equal(outcome.host.value().guest,
                              outcome.guest.value().guest);
  }

  test_support::SoftwareCrypto crypto;
  std::string document{Fixtures::text("vectors.json")};
  Digest256 invite_hash{
      Bytes::unhex_array<32>(Fixtures::string(document, "invite_hash"))};
  Digest256 reply_hash{
      Bytes::unhex_array<32>(Fixtures::string(document, "reply_hash"))};
  Scalar password{scalar(Fixtures::string(document, "w"))};
  Scalar host_scalar{scalar(Fixtures::string(document, "x"))};
  Scalar guest_scalar{scalar(Fixtures::string(document, "y"))};
  std::vector<std::byte> associated{
      Bytes::unhex(Fixtures::string(document, "AAD"))};
  Parties parties{
      Bytes::unhex_array<32>(Fixtures::string(document, "A").substr(22)),
      Bytes::unhex_array<32>(Fixtures::string(document, "B").substr(24))};
};

TEST_F(PakeExchange, HashesTheBlocksIntoTheReferenceTranscript) {
  const auto computed = Exchange::transcript(crypto, invite_hash, reply_hash);
  ASSERT_TRUE(computed);
  EXPECT_EQ(Bytes::hex(computed.value()), text("transcript"));
  const auto swapped = Exchange::transcript(crypto, reply_hash, invite_hash);
  ASSERT_TRUE(swapped);
  EXPECT_NE(swapped.value(), computed.value());
}

TEST_F(PakeExchange, BindsTheTranscriptAndExporterAsAssociatedData) {
  EXPECT_EQ(Bytes::hex(associated).substr(0, 64), text("transcript"));
  EXPECT_EQ(Bytes::hex(associated).substr(64), text("pairing_exporter"));
}

TEST_F(PakeExchange, DerivesTheReferencePasswordScalar) {
  const auto wide = Bytes::unhex_array<64>(text("wide"));
  EXPECT_TRUE(std::ranges::equal(Exchange::reduce_wide(wide), password));

  const auto invitation_id = Bytes::unhex_array<16>(
      text("salt").substr(2 * (password_label.size() + 1)));
  const auto host_code = Code::parse(text("host_code"));
  const auto guest_code = Code::parse(text("guest_code"));
  ASSERT_TRUE(host_code);
  ASSERT_TRUE(guest_code);
  const auto derived = Exchange::password_scalar(
      crypto, host_code.value(), guest_code.value(), invitation_id);
  ASSERT_TRUE(derived);
  EXPECT_TRUE(std::ranges::equal(derived.value(), password));

  const auto reversed = Exchange::password_scalar(
      crypto, guest_code.value(), host_code.value(), invitation_id);
  ASSERT_TRUE(reversed);
  EXPECT_FALSE(std::ranges::equal(reversed.value(), password));
  auto other_invitation = invitation_id;
  other_invitation[0] ^= std::byte{1};
  const auto rebound = Exchange::password_scalar(
      crypto, host_code.value(), guest_code.value(), other_invitation);
  ASSERT_TRUE(rebound);
  EXPECT_FALSE(std::ranges::equal(rebound.value(), password));
}

TEST_F(PakeExchange, MatchesTheReferenceSharesAndKeys) {
  auto host = begin(Role::host);
  auto guest = begin(Role::guest);
  ASSERT_TRUE(host);
  ASSERT_TRUE(guest);
  EXPECT_EQ(Bytes::hex(host->share()), text("pA"));
  EXPECT_EQ(Bytes::hex(guest->share()), text("pB"));

  const auto host_share = host->share();
  const auto guest_share = guest->share();
  const auto host_keys = host->complete(crypto, guest_share);
  const auto guest_keys = guest->complete(crypto, host_share);
  ASSERT_TRUE(host_keys);
  ASSERT_TRUE(guest_keys);
  EXPECT_EQ(Bytes::hex(host_keys.value().host), text("cA"));
  EXPECT_EQ(Bytes::hex(host_keys.value().guest), text("cB"));
  EXPECT_EQ(Bytes::hex(guest_keys.value().host), text("cA"));
  EXPECT_EQ(Bytes::hex(guest_keys.value().guest), text("cB"));
  EXPECT_EQ(Bytes::hex(host_keys.value().shared_key), text("Ke"));
  EXPECT_EQ(Bytes::hex(guest_keys.value().shared_key), text("Ke"));

  EXPECT_FALSE(host->complete(crypto, guest_share));
}

TEST_F(PakeExchange, KeepsItsSecretsThroughMoves) {
  auto own_associated = associated;
  auto own_password = password.clone();
  auto ephemeral = host_scalar.clone();
  auto original =
      Exchange::begin(crypto, Role::host, std::move(own_password),
                      std::move(ephemeral), parties, own_associated);
  const auto zero = [](std::byte byte) { return byte == std::byte{}; };
  EXPECT_TRUE(std::ranges::all_of(own_password, zero));
  EXPECT_TRUE(std::ranges::all_of(ephemeral, zero));
  auto guest = Exchange::begin(crypto, Role::guest, password.clone(),
                               guest_scalar.clone(), parties, own_associated);
  ASSERT_TRUE(original);
  ASSERT_TRUE(guest);
  std::ranges::fill(own_associated, std::byte{});
  auto host = std::move(original).value();
  const auto guest_share = guest.value().share();
  EXPECT_FALSE(original.value().complete(crypto, guest_share));
  auto keys = host.complete(crypto, guest_share);
  ASSERT_TRUE(keys);
  EXPECT_EQ(Bytes::hex(keys.value().host), text("cA"));
  auto moved = std::move(keys).value();
  EXPECT_EQ(Bytes::hex(moved.shared_key), text("Ke"));
  EXPECT_TRUE(std::ranges::all_of(keys.value().shared_key, zero));
  moved.shared_key.clear();
  EXPECT_TRUE(std::ranges::all_of(moved.shared_key, zero));

  auto source = Exchange::begin(crypto, Role::host, password.clone(),
                                host_scalar.clone(), parties, associated);
  auto destination =
      Exchange::begin(crypto, Role::host, password.clone(), host_scalar.clone(),
                      parties, own_associated);
  ASSERT_TRUE(source);
  ASSERT_TRUE(destination);
  destination.value() = std::move(source).value();
  EXPECT_FALSE(source.value().complete(crypto, guest_share));
  const auto assigned = destination.value().complete(crypto, guest_share);
  ASSERT_TRUE(assigned);
  EXPECT_EQ(Bytes::hex(assigned.value().host), text("cA"));
}

TEST_F(PakeExchange, ConfirmsOnlyWhenEverythingMatches) {
  EXPECT_TRUE(
      agree(run(password, password, parties, parties, associated, associated)));

  auto wrong_password = password.clone();
  wrong_password[31] ^= std::byte{1};
  EXPECT_FALSE(agree(
      run(password, wrong_password, parties, parties, associated, associated)));

  auto wrong_parties = parties;
  wrong_parties.guest_identity[0] ^= std::byte{1};
  EXPECT_FALSE(agree(
      run(password, password, parties, wrong_parties, associated, associated)));

  auto wrong_exporter = associated;
  wrong_exporter.back() ^= std::byte{1};
  EXPECT_FALSE(agree(
      run(password, password, parties, parties, associated, wrong_exporter)));

  auto wrong_transcript = associated;
  wrong_transcript.front() ^= std::byte{1};
  EXPECT_FALSE(agree(
      run(password, password, parties, parties, wrong_transcript, associated)));
}

TEST_F(PakeExchange, RejectsAShareOffTheCurve) {
  auto off_curve = Bytes::unhex(text("pB"));
  off_curve.back() ^= std::byte{1};
  auto host = begin(Role::host);
  ASSERT_TRUE(host);
  const auto rejected = host->complete(crypto, off_curve);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), Error::pairing_failed);
}

TEST_F(PakeExchange, RejectsACompressedOrMisprefixedShare) {
  const auto guest_share = Bytes::unhex(text("pB"));
  auto compressed = guest_share;
  compressed.resize(33);
  compressed[0] = std::byte{2};
  auto second = begin(Role::host);
  ASSERT_TRUE(second);
  EXPECT_FALSE(second->complete(crypto, compressed));

  auto wrong_prefix = guest_share;
  wrong_prefix[0] = std::byte{6};
  auto third = begin(Role::host);
  ASSERT_TRUE(third);
  EXPECT_FALSE(third->complete(crypto, wrong_prefix));
}

TEST_F(PakeExchange, RejectsTheZeroPoint) {
  std::vector<std::byte> zero(65);
  zero[0] = std::byte{4};
  auto host = begin(Role::host);
  ASSERT_TRUE(host);
  EXPECT_FALSE(host->complete(crypto, zero));
}

TEST_F(PakeExchange, RejectsAShareThatUnblindsToTheIdentity) {
  const auto mask_n = Bytes::unhex(
      "04d8bbd6c639c62937b04d997f38c3770719c629d7014d49a24b4f98baa1292b49"
      "07d60aa6bfade45008a636337f5168c64d9bd36034808cd564490b1e656edbe7");
  Point blinded{};
  ASSERT_TRUE(crypto.p256_multiply(password, mask_n, blinded));
  auto host = begin(Role::host);
  ASSERT_TRUE(host);
  const auto identity = host->complete(crypto, blinded);
  ASSERT_FALSE(identity);
  EXPECT_EQ(identity.error(), Error::pairing_failed);
}

TEST_F(PakeExchange, RefusesAZeroPasswordOrEphemeral) {
  Scalar zero_scalar{};
  EXPECT_FALSE(Exchange::begin(crypto, Role::host, zero_scalar.clone(),
                               host_scalar.clone(), parties, associated));
  EXPECT_FALSE(Exchange::begin(crypto, Role::host, password.clone(),
                               zero_scalar.clone(), parties, associated));
}

TEST(PakeScalarReduction, MapsTheGroupOrderAndItsEdgesIntoRange) {
  std::array<std::byte, wide_scalar_size> wide{};
  EXPECT_TRUE(std::ranges::equal(Exchange::reduce_wide(wide), Scalar{}));
  const auto order = Bytes::unhex_array<32>(
      "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551");
  std::copy(order.begin(), order.end(), wide.begin() + 32);
  EXPECT_TRUE(std::ranges::equal(Exchange::reduce_wide(wide), Scalar{}));
  wide[63] = std::byte{0x52};
  Scalar one{};
  one[31] = std::byte{1};
  EXPECT_TRUE(std::ranges::equal(Exchange::reduce_wide(wide), one));
  wide[63] = std::byte{0x50};
  auto below = order;
  below[31] = std::byte{0x50};
  EXPECT_TRUE(std::ranges::equal(Exchange::reduce_wide(wide), below));
}

TEST(PakeScalarReduction, ReducesTheLargestWideValue) {
  std::array<std::byte, wide_scalar_size> wide{};
  wide.fill(std::byte{0xff});
  EXPECT_EQ(Bytes::hex(Exchange::reduce_wide(wide)),
            "66e12d94f3d956202845b2392b6bec594699799c49bd6fa683244c95be79eea1");
}

}  // namespace
}  // namespace net_kakusan_test
