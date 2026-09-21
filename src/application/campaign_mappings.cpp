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

const std::vector<CampaignMappingDescriptor>& stage8_campaign_mappings() {
    static const std::vector<CampaignMappingDescriptor> mappings{
        {"Hero.PersistentId", "persist.roster.json", "base_root/heroes/{guid}",
         core::dson::ValueKind::Object, "VERIFIED_SAMPLE", false, false,
         "外层对象键保留为原始标识；创建与删除需处理跨文档引用。"},
        {"Hero.Name", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/actor/name",
         core::dson::ValueKind::String, "VERIFIED_SAMPLE", false, false,
         "从英雄内嵌 DSON 读取；RawLocator 跨越外层与内嵌文档。"},
        {"Hero.Class", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/heroClass",
         core::dson::ValueKind::String, "VERIFIED_SAMPLE", false, false,
         "保留原始职业 ID，通过有效内容环境解析名称、来源和资源。"},
        {"Hero.ResolveXp", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/resolveXp",
         core::dson::ValueKind::Integer, "VERIFIED_SAMPLE", false, false,
         "仅投影原始 XP；未从有效环境取得门槛前不推导等级。"},
        {"Hero.Stress", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/m_Stress",
         core::dson::ValueKind::Float, "VERIFIED_SAMPLE", false, false, "当前阶段只读投影。"},
        {"Hero.CurrentHp", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/actor/current_hp",
         core::dson::ValueKind::Float, "VERIFIED_SAMPLE", false, false, "当前阶段只读投影。"},
        {"Hero.WeaponRank", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/weapon_rank",
         core::dson::ValueKind::Integer, "VERIFIED_SAMPLE", false, false,
         "升级购买历史的同步关系尚未实现。"},
        {"Hero.ArmourRank", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/armour_rank",
         core::dson::ValueKind::Integer, "VERIFIED_SAMPLE", false, false,
         "升级购买历史的同步关系尚未实现。"},
        {"Hero.Quirks", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/quirks/{quirkId}",
         core::dson::ValueKind::Object, "VERIFIED_SAMPLE", false, false,
         "对象键是 Quirk ID；保留锁定、演化等原始元数据。"},
        {"Hero.SelectedCombatSkills", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/skills/selected_combat_skills/{skillId}",
         core::dson::ValueKind::Integer, "VERIFIED_SAMPLE", false, false,
         "保留原始整数，不将其解释成技能等级。"},
        {"Hero.SelectedCampingSkills", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/skills/selected_camping_skills/{skillId}",
         core::dson::ValueKind::Integer, "VERIFIED_SAMPLE", false, false,
         "保留原始整数，不将其解释成技能等级。"},
        {"Hero.Trinkets", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/trinkets/items",
         core::dson::ValueKind::Object, "VERIFIED_SAMPLE", false, false,
         "保留原始有序条目和未知字段。"},
        {"TrinketInventory.Items", "persist.estate.json", "base_root/trinkets/items/{index}",
         core::dson::ValueKind::Object, "VERIFIED_SAMPLE", false, false,
         "按有序条目投影，不按饰品 ID 去重。"},
        {"Town.Buildings", "persist.town.json", "base_root/buildings/{buildingId}",
         core::dson::ValueKind::Object, "VERIFIED_SAMPLE", false, false, "状态与升级购买历史分开保留。"},
        {"Town.Districts", "persist.town.json", "base_root/districts/buildings/{districtId}",
         core::dson::ValueKind::Object, "VERIFIED_SAMPLE", false, false,
         "若当前存档没有此集合则显示为空；不从旧样本数量推断。"},
        {"Progression.QuestTotals", "persist.progression.json", "base_root/{total_quests_finished,total_successful_quests_finished}",
         core::dson::ValueKind::Integer, "VERIFIED_SAMPLE", false, false, "作为战役摘要只读统计。"},
    };
    return mappings;
}

core::Result<std::vector<CampaignResourceValue>, core::Error>
read_campaign_resources(const core::dson::DsonDocument& estate_document) {
    auto scan = scan_campaign_resources(estate_document);
    if (!scan.wallet_found)
        return core::Result<std::vector<CampaignResourceValue>, core::Error>::failure(
            mapping_error("Estate document has no wallet object", "base_root/wallet"));
    if (!scan.issues.empty())
        return core::Result<std::vector<CampaignResourceValue>, core::Error>::failure(
            mapping_error(scan.issues.front().message, scan.issues.front().path));
    std::vector<CampaignResourceValue> result;
    result.reserve(scan.entries.size());
    for (const auto& entry : scan.entries) {
        if (!entry.id || !entry.amount)
            return core::Result<std::vector<CampaignResourceValue>, core::Error>::failure(
                mapping_error("Wallet entry is missing a valid amount or resource type", entry.raw_object_path));
        result.push_back({entry.wallet_index, *entry.id, *entry.amount, entry.raw_object_path});
    }
    return core::Result<std::vector<CampaignResourceValue>, core::Error>::success(std::move(result));
}

CampaignResourceScan scan_campaign_resources(const core::dson::DsonDocument& estate_document) {
    CampaignResourceScan result;
    bool has_wallet = false;
    std::map<std::size_t, PartialResource> partial;
    constexpr std::string_view wallet_prefix{"base_root/wallet/"};
    for (const auto& field : estate_document.fields) {
        if (field.path == "base_root/wallet" && field.kind == core::dson::ValueKind::Object)
            has_wallet = true;
        if (!field.path.starts_with(wallet_prefix)) continue;
        const auto remainder = std::string_view{field.path}.substr(wallet_prefix.size());
        const auto slash = remainder.find('/');
        const auto index_text = slash == std::string_view::npos ? remainder : remainder.substr(0, slash);
        std::size_t index = 0;
        const auto parsed = std::from_chars(index_text.data(), index_text.data() + index_text.size(), index);
        if (parsed.ec != std::errc{} || parsed.ptr != index_text.data() + index_text.size()) {
            result.issues.push_back({field.path, "Wallet entry path has a non-numeric index"});
            continue;
        }
        if (slash == std::string_view::npos) {
            if (field.kind == core::dson::ValueKind::Object) partial.try_emplace(index);
            continue;
        }
        const auto member = remainder.substr(slash + 1);
        auto& value = partial[index];
        if (member == "amount") {
            if (field.kind != core::dson::ValueKind::Integer || !std::holds_alternative<std::int32_t>(field.value)) {
                result.issues.push_back({field.path, "Wallet amount is not an integer"});
                continue;
            }
            value.amount = std::get<std::int32_t>(field.value);
        } else if (member == "type") {
            if (field.kind != core::dson::ValueKind::String || !std::holds_alternative<std::string>(field.value)) {
                result.issues.push_back({field.path, "Wallet type is not a string"});
                continue;
            }
            value.id = std::get<std::string>(field.value);
        }
    }

    result.wallet_found = has_wallet;
    if (!has_wallet) {
        result.issues.push_back({"base_root/wallet", "Estate document has no wallet object"});
        return result;
    }
    result.entries.reserve(partial.size());
    for (const auto& [index, value] : partial) {
        const auto object_path = "base_root/wallet/" + std::to_string(index);
        if (!value.amount || !value.id || value.id->empty())
            result.issues.push_back({object_path, "Wallet entry is missing a valid amount or resource type"});
        result.entries.push_back({index, value.id, value.amount, object_path});
    }
    return result;
}

} // namespace ddse::application
