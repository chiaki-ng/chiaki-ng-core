#include "src/core/psn/graphql/authenticated/operations.hpp"
#include "src/core/psn/graphql/public_web/operations.hpp"
#include "src/core/utils/text/hex.hpp"

#include <libchiaking/platform/crypto/openssl/provider.hpp>
#include <libchiaking/platform/crypto/provider.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <span>
#include <string>
#include <string_view>

namespace psn_test {
namespace {

namespace graphql = libchiaking::psn::graphql;

class GraphqlQueryDocuments : public ::testing::Test {
 protected:
  static auto documents() -> std::filesystem::path {
    return std::filesystem::path{LIBCHIAKING_GRAPHQL_DIR};
  }

  static auto read_bytes(const std::filesystem::path& path) -> std::string {
    std::ifstream file{path, std::ios::binary};
    if (!file.is_open()) {
      ADD_FAILURE() << "cannot open " << path;
      return {};
    }
    return {std::istreambuf_iterator<char>{file},
            std::istreambuf_iterator<char>{}};
  }

  static auto sha256_hex(std::string_view bytes) -> std::string {
    libchiaking::platform::openssl::OpenSslCryptoProvider crypto;
    std::array<std::byte, libchiaking::platform::sha256_digest_size> digest{};
    if (!crypto.sha256(std::as_bytes(std::span{bytes}), digest)) {
      ADD_FAILURE() << "SHA-256 failed";
      return {};
    }
    std::string text;
    libchiaking::text::append_hex(text, digest);
    return text;
  }

  static auto document(std::string_view queries,
                       const graphql::Operation& operation)
      -> std::filesystem::path {
    return documents() / std::filesystem::path{std::string{queries}} /
           (std::string{operation.name} + ".gql");
  }

  template <class Operations>
  static void expect_documents_hash_to(std::string_view queries,
                                       const Operations& operations) {
    std::set<std::string> named;
    for (const auto& operation : operations) {
      SCOPED_TRACE(operation.name);
      const auto bytes = read_bytes(document(queries, operation));
      EXPECT_FALSE(bytes.empty());
      EXPECT_EQ(sha256_hex(bytes), operation.sha256);
      named.insert(std::string{operation.name} + ".gql");
    }
    std::set<std::string> present;
    for (const auto& entry : std::filesystem::directory_iterator{
             documents() / std::filesystem::path{std::string{queries}}})
      if (entry.path().extension() == ".gql")
        present.insert(entry.path().filename().string());
    EXPECT_EQ(present, named);
  }
};

TEST_F(GraphqlQueryDocuments, HashingDetectsAChangedDocument) {
  const auto& operation = graphql::public_web::category_grid_retrieve;
  auto bytes = read_bytes(document(graphql::public_web::queries, operation));
  EXPECT_EQ(sha256_hex(bytes), operation.sha256);
  bytes.push_back(' ');
  EXPECT_NE(sha256_hex(bytes), operation.sha256);
  EXPECT_EQ(sha256_hex("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_F(GraphqlQueryDocuments, OnTheWebHashToTheirOperations) {
  expect_documents_hash_to(graphql::public_web::queries,
                           graphql::public_web::operations);
}

TEST_F(GraphqlQueryDocuments, ThatAreAuthenticatedHashToTheirOperations) {
  expect_documents_hash_to(graphql::authenticated::queries,
                           graphql::authenticated::operations);
}

}  // namespace
}  // namespace psn_test
