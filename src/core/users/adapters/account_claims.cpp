#include "src/core/users/adapters/account_claims.hpp"

#include "src/core/utils/json/ps_fields.hpp"
#include "src/core/utils/json/ps_reply.hpp"
#include "src/core/utils/text/base64.hpp"

#include <libchiaking/detail/secret_string.hpp>

#include <span>

template <>
struct glz::meta<libchiaking::users::AccountContext::Stated> {
  using T = libchiaking::users::AccountContext::Stated;
  static constexpr auto value = glz::object(
      "legal_country", libchiaking::json::ps::lenient(&T::legal_country),
      "locale", libchiaking::json::ps::lenient(&T::locale), "age",
      libchiaking::json::ps::lenient(&T::age), "is_child",
      libchiaking::json::ps::lenient(&T::is_child), "iat",
      libchiaking::json::ps::lenient_epoch_seconds(&T::issued_at));
};

namespace libchiaking::users {

auto AccountClaims::read(std::string_view token) -> AccountContext {
  if (token.size() > token_limit) return {};
  const auto first = token.find('.');
  if (first == std::string_view::npos) return {};
  const auto second = token.find('.', first + 1);
  if (second == std::string_view::npos) return {};
  const auto encoded = token.substr(first + 1, second - first - 1);
  const auto size = text::base64::url_decoded_size(encoded.size());
  if (!size || *size == 0) return {};
  auto payload = SecretBytes::zeroed(*size);
  if (!text::base64::decode_url_into(
          encoded, std::span<std::byte>{payload.begin(), payload.end()}))
    return {};
  const auto stated =
      json::ps::read_reply<AccountContext::Stated>(std::string_view{
          reinterpret_cast<const char*>(payload.data()), payload.size()});
  if (!stated) return {};
  return AccountContext::stating(*stated);
}

}  // namespace libchiaking::users
