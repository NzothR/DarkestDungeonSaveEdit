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
    model.heroes.push_back(std::move(hero));

    domain::Hero unrelated;
    unrelated.persistent_id = "hero-2";
    unrelated.state = domain::EntityState::Resolved;
    unrelated.name.value = "Paracelsus";
    unrelated.name.raw = locator("persist.roster.json", "base_root/heroes/hero-2/hero_file_data/raw_data => base_root/actor/name");
    model.heroes.push_back(std::move(unrelated));
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
    EXPECT_FALSE(session.can_undo());
    EXPECT_TRUE(session.can_redo());

    const auto redone = session.redo(session.revision());
    ASSERT_TRUE(redone) << redone.error().message;
    EXPECT_EQ(redone.value().revision, 3U);
    EXPECT_EQ(session.model().resources.front().amount.value, 777);
    EXPECT_TRUE(session.can_undo());
    EXPECT_FALSE(session.can_redo());
}

TEST(CampaignEditSession, CompositeOperationChangesMultipleDocumentsWithOneUndo) {
    application::CampaignEditSession session{sample_model()};
    const application::CompositeCampaignOperation composite{
        "Edit campaign basics",
        {
            {{"Estate.Resource.Amount", {}, 4}, std::int32_t{900}},
            {{"Hero.Name", "hero-1", std::nullopt}, std::string{"Aster"}},
            {{"Hero.Stress", "hero-1", std::nullopt}, 0.0F},
        }};

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
        }};

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
