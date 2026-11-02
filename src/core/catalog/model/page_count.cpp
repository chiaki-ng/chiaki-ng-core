#include "src/core/catalog/model/page_count.hpp"

#include <utility>

namespace libchiaking::catalog {

PageCount::PageCount(Report report) noexcept : report_(std::move(report)) {}

void PageCount::expect(std::uint32_t pages) {
  expected_ = expected_.value_or(0) + pages;
  tell();
}

void PageCount::read(std::uint32_t pages) {
  read_ += pages;
  tell();
}

void PageCount::tell() const {
  if (report_) report_(read_, expected_);
}

}  // namespace libchiaking::catalog
