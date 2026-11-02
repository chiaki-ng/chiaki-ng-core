#include "src/core/psn/rest/entitlement/library_entitlements.hpp"

#include "src/core/net/transport/http/percent_encoding.hpp"
#include "src/core/net/transport/http/status.hpp"
#include "src/core/net/transport/http/url.hpp"
#include "src/core/utils/json/ps_reply.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <variant>

namespace libchiaking::psn::rest {

struct LibraryEntitlements::Reply {
  using Stated = std::optional<glz::raw_json>;

  struct ConceptMeta {
    using keys = json::ps::CamelCaseKeys;
    json::ps::Tolerant<std::variant<std::string, std::uint64_t>> concept_id;
    json::ps::Tolerant<std::string> name;
    json::ps::Tolerant<std::string> icon_url;
  };
  struct GameMeta {
    using keys = json::ps::CamelCaseKeys;
    json::ps::Tolerant<std::string> package_type;
    json::ps::Tolerant<std::string> name;
    json::ps::Tolerant<std::string> icon_url;
    json::ps::Tolerant<bool> is_play_time_fully_consumed;
  };
  struct TitleMeta {
    using keys = json::ps::CamelCaseKeys;
    json::ps::Tolerant<std::string> title_id;
    json::ps::Tolerant<std::string> name;
    json::ps::Tolerant<std::string> image_url;
  };
  struct ProductMeta {
    using keys = json::ps::CamelCaseKeys;
    json::ps::Tolerant<std::string> image_url;
  };
  struct SkuMeta {
    using keys = json::ps::CamelCaseKeys;
    Stated playability_date;
  };
  struct Entitlement {
    using keys = json::ps::CamelCaseKeys;
    json::ps::Tolerant<std::string> id;
    json::ps::Tolerant<std::string> product_id;
    json::ps::Tolerant<std::string> sku_id;
    json::ps::Tolerant<bool> active_flag;
    Stated active_date;
    json::ps::Tolerant<std::int64_t> entitlement_type;
    json::ps::Tolerant<std::int64_t> feature_type;
    json::ps::Tolerant<bool> preorder_flag;
    json::ps::Tolerant<bool> preorder_core_flag;
    json::ps::Tolerant<bool> preorder_placeholder_flag;
    json::ps::Tolerant<ConceptMeta> concept_meta;
    json::ps::Tolerant<GameMeta> game_meta;
    json::ps::Tolerant<TitleMeta> title_meta;
    json::ps::Tolerant<ProductMeta> product_meta;
    json::ps::Tolerant<SkuMeta> sku_meta;
    json::ps::Tolerant<bool> streaming_supported;
    json::ps::Tolerant<bool> is_play_time_fully_consumed;

    auto play_time_used_up() const -> std::optional<bool> {
      if (const auto stated = flag(is_play_time_fully_consumed)) return stated;
      return game_meta ? flag(game_meta->is_play_time_fully_consumed)
                       : std::nullopt;
    }
    auto concept_text() const -> std::string {
      if (!concept_meta || !concept_meta->concept_id) return {};
      if (const auto* number =
              std::get_if<std::uint64_t>(&*concept_meta->concept_id))
        return std::to_string(*number);
      const auto& digits = std::get<std::string>(*concept_meta->concept_id);
      return std::ranges::all_of(digits, text::ascii_digit) ? digits
                                                            : std::string{};
    }
    auto identified() const noexcept -> bool {
      return !id.value_or("").empty() || !product_id.value_or("").empty();
    }
    auto game() const -> std::optional<GameEntitlement> {
      auto named = id.value_or("");
      if (named.empty()) return std::nullopt;
      return GameEntitlement{
          .id = std::move(named),
          .product_id = product_id.value_or(""),
          .sku_id = sku_id.value_or(""),
          .active = flag(active_flag),
          .active_date = text_of(active_date),
          .entitlement_type = number(entitlement_type),
          .feature_type = number(feature_type),
          .preorder = flag(preorder_flag),
          .preorder_core = flag(preorder_core_flag),
          .preorder_placeholder = flag(preorder_placeholder_flag),
          .streaming_supported = flag(streaming_supported),
          .play_time_used_up = play_time_used_up(),
          .playability_date =
              sku_meta ? text_of(sku_meta->playability_date) : std::nullopt,
          .package_type = game_meta ? game_meta->package_type.value_or("") : "",
          .game_name = game_meta ? game_meta->name.value_or("") : "",
          .game_icon_url = game_meta ? game_meta->icon_url.value_or("") : "",
          .title_id = title_meta ? title_meta->title_id.value_or("") : "",
          .title_name = title_meta ? title_meta->name.value_or("") : "",
          .title_image_url =
              title_meta ? title_meta->image_url.value_or("") : "",
          .concept_id = concept_text(),
          .concept_name = concept_meta ? concept_meta->name.value_or("") : "",
          .concept_icon_url =
              concept_meta ? concept_meta->icon_url.value_or("") : "",
          .product_image_url =
              product_meta ? product_meta->image_url.value_or("") : ""};
    }

