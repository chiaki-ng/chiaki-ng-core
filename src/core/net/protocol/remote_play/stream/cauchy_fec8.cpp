#include "src/core/net/protocol/remote_play/stream/cauchy_fec8.hpp"

#include <cauchy.h>
#include <galois.h>
#include <jerasure.h>

#include <cstdlib>
#include <utility>

namespace libchiaking::net::protocol::remote_play {

static_assert(__STDCPP_DEFAULT_NEW_ALIGNMENT__ >= 16);

void CauchyFec8Decoder::FreeMatrix::operator()(int* matrix) const noexcept {
  std::free(matrix);
}

auto CauchyFec8Decoder::field_ready() noexcept -> bool {
  static const bool ready = galois_init_default_field(word_size) == 0;
  return ready;
}

auto CauchyFec8Decoder::prepare(std::size_t k, std::size_t m) -> bool {
  if (!field_ready()) return false;
  if (!matrix_ || matrix_k_ != k || matrix_m_ != m) {
    matrix_.reset(cauchy_original_coding_matrix(
        static_cast<int>(k), static_cast<int>(m), word_size));
    if (!matrix_) return false;
    matrix_k_ = k;
    matrix_m_ = m;
  }
  data_.resize(k);
  coding_.resize(m);
  return true;
}

auto CauchyFec8Decoder::recover(
    std::vector<std::optional<std::vector<std::byte>>> slots, std::size_t k,
    std::size_t m)
    -> Expected<std::vector<std::vector<std::byte>>, CauchyFec8Error> {
  if (k == 0 || m == 0 || k > field_size || m > field_size ||
      k + m > field_size)
    return unexpected(CauchyFec8Error::invalid_counts);
  if (slots.size() != k + m) return unexpected(CauchyFec8Error::invalid_size);

  std::size_t unit_size = 0;
  std::size_t received = 0;
  for (const auto& slot : slots) {
    if (!slot) continue;
    if (slot->empty() || (unit_size != 0 && slot->size() != unit_size))
      return unexpected(CauchyFec8Error::invalid_size);
    unit_size = slot->size();
    ++received;
  }
  if (received < k) return unexpected(CauchyFec8Error::insufficient_units);

  std::size_t missing_sources = 0;
  std::size_t parity_received = 0;
  for (std::size_t source = 0; source < k; ++source)
    missing_sources += slots[source] ? 0U : 1U;
  for (std::size_t row = 0; row < m; ++row)
    parity_received += slots[k + row] ? 1U : 0U;

  if (missing_sources != 0) {
    if (parity_received < missing_sources)
      return unexpected(CauchyFec8Error::insufficient_units);
    if (!prepare(k, m)) return unexpected(CauchyFec8Error::singular);
    erasures_.clear();
    for (std::size_t index = 0; index < k + m; ++index) {
      if (slots[index]) continue;
      erasures_.push_back(static_cast<int>(index));
      slots[index].emplace(unit_size);
    }
    erasures_.push_back(-1);
    for (std::size_t source = 0; source < k; ++source)
      data_[source] = reinterpret_cast<char*>(slots[source]->data());
    for (std::size_t row = 0; row < m; ++row)
      coding_[row] = reinterpret_cast<char*>(slots[k + row]->data());
    if (jerasure_matrix_decode(static_cast<int>(k), static_cast<int>(m),
                               word_size, matrix_.get(), 0, erasures_.data(),
                               data_.data(), coding_.data(),
                               static_cast<int>(unit_size)) < 0)
      return unexpected(CauchyFec8Error::singular);
  }

  std::vector<std::vector<std::byte>> sources;
  sources.reserve(k);
  for (std::size_t source = 0; source < k; ++source)
    sources.push_back(std::move(*slots[source]));
  return sources;
}

}  // namespace libchiaking::net::protocol::remote_play
