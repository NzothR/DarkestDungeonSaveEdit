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
         core::dson::ValueKind::Integer, "VERIFIED_GAME", true, true,
         "基础资源值已通过游戏内存档验证；写入前按资源索引和原值做冲突检查。", true},
        {"Estate.Resource.Id", "persist.estate.json", "base_root/wallet/{index}/type",
         core::dson::ValueKind::String, "VERIFIED_SAMPLE", false, false,
         "用 type 识别资源；保留存档中的未知资源 ID，不按固定枚举裁剪。"},
    };
    return mappings;
}

const std::vector<CampaignMappingDescriptor>& stage8_campaign_mappings() {
    static const std::vector<CampaignMappingDescriptor> mappings{
        {"Hero.PersistentId", "persist.roster.json", "base_root/heroes/{guid}",
         core::dson::ValueKind::Object, "VERIFIED_GAME", true, true,
         "由程序内置的空白 0 级英雄 DSON 模板生成；职业初始技能与属性从当前生效的原版/mod 内容定义读取。"},
        {"Hero.Name", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/actor/name",
         core::dson::ValueKind::String, "VERIFIED_GAME", true, true,
         "英雄改名已通过游戏内验收；现有英雄和内置模板生成的英雄使用相同的名称字段。", true},
        {"Hero.Class", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/heroClass",
         core::dson::ValueKind::String, "VERIFIED_SAMPLE", false, false,
         "保留原始职业 ID，通过有效内容环境解析名称、来源和资源。"},
        {"Hero.ResolveXp", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/resolveXp",
         core::dson::ValueKind::Integer, "VERIFIED_GAME", true, true,
         "经验值编辑已通过游戏内等级变化验证；不在 Mapping 层硬编码等级门槛。", true},
        {"Hero.Stress", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/m_Stress",
         core::dson::ValueKind::Float, "VERIFIED_SAMPLE", true, false,
         "可设置为有限非负压力值，0 表示清空；待压力 100 游戏验收后再开放普通安全提交。", true},
        {"Hero.AfflictionId", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/affliction_type_id",
         core::dson::ValueKind::String, "VERIFIED_SAMPLE", true, false,
         "只由 SetHeroAfflictionStateOperation 以折磨/非折磨状态写入；候选写回待游戏验收。", false},
        {"Hero.AfflictionSeverity", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/affliction_severity",
         core::dson::ValueKind::Integer, "VERIFIED_SAMPLE", true, false,
         "只由 SetHeroAfflictionStateOperation 与折磨 ID 原子写入；候选写回待游戏验收。", false},
        {"Hero.VirtueId", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/virtue_type_id",
         core::dson::ValueKind::String, "VERIFIED_SAMPLE", true, false,
         "只由 SetHeroAfflictionStateOperation 在设置或清除折磨时清空任务内美德；候选写回待游戏验收。", false},
        {"Hero.CurrentHp", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/actor/current_hp",
         core::dson::ValueKind::Float, "VERIFIED_SAMPLE", false, false, "动态计算状态保持只读。", false},
        {"Hero.WeaponRank", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/weapon_rank",
         core::dson::ValueKind::Integer, "VERIFIED_GAME", true, true,
         "等级字段与武器购买节点必须由 equipment-rank 复合 Operation 原子写入。", true},
        {"Hero.ArmourRank", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/armour_rank",
         core::dson::ValueKind::Integer, "VERIFIED_GAME", true, true,
         "等级字段与防具购买节点必须由 equipment-rank 复合 Operation 原子写入。", true},
        {"Hero.Quirks", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/quirks/{quirkId}",
         core::dson::ValueKind::Object, "VERIFIED_GAME", true, true,
         "增加和替换怪癖已通过游戏测试；插入时需从有效定义和模板构造字段并清理继承元数据。"},
        {"Hero.SelectedCombatSkills", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/skills/selected_combat_skills/{skillId}",
         core::dson::ValueKind::Integer, "VERIFIED_GAME", false, true,
         "保留原始整数，不将其解释成技能等级。"},
        {"Hero.SelectedCampingSkills", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/skills/selected_camping_skills/{skillId}",
         core::dson::ValueKind::Integer, "VERIFIED_GAME", true, true,
         "取消装备生存技能已通过游戏验证；不改变训练营购买状态。", true},
        {"Hero.Trinkets", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/trinkets/items/{memberId}",
         core::dson::ValueKind::Object, "VERIFIED_GAME", true, true,
         "增加英雄装备饰品已通过游戏测试；追加前检查有效饰品定义及英雄职业限制。"},
        {"TrinketInventory.Items", "persist.estate.json", "base_root/trinkets/items/{index}",
         core::dson::ValueKind::Object, "VERIFIED_GAME", true, true,
         "增加库存饰品已通过游戏测试；按有序条目追加，不按饰品 ID 去重。"},
        {"Town.Buildings", "persist.town.json", "base_root/buildings/{buildingId}",
         core::dson::ValueKind::Object, "VERIFIED_SAMPLE", false, false, "状态与升级购买历史分开保留。"},
        {"Town.Districts", "persist.town.json", "base_root/districts/buildings/{districtId}",
         core::dson::ValueKind::Object, "VERIFIED_GAME", true, true,
         "单个小镇建筑的 `built` 状态已通过游戏验证；依赖系统状态已经开放。"},
        {"Town.DistrictSystem", "persist.town.json", "base_root/districts",
         core::dson::ValueKind::Object, "VERIFIED_GAME", true, true,
         "开放/锁定小镇建筑系统已通过游戏验证；状态对象只从有效地区建筑定义生成。"},
        {"Hero.Quirk.Locked", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/quirks/{memberId}/is_locked",
         core::dson::ValueKind::Boolean, "VERIFIED_GAME", true, true,
         "正面且定义允许锁定的怪癖已通过游戏内验证；Operation 校验目标定义与当前状态。", true},
        {"Hero.Quirk.Entry", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/quirks/{memberId}",
         core::dson::ValueKind::Object, "VERIFIED_GAME", true, true,
         "直接移除正面/负面怪癖已通过游戏内验证；删除的是完整怪癖对象及其元数据。", true},
        {"Hero.Trinket.Entry", "persist.roster.json", "base_root/heroes/{guid}/hero_file_data/raw_data => base_root/trinkets/items/{memberId}",
         core::dson::ValueKind::Object, "VERIFIED_GAME", true, true,
         "销毁英雄装备栏中的饰品已通过游戏内验证；该操作不会转入库存。", true},
        {"TrinketInventory.Entry", "persist.estate.json", "base_root/trinkets/items/{memberId}",
         core::dson::ValueKind::Object, "VERIFIED_GAME", true, true,
         "销毁饰品物品栏条目已通过游戏内验证；按原始键定位，不压缩或重排其余条目。", true},
        {"Town.District.Built", "persist.town.json", "base_root/districts/buildings/{memberId}/built",
         core::dson::ValueKind::Boolean, "VERIFIED_GAME", true, true,
         "单个小镇建筑 built 状态的解锁/锁定已通过游戏内验证；要求小镇建筑系统已开放。", true},
        {"Upgrade.PurchaseNode", "persist.upgrades.json", "base_root/purchases/{index}/is_purchased",
         core::dson::ValueKind::Boolean, "VERIFIED_GAME", true, true,
         "购买节点状态随技能、装备和小镇升级测试通过；领域映射同时核对 tree_id、instance_number 与 requirement_code。", true},
        {"Upgrade.PurchaseNode.Entry", "persist.upgrades.json", "base_root/purchases/{index}",
         core::dson::ValueKind::Object, "VERIFIED_GAME", true, true,
         "稀疏存档缺失的购买节点可由安全结构操作克隆同文档模板，并重写实例、树、代码和值。"},
        {"Progression.QuestTotals", "persist.progression.json", "base_root/{total_quests_finished,total_successful_quests_finished}",
         core::dson::ValueKind::Integer, "VERIFIED_SAMPLE", false, false, "作为战役摘要只读统计。"},
    };
    return mappings;
}

const CampaignMappingDescriptor* find_campaign_mapping(std::string_view semantic_property) {
    for (const auto& mapping : stage7_resource_mappings())
        if (mapping.semantic_property == semantic_property) return &mapping;
    for (const auto& mapping : stage8_campaign_mappings())
        if (mapping.semantic_property == semantic_property) return &mapping;
    return nullptr;
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
