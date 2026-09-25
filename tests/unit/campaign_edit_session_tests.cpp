#include "ddse/application/campaign_edit_session.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ddse;

domain::RawLocator locator(std::string document, std::string path) {
    return {std::move(document), {{0, "base_root", false}, {1, "field", false}}, std::move(path)};
}

domain::CampaignModel sample_model() {
    domain::CampaignModel model;

    domain::CampaignResource resource;
    resource.index = 4;
    resource.id.value = "gold";
    resource.id.raw = locator("persist.estate.json", "base_root/wallet/4/type");
    resource.amount.value = 100;
    resource.amount.raw = locator("persist.estate.json", "base_root/wallet/4/amount");
    resource.state = domain::EntityState::Resolved;
    model.resources.push_back(std::move(resource));

    domain::Hero hero;
    hero.persistent_id = "hero-1";
    hero.state = domain::EntityState::Resolved;
    hero.name.value = "Junia";
    hero.name.raw = locator("persist.roster.json", "base_root/heroes/hero-1/hero_file_data/raw_data => base_root/actor/name");
    hero.resolve_xp.value = 12;
    hero.resolve_xp.raw = locator("persist.roster.json", "base_root/heroes/hero-1/hero_file_data/raw_data => base_root/resolveXp");
    hero.stress.value = 15.0F;
    hero.stress.raw = locator("persist.roster.json", "base_root/heroes/hero-1/hero_file_data/raw_data => base_root/m_Stress");
    hero.affliction_id.value = "";
    hero.affliction_id.raw = locator("persist.roster.json", "base_root/heroes/hero-1/hero_file_data/raw_data => base_root/affliction_type_id");
    hero.affliction_severity.value = 0;
    hero.affliction_severity.raw = locator("persist.roster.json", "base_root/heroes/hero-1/hero_file_data/raw_data => base_root/affliction_severity");
    hero.virtue_id.value = "";
    hero.virtue_id.raw = locator("persist.roster.json", "base_root/heroes/hero-1/hero_file_data/raw_data => base_root/virtue_type_id");
    domain::HeroQuirk quirk;
    quirk.id = "mod_positive_quirk";
    quirk.state = domain::EntityState::Resolved;
    quirk.polarity = domain::QuirkPolarity::Positive;
    quirk.is_locked.value = false;
    quirk.is_locked.raw = locator("persist.roster.json",
        "base_root/heroes/hero-1/hero_file_data/raw_data => base_root/quirks/mod_positive_quirk/is_locked");
    quirk.raw = locator("persist.roster.json",
        "base_root/heroes/hero-1/hero_file_data/raw_data => base_root/quirks/mod_positive_quirk");
    quirk.definition.definition_payload_json = R"({"can_modify_in_activity":true})";
    hero.quirks.push_back(std::move(quirk));
    domain::HeroTrinket equipped;
    equipped.id = "test_trinket";
    equipped.raw = locator("persist.roster.json",
        "base_root/heroes/hero-1/hero_file_data/raw_data => base_root/trinkets/items/2");
    hero.trinkets.push_back(std::move(equipped));
    model.heroes.push_back(std::move(hero));

    domain::Hero unrelated;
    unrelated.persistent_id = "hero-2";
    unrelated.state = domain::EntityState::Resolved;
    unrelated.name.value = "Paracelsus";
    unrelated.name.raw = locator("persist.roster.json", "base_root/heroes/hero-2/hero_file_data/raw_data => base_root/actor/name");
    unrelated.stress.value = 8.0F;
    unrelated.stress.raw = locator("persist.roster.json", "base_root/heroes/hero-2/hero_file_data/raw_data => base_root/m_Stress");
    unrelated.affliction_id.value = "";
    unrelated.affliction_id.raw = locator("persist.roster.json", "base_root/heroes/hero-2/hero_file_data/raw_data => base_root/affliction_type_id");
    unrelated.affliction_severity.value = 0;
    unrelated.affliction_severity.raw = locator("persist.roster.json", "base_root/heroes/hero-2/hero_file_data/raw_data => base_root/affliction_severity");
    unrelated.virtue_id.value = "";
    unrelated.virtue_id.raw = locator("persist.roster.json", "base_root/heroes/hero-2/hero_file_data/raw_data => base_root/virtue_type_id");
    model.heroes.push_back(std::move(unrelated));

    domain::DistrictState district;
    district.id = "test_district";
    district.built.value = false;
    district.built.raw = locator("persist.town.json", "base_root/districts/buildings/test_district/built");
    district.raw = locator("persist.town.json", "base_root/districts/buildings/test_district");
    model.districts.push_back(std::move(district));
    return model;
}