    static auto flag(const json::ps::Tolerant<bool>& stated)
        -> std::optional<bool> {
      return stated ? std::optional{*stated} : std::nullopt;
    }
    static auto number(const json::ps::Tolerant<std::int64_t>& stated)
        -> std::optional<std::int64_t> {
      return stated ? std::optional{*stated} : std::nullopt;
    }
    static auto text_of(const Stated& stated) -> std::optional<std::string> {
      if (!stated) return std::nullopt;
      std::string words;
      if (stated->str.starts_with('"') &&
          !glz::read<json::ps::reply_options>(words, stated->str))
        return words;
      return std::string{};
    }
  };
  using keys = json::ps::CamelCaseKeys;
  std::optional<std::vector<json::ps::Tolerant<Entitlement>>> entitlements;
  std::optional<std::uint64_t> total_results;
  Stated revision_id;

  auto revision() const -> std::optional<std::string> {
    if (!revision_id) return std::nullopt;
    return revision_id->str;
  }
};

struct LibraryEntitlements::Refusal {
  using Code = json::ps::Tolerant<std::variant<std::int64_t, std::string>>;
  struct Detail {
    using keys = json::ps::CamelCaseKeys;
    Code code;
  };
  using keys = json::ps::CamelCaseKeys;
  json::ps::Tolerant<Detail> error;
  Code code;

  auto denies_ownership() const -> bool {
    const auto& stated = error ? error->code : code;
    if (!stated) return false;
    if (const auto* number = std::get_if<std::int64_t>(&*stated))
      return *number == not_owned_code;
    return std::get<std::string>(*stated) == std::to_string(not_owned_code);
  }

