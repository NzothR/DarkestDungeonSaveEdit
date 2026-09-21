#include "ddse/application/campaign_mappings.hpp"

#include <charconv>
#include <map>
#include <optional>
#include <string_view>
#include <variant>

namespace ddse::application {
namespace {

struct PartialResource {
    std::optional<std::int32_t> amount;
    std::optional<std::string> id;
};

core::Error mapping_error(std::string message, std::string path) {
    return {core::ErrorCode::ValidationFailed, std::move(message), "CampaignMapping",
            {{"document", "persist.estate.json"}, {"path", std::move(path)}}};
}

} // namespace

const std::vector<CampaignMappingDescriptor>& stage7_resource_mappings() {
    static const std::vector<CampaignMappingDescriptor> mappings{
        {"Estate.Resource.Amount", "persist.estate.json", "base_root/wallet/{index}/amount",
         core::dson::ValueKind::Integer, "VERIFIED_SAMPLE", false, false,
         "存档结构中观察到整数；在游戏内单变量 A/B 确认前不可进入写入流程。"},
        {"Estate.Resource.Id", "persist.estate.json", "base_root/wallet/{index}/type",
         core::dson::ValueKind::String, "VERIFIED_SAMPLE", false, false,
         "用 type 识别资源；保留存档中的未知资源 ID，不按固定枚举裁剪。"},
    };
    return mappings;
}

core::Result<std::vector<CampaignResourceValue>, core::Error>
read_campaign_resources(const core::dson::DsonDocument& estate_document) {
    bool has_wallet = false;
    std::map<std::size_t, PartialResource> partial;
    constexpr std::string_view wallet_prefix{"base_root/wallet/"};
    for (const auto& field : estate_document.fields) {
        if (field.path == "base_root/wallet" && field.kind == core::dson::ValueKind::Object)
            has_wallet = true;
        if (!field.path.starts_with(wallet_prefix)) continue;
        const auto remainder = std::string_view{field.path}.substr(wallet_prefix.size());
        const auto slash = remainder.find('/');
        if (slash == std::string_view::npos) continue;
        std::size_t index = 0;
        const auto parsed = std::from_chars(remainder.data(), remainder.data() + slash, index);
        if (parsed.ec != std::errc{} || parsed.ptr != remainder.data() + slash)
            return core::Result<std::vector<CampaignResourceValue>, core::Error>::failure(
                mapping_error("Wallet entry path has a non-numeric index", field.path));
        const auto member = remainder.substr(slash + 1);
        auto& value = partial[index];
        if (member == "amount") {
            if (field.kind != core::dson::ValueKind::Integer || !std::holds_alternative<std::int32_t>(field.value))
                return core::Result<std::vector<CampaignResourceValue>, core::Error>::failure(
                    mapping_error("Wallet amount is not an integer", field.path));
            value.amount = std::get<std::int32_t>(field.value);
        } else if (member == "type") {
            if (field.kind != core::dson::ValueKind::String || !std::holds_alternative<std::string>(field.value))
                return core::Result<std::vector<CampaignResourceValue>, core::Error>::failure(
                    mapping_error("Wallet type is not a string", field.path));
            value.id = std::get<std::string>(field.value);
        }
    }

    if (!has_wallet)
        return core::Result<std::vector<CampaignResourceValue>, core::Error>::failure(
            mapping_error("Estate document has no wallet object", "base_root/wallet"));
    std::vector<CampaignResourceValue> result;
    result.reserve(partial.size());
    for (const auto& [index, value] : partial) {
        if (!value.amount || !value.id || value.id->empty())
            return core::Result<std::vector<CampaignResourceValue>, core::Error>::failure(
                mapping_error("Wallet entry is missing a valid amount or resource type",
                              "base_root/wallet/" + std::to_string(index)));
        const auto object_path = "base_root/wallet/" + std::to_string(index);
        result.push_back({index, *value.id, *value.amount, object_path});
    }
    return core::Result<std::vector<CampaignResourceValue>, core::Error>::success(std::move(result));
}

} // namespace ddse::application
