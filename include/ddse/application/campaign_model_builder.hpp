#pragma once

#include "ddse/application/content_environment.hpp"
#include "ddse/application/save_profile.hpp"
#include "ddse/domain/campaign_model.hpp"

namespace ddse::application {

class CampaignModelBuilder {
public:
    [[nodiscard]] domain::CampaignModel build(const RawSaveProfile& profile,
                                               const IContentEnvironment& content) const;
};

} // namespace ddse::application