application::CampaignOperation resource_amount(std::size_t index, std::int32_t amount) {
    return application::SetCampaignValueOperation{
        {"Estate.Resource.Amount", {}, index}, amount};
}

application::CampaignOperation hero_name(std::string id, std::string name) {
    return application::SetCampaignValueOperation{
        {"Hero.Name", std::move(id), std::nullopt}, std::move(name)};
}

} // namespace

TEST(CampaignEditSession, ApplyUndoAndRedoAreSymmetricAndRevisioned) {
    const auto original = sample_model();
    application::CampaignEditSession session{original};
    ASSERT_EQ(session.revision(), 0U);
    ASSERT_EQ(session.model().resources.front().amount.value, 100);
    ASSERT_EQ(session.model().heroes[1].name.value, "Paracelsus");

    const auto applied = session.apply(resource_amount(4, 777), session.revision());
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(applied.value().revision, 1U);
    ASSERT_EQ(applied.value().changes.changes.size(), 1U);
    EXPECT_EQ(applied.value().changes.changes.front().before, application::CampaignValue{100});
    EXPECT_EQ(applied.value().changes.changes.front().after, application::CampaignValue{777});
    EXPECT_EQ(applied.value().changes.changes.front().raw.display_path, "base_root/wallet/4/amount");
    ASSERT_EQ(applied.value().changes.changes.front().raw.steps.size(), 2U);
    EXPECT_EQ(applied.value().changes.affected_documents, std::vector<std::string>{"persist.estate.json"});
    EXPECT_EQ(session.model().resources.front().amount.value, 777);
    ASSERT_EQ(session.pending_changes().changes.size(), 1U);
    EXPECT_EQ(session.pending_changes().changes.front().before, application::CampaignValue{100});
    EXPECT_EQ(session.pending_changes().changes.front().after, application::CampaignValue{777});
    EXPECT_EQ(original.resources.front().amount.value, 100);
    EXPECT_EQ(session.model().heroes[1].name.value, "Paracelsus");
    EXPECT_TRUE(session.can_undo());
    EXPECT_FALSE(session.can_redo());

    const auto undone = session.undo(session.revision());
    ASSERT_TRUE(undone) << undone.error().message;
    EXPECT_EQ(undone.value().revision, 2U);
    EXPECT_EQ(undone.value().changes.changes.front().before, application::CampaignValue{777});
    EXPECT_EQ(undone.value().changes.changes.front().after, application::CampaignValue{100});
    EXPECT_EQ(session.model().resources.front().amount.value, 100);
    EXPECT_TRUE(session.pending_changes().empty());
    EXPECT_FALSE(session.can_undo());
    EXPECT_TRUE(session.can_redo());

    const auto redone = session.redo(session.revision());
    ASSERT_TRUE(redone) << redone.error().message;
    EXPECT_EQ(redone.value().revision, 3U);
    EXPECT_EQ(session.model().resources.front().amount.value, 777);
    EXPECT_TRUE(session.can_undo());
    EXPECT_FALSE(session.can_redo());
}

TEST(CampaignEditSession, CampingEquipmentReplacesOldestInEditedOrder) {
    auto model = sample_model();
    auto& hero = model.heroes.front();
    hero.raw = locator("persist.roster.json", "base_root/heroes/hero-1");
    for (const auto* id : {"A", "B", "C", "D"}) {
        domain::HeroSkillSelection skill;
        skill.id = id;
        skill.camping = true;
        skill.raw = locator("persist.roster.json",
            "base_root/heroes/hero-1/hero_file_data/raw_data => base_root/skills/selected_camping_skills/" +
            std::string{id});
        skill.raw_value.value = 0;
        skill.raw_value.raw = skill.raw;
        hero.camping_skills.push_back(std::move(skill));
    }
    application::CampaignEditSession session{std::move(model)};
    const auto selected_ids = [&] {
        std::vector<std::string> ids;
        for (const auto& skill : session.model().heroes.front().camping_skills) ids.push_back(skill.id);
        return ids;
    };
    for (const auto* id : {"E", "F", "A", "B", "C"}) {
        auto planned = application::make_set_hero_camping_skill_equipped_operation(
            session.model(), "hero-1", id, true);
        ASSERT_TRUE(planned) << planned.error().message;
        auto applied = session.apply(planned.value(), session.revision());
        ASSERT_TRUE(applied) << applied.error().message;
    }
    EXPECT_EQ(selected_ids(), (std::vector<std::string>{"F", "A", "B", "C"}));
    auto undone = session.undo(session.revision());
    ASSERT_TRUE(undone) << undone.error().message;
    EXPECT_EQ(selected_ids(), (std::vector<std::string>{"E", "F", "A", "B"}));
    auto redone = session.redo(session.revision());
    ASSERT_TRUE(redone) << redone.error().message;
    EXPECT_EQ(selected_ids(), (std::vector<std::string>{"F", "A", "B", "C"}));
}

