#include "ddse/application/save_profile.hpp"

#include "ddse/core/dson/dson_reader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include <string_view>

namespace ddse::application {
namespace {

using DomainList = std::vector<SaveDomain>;

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

DomainList classify_document(std::string_view filename) {
    const auto name = lower_ascii(std::string{filename});
    DomainList domains;
    auto add = [&](SaveDomain domain) {
        if (std::find(domains.begin(), domains.end(), domain) == domains.end()) domains.push_back(domain);
    };

    if (name.find("circus") != std::string::npos || name.find("butcher") != std::string::npos ||
        name.find("pvp") != std::string::npos) add(SaveDomain::Circus);
    if (name.find("raid") != std::string::npos) add(SaveDomain::Raid);
    if (name.find("shared") != std::string::npos) add(SaveDomain::Shared);

    static constexpr std::array<std::string_view, 16> campaign_files{
        "persist.game.json", "persist.roster.json", "persist.estate.json", "persist.town.json",
        "persist.quest.json", "persist.progression.json", "persist.upgrades.json", "persist.tutorial.json",
        "persist.town_event.json", "persist.narration.json", "persist.journal.json",
        "persist.game_knowledge.json", "persist.curio_tracker.json", "persist.campaign_mash.json",
        "persist.campaign_log.json", "novelty_tracker.json"};
    if (std::find(campaign_files.begin(), campaign_files.end(), name) != campaign_files.end())
        add(SaveDomain::Campaign);
    if (domains.empty()) add(SaveDomain::Unknown);
    return domains;
}

bool is_core_document(std::string_view filename, const DomainList& domains) {
    const auto name = lower_ascii(std::string{filename});
    if (std::find(domains.begin(), domains.end(), SaveDomain::Campaign) != domains.end())
        return name == "persist.game.json" || name == "persist.roster.json";
    if (std::find(domains.begin(), domains.end(), SaveDomain::Circus) != domains.end())
        return name.find("circus") != std::string::npos || name.find("butcher") != std::string::npos ||
               name.find("pvp") != std::string::npos;
    if (std::find(domains.begin(), domains.end(), SaveDomain::Raid) != domains.end())
        return name == "persist.raid.json";
    return false;
}

void append_u64(std::string& target, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) target.push_back(static_cast<char>((value >> (i * 8U)) & 0xffU));
}

std::uint64_t profile_fingerprint(const IFileSystem& file_system,
                                  const std::vector<std::filesystem::path>& paths) {
    std::vector<std::filesystem::path> sorted = paths;
    std::sort(sorted.begin(), sorted.end());
    std::uint64_t hash = 14695981039346656037ULL;
    auto feed = [&](std::string_view bytes) {
        for (const unsigned char byte : bytes) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
    };
    for (const auto& path : sorted) {
        const auto relative_name = path.filename().generic_string();
        feed(relative_name);
        feed(std::string_view{"\0", 1});
        const auto read = file_system.read_file(path);
        if (!read) {
            feed("<read-error>");
            feed(read.error().message);
            continue;
        }
        std::string length;
        append_u64(length, static_cast<std::uint64_t>(read.value().size()));
        feed(length);
        feed(read.value());
    }
    return hash;
}

} // namespace

std::uint64_t SaveProfileDiscovery::fingerprint(std::string_view bytes) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

core::Result<std::vector<RawSaveProfile>, core::Error>
SaveProfileDiscovery::discover(const std::filesystem::path& save_root) const {
    auto directories = file_system_.list_directories(save_root);
    if (!directories) return core::Result<std::vector<RawSaveProfile>, core::Error>::failure(directories.error());
    std::vector<RawSaveProfile> profiles;
    profiles.reserve(directories.value().size());
    for (const auto& directory : directories.value()) {
        const auto candidate_name = directory.filename().string();
        if (!candidate_name.starts_with("profile_")) continue;
        auto profile = load(directory);
        if (!profile) return core::Result<std::vector<RawSaveProfile>, core::Error>::failure(profile.error());
        profiles.push_back(std::move(profile.value()));
    }
    return core::Result<std::vector<RawSaveProfile>, core::Error>::success(std::move(profiles));
}

