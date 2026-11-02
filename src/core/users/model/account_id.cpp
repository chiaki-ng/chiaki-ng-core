#include "libchiaking/detail/users/account_id.hpp"

#include "src/core/utils/text/base64.hpp"

namespace libchiaking {

auto AccountId::from_base64(std::string_view text) noexcept
    -> std::optional<AccountId> {
  if (text.empty() || text.size() > base64_limit ||
      text.find('\0') != std::string_view::npos)
    return std::nullopt;
  std::array<std::byte, psn_account_id_size> decoded{};
  const auto size = text::base64::decode_into(text, decoded,
                                              text::base64::Alphabet::standard);
  if (!size || size.value() != decoded.size()) return std::nullopt;
  return from_bytes(decoded);
}

}  // namespace libchiaking