TEST(CampaignEditSession, AfflictionStateAndStressValueAreSeparateEdits) {
    application::CampaignEditSession session{sample_model()};
    const auto direct_status_edit = session.apply(
        application::SetCampaignValueOperation{{"Hero.VirtueId", "hero-1"}, std::string{"stalwart"}},
        session.revision());
    EXPECT_FALSE(direct_status_edit);
    const auto applied = session.apply(application::SetHeroAfflictionStateOperation{
        "hero-2", application::HeroAfflictionState::Afflicted, "fearful"}, session.revision());
    ASSERT_TRUE(applied) << applied.error().message;
    ASSERT_EQ(applied.value().changes.changes.size(), 2U);
    EXPECT_EQ(session.model().heroes[1].stress.value, 8.0F);
    EXPECT_EQ(session.model().heroes[1].affliction_id.value, "fearful");
    EXPECT_EQ(session.model().heroes[1].affliction_severity.value, 1);
    EXPECT_EQ(session.model().heroes[1].virtue_id.value, "");

    const auto stress_set = session.apply(
        application::SetCampaignValueOperation{{"Hero.Stress", "hero-2"}, 100.0F}, session.revision());
    ASSERT_TRUE(stress_set) << stress_set.error().message;
    EXPECT_EQ(session.model().heroes[1].stress.value, 100.0F);
    EXPECT_EQ(session.model().heroes[1].affliction_id.value, "fearful");

    const auto stress_undone = session.undo(session.revision());
    ASSERT_TRUE(stress_undone) << stress_undone.error().message;
    EXPECT_EQ(session.model().heroes[1].stress.value, 8.0F);
    EXPECT_EQ(session.model().heroes[1].affliction_id.value, "fearful");

    const auto affliction_cleared = session.apply(application::SetHeroAfflictionStateOperation{
        "hero-2", application::HeroAfflictionState::Normal, {}}, session.revision());
    ASSERT_TRUE(affliction_cleared) << affliction_cleared.error().message;
    EXPECT_EQ(session.model().heroes[1].affliction_id.value, "");
    EXPECT_EQ(session.model().heroes[1].affliction_severity.value, 0);
    EXPECT_EQ(session.model().heroes[1].virtue_id.value, "");
}

TEST(CampaignEditSession, PendingChangeSetFoldsRepeatedEditsToTheSessionBaseline) {
    application::CampaignEditSession session{sample_model()};
    ASSERT_TRUE(session.apply(resource_amount(4, 200), 0));
    ASSERT_TRUE(session.apply(resource_amount(4, 300), 1));
    ASSERT_EQ(session.pending_changes().changes.size(), 1U);
    EXPECT_EQ(session.pending_changes().changes.front().before, application::CampaignValue{100});
    EXPECT_EQ(session.pending_changes().changes.front().after, application::CampaignValue{300});

    ASSERT_TRUE(session.undo(2));
    ASSERT_EQ(session.pending_changes().changes.size(), 1U);
    EXPECT_EQ(session.pending_changes().changes.front().after, application::CampaignValue{200});
    ASSERT_TRUE(session.undo(3));
    EXPECT_TRUE(session.pending_changes().empty());
}