core::Result<RawSaveProfile, core::Error>
SaveProfileDiscovery::load(const std::filesystem::path& profile_root) const {
    auto listed = file_system_.list_files(profile_root);
    if (!listed) return core::Result<RawSaveProfile, core::Error>::failure(listed.error());

    RawSaveProfile profile;
    profile.descriptor.id = profile_root.filename().string();
    profile.descriptor.root_path = profile_root;
    auto paths = std::move(listed.value());
    std::sort(paths.begin(), paths.end());
    std::vector<SaveDomain> domains;

    for (const auto& path : paths) {
        const auto id = path.filename().string();
        const auto doc_domains = classify_document(id);
        const bool is_core = is_core_document(id, doc_domains);
        RawSaveDocument document;
        document.id = id;
        document.path = path;
        document.domains = doc_domains;
        document.core_document = is_core;

        for (const auto domain : doc_domains)
            if (domain != SaveDomain::Unknown && std::find(domains.begin(), domains.end(), domain) == domains.end())
                domains.push_back(domain);

        auto modified = file_system_.last_modified(path);
        if (modified && modified.value() &&
            (!profile.descriptor.modified_at || *modified.value() > *profile.descriptor.modified_at))
            profile.descriptor.modified_at = *modified.value();

        auto read = file_system_.read_file(path);
        if (!read) {
            profile.status = ProfileReadStatus::PartialReadOnly;
            profile.descriptor.diagnostics.push_back({id, read.error().message, is_core});
            document.decode_error = read.error();
            profile.descriptor.document_ids.push_back(id);
            profile.documents.emplace(id, std::move(document));
            continue;
        }
        document.bytes = std::move(read.value());
        document.baseline_fingerprint = fingerprint(document.bytes);

        if (path.extension() == ".json") {
            const auto* raw = reinterpret_cast<const std::byte*>(document.bytes.data());
            core::dson::DsonReader reader;
            auto parsed = reader.parse(std::span<const std::byte>{raw, document.bytes.size()}, id);
            if (parsed) {
                document.decoded = std::move(parsed.value());
            } else {
                document.decode_error = parsed.error();
                profile.status = ProfileReadStatus::PartialReadOnly;
                profile.descriptor.diagnostics.push_back({id, parsed.error().message, is_core});
            }
        }
        profile.descriptor.document_ids.push_back(id);
        profile.documents.emplace(id, std::move(document));
    }

    profile.descriptor.detected_domains = std::move(domains);
    profile.baseline_fingerprint = profile_fingerprint(file_system_, paths);
    return core::Result<RawSaveProfile, core::Error>::success(std::move(profile));
}

core::Result<bool, core::Error> RawSaveProfile::matches_disk_baseline(const IFileSystem& file_system) const {
    auto listed = file_system.list_files(descriptor.root_path);
    if (!listed) return core::Result<bool, core::Error>::failure(listed.error());
    return core::Result<bool, core::Error>::success(
        baseline_fingerprint == profile_fingerprint(file_system, listed.value()));
}

std::string_view to_string(SaveDomain domain) noexcept {
    switch (domain) {
    case SaveDomain::Campaign: return "Campaign";
    case SaveDomain::Circus: return "Circus";
    case SaveDomain::Raid: return "Raid";
    case SaveDomain::Shared: return "Shared";
    case SaveDomain::Unknown: return "Unknown";
    }
    return "Unknown";
}

std::string_view to_string(ProfileReadStatus status) noexcept {
    switch (status) {
    case ProfileReadStatus::Complete: return "Complete";
    case ProfileReadStatus::PartialReadOnly: return "PartialReadOnly";
    }
    return "PartialReadOnly";
}

} // namespace ddse::application
