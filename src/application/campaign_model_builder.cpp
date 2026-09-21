#include "ddse/application/campaign_model_builder.hpp"

#include "ddse/application/campaign_mappings.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <map>
#include <string_view>
#include <utility>
#include <variant>

namespace ddse::application {
namespace {

using core::dson::DsonDocument;
using core::dson::DsonField;
using domain::CampaignModel;
using domain::CampaignResource;
using domain::DistrictState;
using domain::DefinitionReference;
using domain::DiagnosticSeverity;
using domain::EntityState;
using domain::Hero;
using domain::HeroQuirk;
using domain::HeroSkillSelection;
using domain::HeroTrinket;
using domain::LocatedValue;
using domain::ModelState;
using domain::RawLocator;
using domain::RawLocatorStep;
using domain::SemanticDiagnostic;
using domain::TownBuilding;
using domain::TrinketInventoryEntry;
using domain::UpgradePurchaseNode;

struct FieldView {
    const DsonDocument* document{};
    std::size_t index{};
    std::vector<RawLocatorStep> steps;
    std::string display_path;

    [[nodiscard]] const DsonField& field() const { return document->fields[index]; }
    [[nodiscard]] RawLocator locator(std::string_view document_id) const {
        return {std::string{document_id}, steps, display_path};
    }
};

std::optional<FieldView> root_view(const DsonDocument& document) {
    if (document.root_fields.empty()) return std::nullopt;
    const auto index = document.root_fields.front();
    const auto& field = document.fields[index];
    return FieldView{&document, index, {{index, field.name, field.kind == core::dson::ValueKind::EmbeddedDson}},
                     field.path};
}

std::optional<FieldView> child_named(const FieldView& parent, std::string_view name) {
    for (const auto index : parent.field().children) {
        const auto& child = parent.document->fields[index];
        if (child.name != name) continue;
        auto steps = parent.steps;
        steps.push_back({index, child.name, child.kind == core::dson::ValueKind::EmbeddedDson});
        return FieldView{parent.document, index, std::move(steps), parent.display_path + "/" + child.name};
    }
    return std::nullopt;
}

std::vector<FieldView> child_fields(const FieldView& parent) {
    std::vector<FieldView> result;
    result.reserve(parent.field().children.size());
    for (const auto index : parent.field().children) {
        const auto& child = parent.document->fields[index];
        auto steps = parent.steps;
        steps.push_back({index, child.name, child.kind == core::dson::ValueKind::EmbeddedDson});
        result.push_back({parent.document, index, std::move(steps), parent.display_path + "/" + child.name});
    }
    return result;
}

std::optional<FieldView> embedded_root(const FieldView& embedded_value) {
    const auto& field = embedded_value.field();
    if (field.kind != core::dson::ValueKind::EmbeddedDson || !field.embedded_document) return std::nullopt;
    auto root = root_view(*field.embedded_document);
    if (!root) return std::nullopt;
    auto steps = embedded_value.steps;
    steps.insert(steps.end(), root->steps.begin(), root->steps.end());
    return FieldView{root->document, root->index, std::move(steps),
                     embedded_value.display_path + " => " + root->display_path};
}

template <typename T>
LocatedValue<T> read_value(const std::optional<FieldView>& view, std::string_view document_id,
                           std::string_view entity_id, std::string_view property,
                           CampaignModel& model, std::vector<SemanticDiagnostic>* entity_diagnostics = nullptr,
                           bool required = false) {
    if (!view) {
        if (required) {
            SemanticDiagnostic diagnostic{DiagnosticSeverity::Warning, "semantic.field_missing",
                "A mapped field is missing from this save entity", std::string{document_id}, {},
                std::string{entity_id} + ":" + std::string{property}};
            model.diagnostics.push_back(diagnostic);
            if (entity_diagnostics) entity_diagnostics->push_back(std::move(diagnostic));
        }
        return {};
    }
    if (const auto* value = std::get_if<T>(&view->field().value))
        return {std::optional<T>{*value}, view->locator(document_id)};

    SemanticDiagnostic diagnostic{DiagnosticSeverity::Warning, "semantic.field_type_mismatch",
        "A mapped field has a different DSON value type than expected", std::string{document_id},
        view->display_path, std::string{entity_id} + ":" + std::string{property}};
    model.diagnostics.push_back(diagnostic);
    if (entity_diagnostics) entity_diagnostics->push_back(std::move(diagnostic));
    return {std::nullopt, view->locator(document_id)};
}

std::optional<FieldView> document_base(const RawSaveProfile& profile, std::string_view document_id) {
    const auto document = profile.documents.find(document_id);
    if (document == profile.documents.end() || !document->second.decoded) return std::nullopt;
    return root_view(*document->second.decoded);
}

void add_diagnostic(CampaignModel& model, DiagnosticSeverity severity, std::string code,
                    std::string message, std::string document_id, std::string raw_path = {},
                    std::string entity_id = {}, std::vector<SemanticDiagnostic>* local = nullptr) {
    SemanticDiagnostic diagnostic{severity, std::move(code), std::move(message),
        std::move(document_id), std::move(raw_path), std::move(entity_id)};
    model.diagnostics.push_back(diagnostic);
    if (local) local->push_back(std::move(diagnostic));
}

struct CachedLookup {
    std::optional<ContentDefinition> definition;
    std::optional<core::Error> error;
};

class ContentLookupCache {
public:
    explicit ContentLookupCache(const IContentEnvironment& content) : content_(content) {}