TEST(CampaignEditSession, CompositeOperationChangesMultipleDocumentsWithOneUndo) {
    application::CampaignEditSession session{sample_model()};
    const application::CompositeCampaignOperation composite{
        "Edit campaign basics",
        {
            {{"Estate.Resource.Amount", {}, 4}, std::int32_t{900}},
            {{"Hero.Name", "hero-1", std::nullopt}, std::string{"Aster"}},
            {{"Hero.Stress", "hero-1", std::nullopt}, 0.0F},
        }, {}, {}};

    const auto applied = session.apply(composite, 0);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(applied.value().revision, 1U);
    ASSERT_EQ(applied.value().changes.changes.size(), 3U);
    EXPECT_EQ(applied.value().changes.affected_documents,
              (std::vector<std::string>{"persist.estate.json", "persist.roster.json"}));
    EXPECT_EQ(session.model().resources.front().amount.value, 900);
    EXPECT_EQ(session.model().heroes.front().name.value, "Aster");
    EXPECT_EQ(session.model().heroes.front().stress.value, 0.0F);

    const auto undone = session.undo(1);
    ASSERT_TRUE(undone) << undone.error().message;
    EXPECT_EQ(undone.value().revision, 2U);
    EXPECT_EQ(session.model().resources.front().amount.value, 100);
    EXPECT_EQ(session.model().heroes.front().name.value, "Junia");
    EXPECT_EQ(session.model().heroes.front().stress.value, 15.0F);
}

TEST(CampaignEditSession, InvalidTargetAndMissingMappingLeaveModelUntouched) {
    application::CampaignEditSession session{sample_model()};
    const auto before = session.model();

    const application::CampaignOperation invalid_target = application::SetCampaignValueOperation{
        {"Hero.Name", "missing-hero", std::nullopt}, std::string{"Aster"}};
    const auto missing_target_result = session.apply(invalid_target, 0);
    ASSERT_FALSE(missing_target_result);
    EXPECT_EQ(missing_target_result.error().code, core::ErrorCode::ValidationFailed);
    EXPECT_EQ(session.model().heroes.front().name.value, before.heroes.front().name.value);
    EXPECT_EQ(session.revision(), 0U);

    const application::CampaignOperation no_mapping = application::SetCampaignValueOperation{
        {"Hero.UnmappedField", "hero-1", std::nullopt}, std::int32_t{10}};
    const auto no_mapping_result = session.apply(no_mapping, 0);
    ASSERT_FALSE(no_mapping_result);
    EXPECT_EQ(no_mapping_result.error().code, core::ErrorCode::MappingNotWritable);
    EXPECT_EQ(session.model().heroes.front().resolve_xp.value, before.heroes.front().resolve_xp.value);
    EXPECT_EQ(session.revision(), 0U);
}

TEST(CampaignEditSession, CompositeValidationIsAtomicWhenOneTargetIsInvalid) {
    application::CampaignEditSession session{sample_model()};
    const application::CompositeCampaignOperation composite{
        "Should be rejected atomically",
        {
            {{"Estate.Resource.Amount", {}, 4}, std::int32_t{500}},
            {{"Hero.Name", "missing-hero", std::nullopt}, std::string{"Aster"}},
        }, {}, {}};

    const auto result = session.apply(composite, 0);
    ASSERT_FALSE(result);
    EXPECT_EQ(session.model().resources.front().amount.value, 100);
    EXPECT_EQ(session.model().heroes.front().name.value, "Junia");
    EXPECT_EQ(session.revision(), 0U);
    EXPECT_FALSE(session.can_undo());
}

TEST(CampaignEditSession, StaleRevisionIsRejectedWithoutChangingTheDraft) {
    application::CampaignEditSession session{sample_model()};
    ASSERT_TRUE(session.apply(resource_amount(4, 200), 0));
    const auto stale = session.apply(resource_amount(4, 300), 0);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, core::ErrorCode::StaleSessionRevision);
    EXPECT_EQ(session.model().resources.front().amount.value, 200);
    EXPECT_EQ(session.revision(), 1U);
}

TEST(CampaignEditSession, ValidationAndRiskAreSeparateAndRiskDoesNotBlockStaging) {
    const auto model = sample_model();
    const application::CampaignOperation invalid = resource_amount(4, -1);
    const auto invalid_report = application::CampaignOperationValidator{}.validate(model, invalid);
    EXPECT_FALSE(invalid_report.valid());

    const application::CampaignOperation high_risk = application::SetCampaignValueOperation{
        {"Hero.ResolveXp", "hero-1", std::nullopt}, std::int32_t{50}};
    const auto validation = application::CampaignOperationValidator{}.validate(model, high_risk);
    const auto risk = application::CampaignOperationRiskAssessor{}.assess(high_risk);
    EXPECT_TRUE(validation.valid());
    EXPECT_EQ(risk.level, application::CampaignRiskLevel::High);
    EXPECT_FALSE(risk.reasons.empty());

    application::CampaignEditSession session{model};
    const auto applied = session.apply(high_risk, 0);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_TRUE(applied.value().validation.valid());
    EXPECT_EQ(applied.value().risk.level, application::CampaignRiskLevel::High);
    EXPECT_EQ(session.model().heroes.front().resolve_xp.value, 50);
}

