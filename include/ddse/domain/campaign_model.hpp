#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ddse::domain {

enum class ModelState { Complete, Partial, Invalid };
enum class EntityState { Resolved, Partial, Unresolved, Invalid };
enum class DiagnosticSeverity { Info, Warning, Error };
enum class QuirkPolarity { Positive, Negative, Unknown };

struct RawLocatorStep {
    std::size_t field_index{};
    std::string field_name;
    bool enters_embedded_document{};
};

// Indices refer to ordered DSON fields, including document boundaries at embedded DSON values.
struct RawLocator {
    std::string document_id;
    std::vector<RawLocatorStep> steps;
    std::string display_path;
};

template <typename T>
struct LocatedValue {
    std::optional<T> value;
    std::optional<RawLocator> raw;
};

struct AssetReference {
    std::string role;
    std::string virtual_path;
    std::string source_id;
    bool resolved{};
};

struct DefinitionReference {
    std::string raw_id;
    EntityState state{EntityState::Unresolved};
    std::string display_name;
    std::string source_id;
    std::string layer_type;
    std::string virtual_path;
    // Retained as an opaque rule snapshot until typed rule models are introduced.
    std::string definition_payload_json;
    std::vector<AssetReference> assets;
};

struct SemanticDiagnostic {
    DiagnosticSeverity severity{DiagnosticSeverity::Warning};
    std::string code;
    std::string message;
    std::string document_id;
    std::string raw_path;
    std::string entity_id;
};

struct CampaignResource {
    std::size_t index{};
    bool read_only{true};
    LocatedValue<std::string> id;
    LocatedValue<std::int32_t> amount;
    DefinitionReference definition;
    EntityState state{EntityState::Partial};
};

struct HeroQuirk {
    std::string id;
    bool is_disease{};
    bool read_only{true};
    EntityState state{EntityState::Partial};
    QuirkPolarity polarity{QuirkPolarity::Unknown};
    LocatedValue<bool> is_new;
    LocatedValue<bool> is_locked;
    LocatedValue<std::int32_t> trinket_id;
    LocatedValue<std::int32_t> mission_count;
    LocatedValue<std::int32_t> replaces_quirk;
    LocatedValue<bool> replaces_quirk_viewed;
    LocatedValue<std::int32_t> evolution_duration_remaining;
    RawLocator raw;
    DefinitionReference definition;
};

struct HeroSkillSelection {
    std::string id;
    bool camping{};
    bool read_only{true};
    RawLocator raw;
    // The stored integer is preserved without assuming that it means rank or selected state.
    LocatedValue<std::int32_t> raw_value;
    DefinitionReference definition;
};

struct HeroTrinket {
    std::string id;
    bool read_only{true};
    RawLocator raw;
    DefinitionReference definition;
};

struct Hero {
    std::string persistent_id;
    std::size_t roster_position{};
    EntityState state{EntityState::Partial};
    bool read_only{true};
    RawLocator raw;
    LocatedValue<std::string> name;
    LocatedValue<std::string> class_id;
    DefinitionReference definition;
    LocatedValue<std::int32_t> resolve_xp;
    // Level stays empty until an effective progression rule is available.
    LocatedValue<std::int32_t> level;
    LocatedValue<float> stress;
    LocatedValue<float> current_hp;
    LocatedValue<std::int32_t> weapon_rank;
    LocatedValue<std::int32_t> armour_rank;
    LocatedValue<std::string> affliction_id;
    LocatedValue<std::int32_t> affliction_severity;
    LocatedValue<std::string> virtue_id;
    std::vector<HeroQuirk> quirks;
    std::vector<HeroSkillSelection> combat_skills;
    std::vector<HeroSkillSelection> camping_skills;
    std::vector<HeroTrinket> trinkets;
    std::vector<SemanticDiagnostic> diagnostics;
};

struct TrinketInventoryEntry {
    std::size_t index{};
    std::string raw_key;
    bool read_only{true};
    EntityState state{EntityState::Partial};
    RawLocator raw;
    LocatedValue<std::string> id;
    LocatedValue<std::string> item_type;
    LocatedValue<std::int32_t> amount;
    DefinitionReference definition;
};

struct TownBuilding {
    std::string id;
    bool read_only{true};
    RawLocator raw;
    DefinitionReference definition;
};

struct DistrictState {
    std::string id;
    bool read_only{true};
    RawLocator raw;
    LocatedValue<bool> built;
    DefinitionReference definition;
};

// Purchase-history rows are the authoritative progression state for combat,
// camping, equipment, and town upgrades. Keep the row's original locator so
// Operations can edit the exact node without guessing from displayed ranks.
struct UpgradePurchaseNode {
    std::size_t index{};
    std::int32_t instance_number{};
    std::int32_t tree_id{};
    char requirement_code{};
    RawLocator row_raw;
    LocatedValue<bool> is_purchased;
};

struct ProgressionSummary {
    LocatedValue<std::int32_t> total_recruited_heroes;
    LocatedValue<std::int32_t> total_quests_finished;
    LocatedValue<std::int32_t> total_successful_quests_finished;
    LocatedValue<std::string> last_quest_played_id;
};

struct CampaignSummary {
    std::string profile_id;
    LocatedValue<std::string> estate_name;
    std::size_t document_count{};
    std::size_t hero_count{};
    std::size_t resolved_hero_count{};
    std::size_t unresolved_hero_count{};
    std::size_t resource_count{};
    std::size_t trinket_inventory_count{};
    std::size_t town_building_count{};
    std::size_t district_count{};
    std::size_t diagnostic_count{};
};

struct CampaignModel {
    ModelState state{ModelState::Partial};
    // The save projection is not directly persisted; edit sessions stage changes in a private copy.
    bool read_only{true};
    CampaignSummary summary;
    ProgressionSummary progression;
    std::vector<CampaignResource> resources;
    std::vector<Hero> heroes;
    std::vector<TrinketInventoryEntry> trinket_inventory;
    std::vector<TownBuilding> town_buildings;
    bool district_system_open{};
    std::vector<DistrictState> districts;
    std::vector<UpgradePurchaseNode> upgrade_purchase_nodes;
    std::vector<SemanticDiagnostic> diagnostics;
};

} // namespace ddse::domain