    const CachedLookup& get(std::string_view type, std::string_view id) {
        const auto key = std::pair{std::string{type}, std::string{id}};
        if (const auto found = entries_.find(key); found != entries_.end()) return found->second;
        CachedLookup value;
        auto result = content_.find_content(type, id);
        if (result) value.definition = std::move(result.value());
        else value.error = result.error();
        return entries_.emplace(key, std::move(value)).first->second;
    }

private:
    const IContentEnvironment& content_;
    std::map<std::pair<std::string, std::string>, CachedLookup> entries_;
};

DefinitionReference to_reference(std::string_view id, const ContentDefinition& definition) {
    DefinitionReference result;
    result.raw_id = std::string{id};
    result.state = EntityState::Resolved;
    result.display_name = definition.localized_name.empty() ? definition.id : definition.localized_name;
    result.source_id = definition.provenance.source_id;
    result.layer_type = definition.provenance.layer_type;
    result.virtual_path = definition.provenance.virtual_path;
    result.definition_payload_json = definition.payload_json;
    result.assets.reserve(definition.asset_references.size());
    for (const auto& reference : definition.asset_references) {
        result.assets.push_back({reference.role, reference.virtual_path,
            reference.resolved_asset ? reference.resolved_asset->source_id : std::string{},
            reference.resolved_asset.has_value()});
    }
    return result;
}

DefinitionReference resolve_reference(ContentLookupCache& cache, CampaignModel& model,
                                      std::string_view type, std::string_view id,
                                      std::string_view document_id, std::string_view raw_path,
                                      std::string_view entity_id,
                                      std::vector<SemanticDiagnostic>* local = nullptr) {
    DefinitionReference result;
    result.raw_id = std::string{id};
    if (id.empty()) return result;
    const auto& lookup = cache.get(type, id);
    if (lookup.error) {
        add_diagnostic(model, DiagnosticSeverity::Warning, "content.query_failed",
            "Content lookup failed; the raw identifier was preserved: " + lookup.error->message,
            std::string{document_id}, std::string{raw_path}, std::string{entity_id}, local);
    } else if (lookup.definition) {
        result = to_reference(id, *lookup.definition);
    } else {
        add_diagnostic(model, DiagnosticSeverity::Warning, "content.definition_unresolved",
            "No effective content definition was found; the raw identifier was preserved",
            std::string{document_id}, std::string{raw_path}, std::string{entity_id}, local);
    }
    return result;
}

std::optional<std::int32_t> json_bool_as_int(std::string_view payload, std::string_view key) {
    try {
        const auto parsed = nlohmann::json::parse(payload);
        const auto found = parsed.find(std::string{key});
        if (found != parsed.end() && found->is_boolean()) return found->get<bool>() ? 1 : 0;
    } catch (const nlohmann::json::exception&) {
    }
    return std::nullopt;
}

DefinitionReference resolve_quirk(ContentLookupCache& cache, CampaignModel& model,
                                  std::string_view id, bool& is_disease,
                                  std::string_view document_id, std::string_view raw_path,
                                  std::string_view entity_id,
                                  std::vector<SemanticDiagnostic>* local) {
    for (const auto type : {std::string_view{"quirk"}, std::string_view{"disease"}}) {
        const auto& lookup = cache.get(type, id);
        if (lookup.definition) {
            is_disease = type == "disease";
            return to_reference(id, *lookup.definition);
        }
        if (lookup.error) {
            add_diagnostic(model, DiagnosticSeverity::Warning, "content.query_failed",
                "Quirk or disease lookup failed; the raw identifier was preserved: " + lookup.error->message,
                std::string{document_id}, std::string{raw_path}, std::string{entity_id}, local);
            DefinitionReference unresolved;
            unresolved.raw_id = std::string{id};
            return unresolved;
        }
    }
    add_diagnostic(model, DiagnosticSeverity::Warning, "content.definition_unresolved",
        "No effective quirk or disease definition was found; the raw identifier was preserved",
        std::string{document_id}, std::string{raw_path}, std::string{entity_id}, local);
    DefinitionReference unresolved;
    unresolved.raw_id = std::string{id};
    return unresolved;
}

std::string indexed_content_id(std::string_view hero_class, std::string_view raw_skill_id) {
    if (raw_skill_id.find(':') != std::string_view::npos) return std::string{raw_skill_id};
    if (hero_class.empty()) return std::string{raw_skill_id};
    return std::string{hero_class} + ":" + std::string{raw_skill_id};
}

void read_hero_skills(const FieldView& base, std::string_view hero_class,
                      std::string_view document_id, ContentLookupCache& cache,
                      CampaignModel& model, Hero& hero, std::string_view collection_name,
                      bool camping) {
    auto skills = child_named(base, "skills");
    if (!skills) return;
    auto collection = child_named(*skills, collection_name);
    if (!collection) return;
    auto& output = camping ? hero.camping_skills : hero.combat_skills;
    for (const auto& skill_field : child_fields(*collection)) {
        HeroSkillSelection skill;
        skill.id = skill_field.field().name;
        skill.camping = camping;
        skill.raw = skill_field.locator(document_id);
        skill.raw_value = read_value<std::int32_t>(skill_field, document_id, hero.persistent_id,
            camping ? "camping_skill_value" : "combat_skill_value", model, &hero.diagnostics);
        const auto content_id = indexed_content_id(hero_class, skill.id);
        skill.definition = resolve_reference(cache, model, "skill", content_id, document_id,
            skill_field.display_path, hero.persistent_id, &hero.diagnostics);
        output.push_back(std::move(skill));
    }
}

void read_hero_quirks(const FieldView& base, std::string_view document_id,
                     ContentLookupCache& cache, CampaignModel& model, Hero& hero) {
    auto quirks = child_named(base, "quirks");
    if (!quirks) return;
    for (const auto& quirk_field : child_fields(*quirks)) {
        HeroQuirk quirk;
        quirk.id = quirk_field.field().name;
        quirk.raw = quirk_field.locator(document_id);
        if (quirk_field.field().kind != core::dson::ValueKind::Object) {
            add_diagnostic(model, DiagnosticSeverity::Warning, "semantic.quirk_shape_invalid",
                "Quirk entry is not an object; it was retained as an unresolved raw entry",
                std::string{document_id}, quirk_field.display_path, hero.persistent_id, &hero.diagnostics);
            quirk.state = EntityState::Invalid;
            hero.quirks.push_back(std::move(quirk));
            continue;
        }
        quirk.is_new = read_value<bool>(child_named(quirk_field, "is_new"), document_id,
            hero.persistent_id, "quirk.is_new", model, &hero.diagnostics);
        quirk.is_locked = read_value<bool>(child_named(quirk_field, "is_locked"), document_id,
            hero.persistent_id, "quirk.is_locked", model, &hero.diagnostics);
        quirk.trinket_id = read_value<std::int32_t>(child_named(quirk_field, "trinketId"), document_id,
            hero.persistent_id, "quirk.trinket_id", model, &hero.diagnostics);
        quirk.mission_count = read_value<std::int32_t>(child_named(quirk_field, "mission_count"), document_id,
            hero.persistent_id, "quirk.mission_count", model, &hero.diagnostics);
        quirk.replaces_quirk = read_value<std::int32_t>(child_named(quirk_field, "replaces_quirk"), document_id,
            hero.persistent_id, "quirk.replaces_quirk", model, &hero.diagnostics);
        quirk.replaces_quirk_viewed = read_value<bool>(child_named(quirk_field, "replaces_quirk_viewed"), document_id,
            hero.persistent_id, "quirk.replaces_quirk_viewed", model, &hero.diagnostics);
        quirk.evolution_duration_remaining = read_value<std::int32_t>(
            child_named(quirk_field, "evolution_duration_remaining"), document_id,
            hero.persistent_id, "quirk.evolution_duration_remaining", model, &hero.diagnostics);
        quirk.definition = resolve_quirk(cache, model, quirk.id, quirk.is_disease, document_id,
            quirk_field.display_path, hero.persistent_id, &hero.diagnostics);
        if (const auto positive = json_bool_as_int(quirk.definition.definition_payload_json, "is_positive"))
            quirk.polarity = *positive != 0 ? domain::QuirkPolarity::Positive : domain::QuirkPolarity::Negative;
        quirk.state = quirk.definition.state == EntityState::Resolved
            ? EntityState::Resolved : EntityState::Unresolved;
        hero.quirks.push_back(std::move(quirk));
    }
}

void read_hero_trinkets(const FieldView& base, std::string_view document_id,
                       ContentLookupCache& cache, CampaignModel& model, Hero& hero) {
    auto trinkets = child_named(base, "trinkets");
    if (!trinkets) return;
    auto items = child_named(*trinkets, "items");
    if (!items) return;
    for (const auto& item : child_fields(*items)) {
        std::string id = item.field().name;
        if (item.field().kind == core::dson::ValueKind::Object) {
            const auto id_field = child_named(item, "id");
            if (id_field) {
                if (const auto* value = std::get_if<std::string>(&id_field->field().value)) id = *value;
            }
        }
        HeroTrinket trinket;
        trinket.id = id;
        trinket.raw = item.locator(document_id);
        trinket.definition = resolve_reference(cache, model, "trinket", id,
            document_id, item.display_path, hero.persistent_id, &hero.diagnostics);
        hero.trinkets.push_back(std::move(trinket));
    }
}

Hero read_hero(const FieldView& outer, std::size_t position, std::string_view document_id,
               ContentLookupCache& cache, CampaignModel& model) {
    Hero hero;
    hero.persistent_id = outer.field().name;
    hero.roster_position = position;
    hero.raw = outer.locator(document_id);
    auto file_data = child_named(outer, "hero_file_data");
    auto raw_data = file_data ? child_named(*file_data, "raw_data") : std::nullopt;
    auto nested_root = raw_data ? embedded_root(*raw_data) : std::nullopt;
    auto base = nested_root;
    if (!base) {
        hero.state = EntityState::Invalid;
        add_diagnostic(model, DiagnosticSeverity::Warning, "hero.embedded_save_missing",
            "Hero has no decodable embedded DSON body; other roster entries remain available",
            std::string{document_id}, outer.display_path, hero.persistent_id);
        hero.diagnostics.push_back({DiagnosticSeverity::Error, "hero.embedded_save_missing",
            "Hero has no decodable embedded DSON body", std::string{document_id},
            outer.display_path, hero.persistent_id});
        return hero;
    }

    auto actor = child_named(*base, "actor");
    hero.name = read_value<std::string>(actor ? child_named(*actor, "name") : std::nullopt,
        document_id, hero.persistent_id, "name", model, &hero.diagnostics);
    hero.class_id = read_value<std::string>(child_named(*base, "heroClass"), document_id,
        hero.persistent_id, "class_id", model, &hero.diagnostics, true);
    hero.resolve_xp = read_value<std::int32_t>(child_named(*base, "resolveXp"), document_id,
        hero.persistent_id, "resolve_xp", model, &hero.diagnostics);
    hero.stress = read_value<float>(child_named(*base, "m_Stress"), document_id,
        hero.persistent_id, "stress", model, &hero.diagnostics);
    hero.current_hp = read_value<float>(actor ? child_named(*actor, "current_hp") : std::nullopt,
        document_id, hero.persistent_id, "current_hp", model, &hero.diagnostics);
    hero.weapon_rank = read_value<std::int32_t>(child_named(*base, "weapon_rank"), document_id,
        hero.persistent_id, "weapon_rank", model, &hero.diagnostics);
    hero.armour_rank = read_value<std::int32_t>(child_named(*base, "armour_rank"), document_id,
        hero.persistent_id, "armour_rank", model, &hero.diagnostics);
    hero.affliction_id = read_value<std::string>(child_named(*base, "affliction_type_id"), document_id,
        hero.persistent_id, "affliction_id", model, &hero.diagnostics);
    hero.affliction_severity = read_value<std::int32_t>(child_named(*base, "affliction_severity"), document_id,
        hero.persistent_id, "affliction_severity", model, &hero.diagnostics);
    hero.virtue_id = read_value<std::string>(child_named(*base, "virtue_type_id"), document_id,
        hero.persistent_id, "virtue_id", model, &hero.diagnostics);

    if (hero.class_id.value) {
        hero.definition = resolve_reference(cache, model, "hero_class", *hero.class_id.value,
            document_id, hero.class_id.raw ? hero.class_id.raw->display_path : outer.display_path,
            hero.persistent_id, &hero.diagnostics);
        read_hero_skills(*base, *hero.class_id.value, document_id, cache, model, hero,
                         "selected_combat_skills", false);
        read_hero_skills(*base, *hero.class_id.value, document_id, cache, model, hero,
                         "selected_camping_skills", true);
    }
    read_hero_quirks(*base, document_id, cache, model, hero);
    read_hero_trinkets(*base, document_id, cache, model, hero);

    if (!hero.class_id.value) hero.state = EntityState::Invalid;
    else if (!hero.diagnostics.empty() || hero.definition.state != EntityState::Resolved)
        hero.state = EntityState::Partial;
    else hero.state = EntityState::Resolved;
    return hero;
}

void read_resources(const RawSaveProfile& profile, ContentLookupCache& cache, CampaignModel& model) {
    constexpr std::string_view document_id{"persist.estate.json"};
    const auto found_document = profile.documents.find(document_id);
    if (found_document == profile.documents.end() || !found_document->second.decoded) return;
    const auto estate = document_base(profile, document_id);
    const auto wallet = estate ? child_named(*estate, "wallet") : std::nullopt;
    if (!wallet) {
        add_diagnostic(model, DiagnosticSeverity::Warning, "estate.wallet_missing",
            "Estate document has no readable wallet object", std::string{document_id}, "base_root/wallet");
        return;
    }
    const auto scan = scan_campaign_resources(*found_document->second.decoded);
    for (const auto& issue : scan.issues)
        add_diagnostic(model, DiagnosticSeverity::Warning, "estate.wallet_mapping_issue",
            issue.message, std::string{document_id}, issue.path);
    for (const auto& value : scan.entries) {
        const auto entry = child_named(*wallet, std::to_string(value.wallet_index));
        CampaignResource resource;
        resource.index = value.wallet_index;
        resource.id = read_value<std::string>(entry ? child_named(*entry, "type") : std::nullopt,
            document_id, value.id.value_or(std::to_string(value.wallet_index)), "id", model);
        resource.amount = read_value<std::int32_t>(entry ? child_named(*entry, "amount") : std::nullopt,
            document_id, value.id.value_or(std::to_string(value.wallet_index)), "amount", model);
        if (resource.id.value)
            resource.definition = resolve_reference(cache, model, "resource", *resource.id.value,
                document_id, value.raw_object_path, std::to_string(value.wallet_index));
        resource.state = !resource.id.value || !resource.amount.value || resource.id.value->empty()
            ? EntityState::Invalid
            : resource.definition.state == EntityState::Resolved ? EntityState::Resolved : EntityState::Partial;
        model.resources.push_back(std::move(resource));
    }
}

void read_trinket_inventory(const RawSaveProfile& profile, ContentLookupCache& cache, CampaignModel& model) {
    constexpr std::string_view document_id{"persist.estate.json"};
    const auto estate = document_base(profile, document_id);
    auto trinkets = estate ? child_named(*estate, "trinkets") : std::nullopt;
    auto items = trinkets ? child_named(*trinkets, "items") : std::nullopt;
    if (!items) return;
    for (const auto& item : child_fields(*items)) {
        std::size_t index = 0;
        const auto parsed = std::from_chars(item.field().name.data(),
            item.field().name.data() + item.field().name.size(), index);
        const bool valid_index = parsed.ec == std::errc{} &&
            parsed.ptr == item.field().name.data() + item.field().name.size();
        if (!valid_index) {
            add_diagnostic(model, DiagnosticSeverity::Warning, "estate.trinket_index_invalid",
                "Trinket inventory key is not a numeric index; the raw entry is retained",
                std::string{document_id}, item.display_path);
        }
        TrinketInventoryEntry entry;
        entry.index = index;
        entry.raw_key = item.field().name;
        if (!valid_index) entry.state = EntityState::Partial;
        entry.raw = item.locator(document_id);
        entry.id = read_value<std::string>(child_named(item, "id"), document_id,
            item.field().name, "trinket.id", model);
        entry.item_type = read_value<std::string>(child_named(item, "type"), document_id,
            item.field().name, "trinket.type", model);
        entry.amount = read_value<std::int32_t>(child_named(item, "amount"), document_id,
            item.field().name, "trinket.amount", model);
        if (entry.id.value) {
            entry.definition = resolve_reference(cache, model, "trinket", *entry.id.value,
                document_id, item.display_path, item.field().name);
            if (entry.state != EntityState::Partial)
                entry.state = entry.definition.state == EntityState::Resolved ? EntityState::Resolved : EntityState::Partial;
        } else {
            entry.state = EntityState::Invalid;
            add_diagnostic(model, DiagnosticSeverity::Warning, "estate.trinket_id_missing",
                "Trinket entry has no readable raw identifier and was preserved only by its locator",
                std::string{document_id}, item.display_path, item.field().name);
        }
        model.trinket_inventory.push_back(std::move(entry));
    }
}

void read_town(const RawSaveProfile& profile, ContentLookupCache& cache, CampaignModel& model) {
    constexpr std::string_view document_id{"persist.town.json"};
    auto town = document_base(profile, document_id);
    if (!town) return;
    if (auto buildings = child_named(*town, "buildings")) {
        for (const auto& building : child_fields(*buildings)) {
            TownBuilding value;
            value.id = building.field().name;
            value.raw = building.locator(document_id);
            value.definition = resolve_reference(cache, model, "building", value.id,
                document_id, building.display_path, value.id);
            model.town_buildings.push_back(std::move(value));
        }
    }
    auto districts = child_named(*town, "districts");
    auto buildings = districts ? child_named(*districts, "buildings") : std::nullopt;
    if (buildings) {
        for (const auto& district : child_fields(*buildings)) {
            DistrictState value;
            value.id = district.field().name;
            value.raw = district.locator(document_id);
            value.built = read_value<bool>(child_named(district, "built"), document_id,
                value.id, "district.built", model);
            value.definition = resolve_reference(cache, model, "building", value.id,
                document_id, district.display_path, value.id);
            model.districts.push_back(std::move(value));
        }
    }
}

void read_progression(const RawSaveProfile& profile, CampaignModel& model) {
    constexpr std::string_view document_id{"persist.progression.json"};
    auto progression = document_base(profile, document_id);
    if (!progression) return;
    model.progression.total_recruited_heroes = read_value<std::int32_t>(
        child_named(*progression, "total_recruited_stage_coach_heroes"), document_id, {},
        "total_recruited_heroes", model);
    model.progression.total_quests_finished = read_value<std::int32_t>(
        child_named(*progression, "total_quests_finished"), document_id, {},
        "total_quests_finished", model);
    model.progression.total_successful_quests_finished = read_value<std::int32_t>(
        child_named(*progression, "total_successful_quests_finished"), document_id, {},
        "total_successful_quests_finished", model);
    model.progression.last_quest_played_id = read_value<std::string>(
        child_named(*progression, "last_quest_played_id"), document_id, {},
        "last_quest_played_id", model);
}

void read_campaign_metadata(const RawSaveProfile& profile, CampaignModel& model) {
    constexpr std::string_view document_id{"persist.game.json"};
    auto game = document_base(profile, document_id);
    if (!game) return;
    model.summary.estate_name = read_value<std::string>(child_named(*game, "estatename"),
        document_id, {}, "estate_name", model);
}

void read_roster(const RawSaveProfile& profile, ContentLookupCache& cache, CampaignModel& model) {
    constexpr std::string_view document_id{"persist.roster.json"};
    const auto roster = document_base(profile, document_id);
    const auto heroes = roster ? child_named(*roster, "heroes") : std::nullopt;
    if (!heroes) {
        add_diagnostic(model, DiagnosticSeverity::Error, "roster.heroes_missing",
            "Campaign roster document has no heroes collection", std::string{document_id}, "base_root/heroes");
        model.state = ModelState::Invalid;
        return;
    }
    const auto entries = child_fields(*heroes);
    model.heroes.reserve(entries.size());
    for (std::size_t position = 0; position < entries.size(); ++position)
        model.heroes.push_back(read_hero(entries[position], position, document_id, cache, model));
}

void read_upgrade_purchases(const RawSaveProfile& profile, CampaignModel& model) {
    constexpr std::string_view document_id{"persist.upgrades.json"};
    const auto root = document_base(profile, document_id);
    const auto purchases = root ? child_named(*root, "purchases") : std::nullopt;
    if (!purchases) return;
    const auto rows = child_fields(*purchases);
    model.upgrade_purchase_nodes.reserve(rows.size());
    for (std::size_t position = 0; position < rows.size(); ++position) {
        const auto& row = rows[position];
        const auto instance = child_named(row, "instance_number");
        const auto tree = child_named(row, "tree_id");
        const auto code = child_named(row, "requirement_code");
        const auto purchased = child_named(row, "is_purchased");
        const auto* instance_value = instance ? std::get_if<std::int32_t>(&instance->field().value) : nullptr;
        const auto* tree_value = tree ? std::get_if<std::int32_t>(&tree->field().value) : nullptr;
        const auto* code_value = code ? std::get_if<char>(&code->field().value) : nullptr;
        const auto* purchased_value = purchased ? std::get_if<bool>(&purchased->field().value) : nullptr;
        if (!instance_value || !tree_value || !code_value || !purchased_value) {
            add_diagnostic(model, DiagnosticSeverity::Warning, "upgrade.purchase_row_incomplete",
                "A purchase-history row is missing a typed key or state", std::string{document_id}, row.display_path);
            continue;
        }
        UpgradePurchaseNode node;
        node.index = position;
        const auto& raw_index = row.field().name;
        std::size_t parsed_index{};
        const auto [index_end, index_error] = std::from_chars(
            raw_index.data(), raw_index.data() + raw_index.size(), parsed_index);
        if (index_error == std::errc{} && index_end == raw_index.data() + raw_index.size())
            node.index = parsed_index;
        node.instance_number = *instance_value;
        node.tree_id = *tree_value;
        node.requirement_code = *code_value;
        node.row_raw = row.locator(document_id);
        node.is_purchased = {*purchased_value, purchased->locator(document_id)};
        model.upgrade_purchase_nodes.push_back(std::move(node));
    }
}

} // namespace

domain::CampaignModel CampaignModelBuilder::build(const RawSaveProfile& profile,
                                                    const IContentEnvironment& content) const {
    CampaignModel model;
    model.summary.profile_id = profile.descriptor.id;
    model.summary.document_count = profile.documents.size();
    ContentLookupCache cache{content};

    for (const auto& diagnostic : profile.descriptor.diagnostics)
        add_diagnostic(model, diagnostic.core_document ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning,
            "profile.document_diagnostic", diagnostic.message, diagnostic.document_id);
    for (const auto document_id : {"persist.game.json", "persist.estate.json",
                                   "persist.town.json", "persist.progression.json"}) {
        const auto document = profile.documents.find(document_id);
        if (document != profile.documents.end() && document->second.decoded) continue;
        const bool already_reported = std::any_of(model.diagnostics.begin(), model.diagnostics.end(),
            [&](const auto& diagnostic) { return diagnostic.document_id == document_id; });
        if (!already_reported)
            add_diagnostic(model, DiagnosticSeverity::Warning, "profile.document_unavailable",
                "Campaign summary input document is missing or unreadable", document_id);
    }

    read_campaign_metadata(profile, model);
    read_resources(profile, cache, model);
    read_trinket_inventory(profile, cache, model);
    read_town(profile, cache, model);
    read_progression(profile, model);
    read_roster(profile, cache, model);
    read_upgrade_purchases(profile, model);

    model.summary.hero_count = model.heroes.size();
    model.summary.resolved_hero_count = static_cast<std::size_t>(std::count_if(
        model.heroes.begin(), model.heroes.end(), [](const auto& hero) { return hero.state == EntityState::Resolved; }));
    model.summary.unresolved_hero_count = static_cast<std::size_t>(std::count_if(
        model.heroes.begin(), model.heroes.end(), [](const auto& hero) {
            return hero.state == EntityState::Unresolved || hero.state == EntityState::Invalid ||
                   hero.definition.state == EntityState::Unresolved;
        }));
    model.summary.resource_count = model.resources.size();
    model.summary.trinket_inventory_count = model.trinket_inventory.size();
    model.summary.town_building_count = model.town_buildings.size();
    model.summary.district_count = model.districts.size();
    model.summary.diagnostic_count = model.diagnostics.size();

    if (model.state != ModelState::Invalid) {
        const auto has_partial_entities = [](const auto& entities) {
            return std::any_of(entities.begin(), entities.end(), [](const auto& entity) {
                return entity.state != EntityState::Resolved;
            });
        };
        const bool partial = !model.diagnostics.empty() || has_partial_entities(model.resources) ||
            has_partial_entities(model.heroes) || has_partial_entities(model.trinket_inventory) ||
            std::any_of(model.town_buildings.begin(), model.town_buildings.end(), [](const auto& entity) {
                return entity.definition.state != EntityState::Resolved;
            }) || std::any_of(model.districts.begin(), model.districts.end(), [](const auto& entity) {
                return entity.definition.state != EntityState::Resolved;
            });
        model.state = partial ? ModelState::Partial : ModelState::Complete;
    }
    return model;
}

} // namespace ddse::application