TEST(CampaignEditSession, PartialEntityFieldCanBeEditedWithNonBlockingValidationWarning) {
    auto model = sample_model();
    model.heroes.front().state = domain::EntityState::Partial;
    const application::CampaignOperation operation = hero_name("hero-1", "Aster");

    const auto validation = application::CampaignOperationValidator{}.validate(model, operation);
    ASSERT_TRUE(validation.valid());
    ASSERT_EQ(validation.issues.size(), 1U);
    EXPECT_EQ(validation.issues.front().severity, application::ValidationSeverity::Warning);
    EXPECT_FALSE(validation.issues.front().blocking);

    application::CampaignEditSession session{std::move(model)};
    const auto result = session.apply(operation, 0);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(session.model().heroes.front().name.value, "Aster");
}

TEST(CampaignEditSession, NoOpDoesNotCreateHistoryOrAdvanceRevision) {
    application::CampaignEditSession session{sample_model()};
    const auto result = session.apply(resource_amount(4, 100), 0);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_TRUE(result.value().changes.empty());
    EXPECT_EQ(result.value().revision, 0U);
    EXPECT_FALSE(session.can_undo());
}

TEST(CampaignEditSession, NewEditAfterUndoClearsTheRedoBranch) {
    application::CampaignEditSession session{sample_model()};
    ASSERT_TRUE(session.apply(resource_amount(4, 200), 0));
    ASSERT_TRUE(session.undo(1));
    ASSERT_TRUE(session.can_redo());

    const auto new_edit = session.apply(hero_name("hero-1", "Aster"), 2);
    ASSERT_TRUE(new_edit) << new_edit.error().message;
    EXPECT_EQ(new_edit.value().revision, 3U);
    EXPECT_EQ(session.model().heroes.front().name.value, "Aster");
    EXPECT_FALSE(session.can_redo());
}

TEST(CampaignEditSession, TypedQuirkLockOperationUsesTheMappedBooleanAndSupportsUndo) {
    application::CampaignEditSession session{sample_model()};
    const application::CampaignOperation operation = application::SetHeroQuirkLockedOperation{
        "hero-1", "mod_positive_quirk", true};
    const auto applied = session.apply(operation, 0);
    ASSERT_TRUE(applied) << applied.error().message;
    ASSERT_EQ(applied.value().changes.changes.size(), 1U);
    EXPECT_EQ(applied.value().changes.changes.front().target.semantic_property, "Hero.Quirk.Locked");
    EXPECT_EQ(applied.value().changes.changes.front().after, application::CampaignValue{true});
    EXPECT_TRUE(session.model().heroes.front().quirks.front().is_locked.value.value());

    ASSERT_TRUE(session.undo(1));
    EXPECT_FALSE(session.model().heroes.front().quirks.front().is_locked.value.value());
    ASSERT_TRUE(session.redo(2));
    EXPECT_TRUE(session.model().heroes.front().quirks.front().is_locked.value.value());
}

TEST(CampaignEditSession, QuirkRemovalIsStructuralAndReversibleInTheWorkingModel) {
    auto model = sample_model();
    const auto entry = model.heroes.front().quirks.front();
    application::CampaignEditSession session{std::move(model)};
    const application::CampaignOperation operation = application::RemoveHeroQuirkOperation{
        "hero-1", "mod_positive_quirk"};

    const auto applied = session.apply(operation, 0);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_TRUE(applied.value().changes.changes.empty());
    ASSERT_EQ(applied.value().changes.structural_changes.size(), 1U);
    EXPECT_EQ(applied.value().changes.structural_changes.front().raw.display_path, entry.raw.display_path);
    EXPECT_TRUE(session.model().heroes.front().quirks.empty());
    ASSERT_EQ(session.pending_changes().structural_changes.size(), 1U);

    const auto undone = session.undo(1);
    ASSERT_TRUE(undone);
    ASSERT_EQ(undone.value().changes.structural_changes.size(), 1U);
    EXPECT_EQ(undone.value().changes.structural_changes.front().action,
              application::CampaignStructuralAction::Restore);
    EXPECT_TRUE(session.pending_changes().empty());
    ASSERT_EQ(session.model().heroes.front().quirks.size(), 1U);
    EXPECT_EQ(session.model().heroes.front().quirks.front().id, entry.id);
    ASSERT_TRUE(session.redo(2));
    EXPECT_TRUE(session.model().heroes.front().quirks.empty());
    ASSERT_EQ(session.pending_changes().structural_changes.size(), 1U);
}