  static auto of(const net::http::Response& response) -> bool {
    const auto read = json::ps::read_reply<Refusal>(response.body());
    return read && read->denies_ownership();
  }
};

LibraryEntitlements::LibraryEntitlements(net::http::Client& client,
                                         auth::BearerToken token) noexcept
    : client_(client), token_(std::move(token)) {}

auto LibraryEntitlements::request(Asked asked, std::string_view package_type,
                                  std::size_t offset) const
    -> net::http::Request {
  net::http::Query url{entitlements_url};
  if (asked == Asked::games) {
    url.add_verbatim("isActive", "true")
        .add("gameMetaPackageType", package_type)
        .add_verbatim("sortBy", "ACTIVE_DATE")
        .add_verbatim("sortDirection", "asc")
        .add_verbatim("limit", std::to_string(games_page_size))
        .add_verbatim("offset", std::to_string(offset))
        .add_verbatim("useCache", "true")
        .add_verbatim("fields", games_fields);
  } else {
    url.add_verbatim("isActive", "true")
        .add("gameMetaPackageType", package_type)
        .add_verbatim("limit", std::to_string(page_size))
        .add_verbatim("offset", std::to_string(offset))
        .add_verbatim("fields", wanted_fields);
  }
  net::http::Request built{
      asked == Asked::games ? games_endpoint : concepts_endpoint,
      std::move(url)};
  built.authorize(token_).header("Accept", "application/json");
  return built;
}

template <class Take>
auto LibraryEntitlements::walk(Asked asked, std::string_view package_type,
                               Take&& take, const LibraryPageObserver& observe)
    -> Expected<Walk, WalkError> {
  Walk walked;
  const std::size_t rows = asked == Asked::games ? games_page_size : page_size;
  bool first = true;
  const auto consume =
      [&](const net::http::Response& response) -> Expected<int, WalkError> {
    auto reply = WalkError::checked_reply<Reply>(response, token_.view());
    if (!reply) return unexpected(std::move(reply.error()));
    if (!reply->entitlements || !reply->total_results)
      return unexpected(WalkError{bad_reply::InvalidSchema{}});
    if (!take(*reply)) return 0;
    walked.total = static_cast<std::size_t>(*reply->total_results);
    walked.received += reply->entitlements->size();
    if (observe)
      observe(first ? std::optional{(walked.total + rows - 1) / rows}
                    : std::nullopt);
    first = false;
    const bool finished = walked.received >= walked.total;
    return finished ? 0 : static_cast<int>(reply->entitlements->size());
  };
  auto paged = client_.pages(
      net::http::Paging{fewest_rows_to_continue, static_cast<int>(page_limit),
                        page_delay},
      [&](int offset) {
        return request(asked, package_type, static_cast<std::size_t>(offset));
      },
      consume);
  if (!paged) {
    if (auto* failure = std::get_if<net::http::Failure>(&paged.error().cause))
      return unexpected(WalkError{*failure});
    return unexpected(std::move(std::get<WalkError>(paged.error().cause)));
  }
  walked.requests = paged->requests;
  walked.truncated = paged->truncated;
  return walked;
}

auto LibraryEntitlements::concepts_of(std::string_view package_type,
                                      const LibraryPageObserver& observe)
    -> Expected<EntitlementConcepts, ConceptsError> {
  EntitlementConcepts all;
  auto walked = walk(
      Asked::concepts, package_type,
      [&](Reply& reply) {
        for (auto& entitlement : *reply.entitlements) {
          if (!entitlement) continue;
          auto concept_id = entitlement->concept_text();
          if (concept_id.empty() || !entitlement->identified()) continue;
          all.known.push_back(EntitlementConcept{
              std::move(entitlement->id).value_or(""),
              std::move(entitlement->product_id).value_or(""),
              std::move(concept_id)});
        }
        return true;
      },
      observe);
  if (!walked) return unexpected(std::move(walked.error()));
  all.total = walked->total;
  all.received = walked->received;
  all.requests = walked->requests;
  all.truncated = walked->truncated;
  return all;
}

auto LibraryEntitlements::games_pass(GameEntitlements& all,
                                     const LibraryPageObserver& observe)
    -> Expected<bool, GamesPassError> {
  std::optional<std::string> revision;
  bool changed = false;
  auto walked = walk(
      Asked::games, ps5_games,
      [&](const Reply& reply) {
        const auto seen = reply.revision();
        if (revision && seen && *seen != *revision) {
          changed = true;
          return false;
        }
        if (!revision) revision = seen;
        for (const auto& entitlement : *reply.entitlements)
          if (entitlement)
            if (auto game = entitlement->game())
              all.entitlements.push_back(std::move(*game));
        return true;
      },
      observe);
  if (!walked) return unexpected(std::move(walked.error()));
  all.total = walked->total;
  all.received = walked->received;
  all.requests += walked->requests;
  all.truncated = walked->truncated;
  return !changed;
}

auto LibraryEntitlements::owned_ps5_games(const LibraryPageObserver& observe)
    -> Expected<GameEntitlements, OwnedGamesError> {
  GameEntitlements all;
  for (;;) {
    auto settled = games_pass(all, observe);
    if (!settled) return unexpected(std::move(settled.error()));
    if (settled.value()) return all;
    if (all.revision_restarts >= revision_restart_limit) {
      all.revision_unsettled = true;
      return all;
    }
    ++all.revision_restarts;
    all.entitlements.clear();
    if (!client_.wait(page_delay))
      return unexpected(OwnedGamesError{net::http::Failure::cancelled()});
  }
}

auto LibraryEntitlements::owned_ps5_game(std::string_view id)
    -> Expected<std::optional<GameEntitlement>, OwnedGameError> {
  net::http::Query url{std::format("{}/{}", entitlements_url,
                                   net::http::PercentEncoding::component(id))};
  url.add_verbatim("fields", game_fields);
  net::http::Request request{game_endpoint, std::move(url)};
  request.authorize(token_).header("Accept", "application/json");
  auto sent = client_.send(std::move(request));
  if (sent) {
    const auto status = sent.value().status();
    if (net::HttpStatus{status} == net::HttpStatus::not_found ||
        (!net::HttpStatus{status}.is_success() && Refusal::of(sent.value())))
      return std::optional<GameEntitlement>{};
  }
  auto reply = OwnedGameError::checked_reply<Reply::Entitlement>(
      std::move(sent), token_.view());
  if (!reply) return unexpected(std::move(reply.error()));
  auto game = reply.value().game();
  if (!game) return unexpected(OwnedGameError{bad_reply::InvalidSchema{}});
  return std::optional<GameEntitlement>{std::move(*game)};
}

}  // namespace libchiaking::psn::rest
