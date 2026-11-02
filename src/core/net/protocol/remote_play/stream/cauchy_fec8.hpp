/// @file
/// @brief The Cauchy Reed-Solomon erasure decoder over GF(2^8) that recovers
/// lost units of a frame from its parity units.
///
/// A frame of k source units is sent with m parity units. Parity row r is the
/// sum over sources s of `1 / (r XOR (m + s))` times source s, in GF(2^8) with
/// the polynomial x^8 + x^4 + x^3 + x^2 + 1 (0x11d): Jerasure's original
/// Cauchy coding matrix for w = 8. Any k received units recover the frame.
#pragma once

#include "libchiaking/detail/expected.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace libchiaking::net::protocol::remote_play {

/// @brief Why the sources could not be recovered.
enum class CauchyFec8Error : std::uint8_t {
  /// @brief k or m is 0, or k + m exceeds 256.
  invalid_counts,
  /// @brief The slots do not number k + m, or the units differ in size or are
  /// empty.
  invalid_size,
  /// @brief Fewer than k units, or fewer parity units than missing sources.
  insufficient_units,
  /// @brief The equations could not be solved, or Jerasure could not
  /// allocate its matrices.
  singular,
};

/// @brief The Cauchy decoder that solves a frame's lost source units from its
/// parity units with Jerasure over gf-complete.
///
/// It builds Jerasure's original Cauchy coding matrix for the frame's k and m
/// once and keeps it, with the unit pointer arrays, until a frame with other
/// counts arrives. Recovery runs `jerasure_matrix_decode` over pointers into
/// the units themselves, so lost units are written in place. Region products
/// come from gf-complete's default GF(2^8) method, NEON nibble lookups on
/// AArch64 builds. Not thread-safe; each assembler owns one.
class CauchyFec8Decoder {
 public:
  /// @brief Recovers the k source units of a frame.
  ///
  /// Received sources are moved into the result. Lost sources, and lost
  /// parity units, which Jerasure re-encodes, are given fresh buffers that
  /// Jerasure fills in place.
  ///
  /// @param slots The k source slots then the m parity slots; empty optionals
  ///     are lost units. All units must be the same size.
  /// @param k The number of source units.
  /// @param m The number of parity units.
  /// @return The k sources, in order.
  /// @retval CauchyFec8Error::invalid_counts The counts are out of range.
  /// @retval CauchyFec8Error::invalid_size The slots are malformed.
  /// @retval CauchyFec8Error::insufficient_units Too few units arrived.
  /// @retval CauchyFec8Error::singular Jerasure could not decode.
  [[nodiscard]] auto recover(
      std::vector<std::optional<std::vector<std::byte>>> slots, std::size_t k,
      std::size_t m)
      -> Expected<std::vector<std::vector<std::byte>>, CauchyFec8Error>;

 private:
  /// @brief Frees a matrix Jerasure allocated.
  struct FreeMatrix {
    /// @brief Frees the matrix.
    ///
    /// @param matrix The matrix, or null.
    void operator()(int* matrix) const noexcept;
  };

  /// @brief A coding matrix owned by the decoder.
  using CodingMatrix = std::unique_ptr<int, FreeMatrix>;

  /// @brief The number of field elements, which bounds k + m.
  static constexpr std::size_t field_size = 256;
  /// @brief The Jerasure word size: one byte per symbol.
  static constexpr int word_size = 8;

  /// @brief Initialises Jerasure's default GF(2^8) field once per process.
  ///
  /// @return Whether the field is ready.
  static auto field_ready() noexcept -> bool;

  /// @brief Makes the coding matrix and pointer arrays fit k and m.
  ///
  /// @param k The number of source units.
  /// @param m The number of parity units.
  /// @return Whether the matrix could be built.
  auto prepare(std::size_t k, std::size_t m) -> bool;

  /// @brief The coding matrix for matrix_k_ and matrix_m_.
  CodingMatrix matrix_;
  /// @brief The k the matrix was built for.
  std::size_t matrix_k_{};
  /// @brief The m the matrix was built for.
  std::size_t matrix_m_{};
  /// @brief Pointers to the source units handed to Jerasure.
  std::vector<char*> data_;
  /// @brief Pointers to the parity units handed to Jerasure.
  std::vector<char*> coding_;
  /// @brief The lost unit indexes, ended by -1.
  std::vector<int> erasures_;
};

}  // namespace libchiaking::net::protocol::remote_play