TEST(CampaignEditSession, DistrictStateUsesAnExplicitSemanticOperation) {
    application::CampaignEditSession session{sample_model()};
    const auto result = session.apply(application::SetDistrictBuiltOperation{"test_district", true}, 0);
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_EQ(result.value().changes.changes.size(), 1U);
    EXPECT_EQ(result.value().changes.changes.front().target.semantic_property, "Town.District.Built");
    EXPECT_EQ(result.value().changes.changes.front().raw.document_id, "persist.town.json");
    EXPECT_TRUE(session.model().districts.front().built.value.value());
}

TEST(CampaignEditSession, DestroyEquippedTrinketRemovesItFromTheDraftAndCanBeUndone) {
    application::CampaignEditSession session{sample_model()};
    const auto raw_path = session.model().heroes.front().trinkets.front().raw.display_path;
    const auto result = session.apply(application::DestroyTrinketOperation{"hero-1", raw_path}, 0);
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_EQ(result.value().changes.structural_changes.size(), 1U);
    EXPECT_EQ(result.value().changes.structural_changes.front().target.semantic_property, "Hero.Trinket.Entry");
    EXPECT_TRUE(session.model().heroes.front().trinkets.empty());
    ASSERT_TRUE(session.undo(1));
    ASSERT_EQ(session.model().heroes.front().trinkets.size(), 1U);
    EXPECT_EQ(session.model().heroes.front().trinkets.front().id, "test_trinket");
}

TEST(CampaignEditSession, CapabilityCatalogEnablesVerifiedFeaturesAndKeepsDeferredFeaturesDisabled) {
    const auto& catalog = application::campaign_operation_capabilities();
    const auto find = [&](std::string_view id) {
        return std::find_if(catalog.begin(), catalog.end(), [&](const auto& item) {
            return item.operation_id == id;
        });
    };
    const auto resource = find("campaign.resource.set_amount");
    const auto camping_lock = find("campaign.hero.lock_camping_skill");
    const auto disease = find("campaign.hero.edit_disease");
    const auto add_hero = find("campaign.hero.add");
    const auto rename = find("campaign.hero.rename");
    const auto delete_hero = find("campaign.hero.delete");
    const auto reorder_heroes = find("campaign.hero.reorder");
    const auto set_stress = find("campaign.hero.set_stress");
    const auto affliction_state = find("campaign.hero.set_affliction_state");
    ASSERT_NE(resource, catalog.end());
    ASSERT_NE(camping_lock, catalog.end());
    ASSERT_NE(disease, catalog.end());
    ASSERT_NE(add_hero, catalog.end());
    ASSERT_NE(rename, catalog.end());
    ASSERT_NE(delete_hero, catalog.end());
    ASSERT_NE(reorder_heroes, catalog.end());
    ASSERT_NE(set_stress, catalog.end());
    ASSERT_NE(affliction_state, catalog.end());
    EXPECT_EQ(resource->availability, application::CampaignOperationAvailability::Available);
    EXPECT_EQ(camping_lock->availability, application::CampaignOperationAvailability::Deferred);
    EXPECT_EQ(disease->availability, application::CampaignOperationAvailability::Deferred);
    EXPECT_EQ(add_hero->availability, application::CampaignOperationAvailability::Available);
    EXPECT_EQ(rename->availability, application::CampaignOperationAvailability::Available);
    EXPECT_EQ(delete_hero->availability, application::CampaignOperationAvailability::Available);
    EXPECT_EQ(reorder_heroes->availability, application::CampaignOperationAvailability::Available);
    EXPECT_EQ(set_stress->availability, application::CampaignOperationAvailability::Available);
    EXPECT_EQ(affliction_state->availability, application::CampaignOperationAvailability::Available);
}
