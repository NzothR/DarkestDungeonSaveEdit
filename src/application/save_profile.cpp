#include "ddse/application/save_profile.hpp"

#include "ddse/core/dson/dson_reader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <set>
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

struct ProfileTree {
    std::vector<std::filesystem::path> files;
    std::vector<std::filesystem::path> directories;
};

core::Result<ProfileTree, core::Error> collect_profile_tree(const IFileSystem& file_system,
                                                             const std::filesystem::path& root) {
    auto root_exists = file_system.exists(root);
    if (!root_exists) return core::Result<ProfileTree, core::Error>::failure(root_exists.error());
    if (!root_exists.value())
        return core::Result<ProfileTree, core::Error>::failure(
            {core::ErrorCode::FileNotFound, "Save profile directory does not exist", "SaveProfile",
             {{"path", root.string()}}});

    ProfileTree tree;
    std::vector<std::filesystem::path> pending{root};
    std::set<std::string, std::less<>> visited;
    while (!pending.empty()) {
        const auto directory = std::move(pending.back());
        pending.pop_back();
        const auto relative_directory = directory.lexically_relative(root);
        if (std::distance(relative_directory.begin(), relative_directory.end()) > 64)
            return core::Result<ProfileTree, core::Error>::failure(
                {core::ErrorCode::IoError, "Save profile directory nesting exceeds the safe traversal limit",
                 "SaveProfile", {{"path", directory.string()}}});
        if (!visited.insert(directory.lexically_normal().generic_string()).second) continue;
        if (visited.size() > 100000)
            return core::Result<ProfileTree, core::Error>::failure(
                {core::ErrorCode::IoError, "Save profile directory tree exceeds the traversal limit", "SaveProfile",
                 {{"path", root.string()}}});

        auto files = file_system.list_files(directory);
        if (!files) return core::Result<ProfileTree, core::Error>::failure(files.error());
        tree.files.insert(tree.files.end(), files.value().begin(), files.value().end());
        auto directories = file_system.list_directories(directory);
        if (!directories) return core::Result<ProfileTree, core::Error>::failure(directories.error());
        for (const auto& child : directories.value()) {
            tree.directories.push_back(child);
            pending.push_back(child);
        }
    }
    return core::Result<ProfileTree, core::Error>::success(std::move(tree));
}

void feed_bytes(std::uint64_t& hash, std::string_view bytes) {
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
}

void feed_u64(std::uint64_t& hash, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        const auto byte = static_cast<char>((value >> (i * 8U)) & 0xffU);
        feed_bytes(hash, std::string_view{&byte, 1});
    }
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

core::Result<std::uint64_t, core::Error>
SaveProfileDiscovery::fingerprint_profile(const IFileSystem& file_system,
                                          const std::filesystem::path& profile_root) {
    auto tree = collect_profile_tree(file_system, profile_root);
    if (!tree) return core::Result<std::uint64_t, core::Error>::failure(tree.error());

    const auto relative_paths = [&](const std::vector<std::filesystem::path>& paths) {
        std::vector<std::filesystem::path> relative;
        relative.reserve(paths.size());
        for (const auto& path : paths) relative.push_back(path.lexically_relative(profile_root));
        std::sort(relative.begin(), relative.end());
        return relative;
    };
    const auto directories = relative_paths(tree.value().directories);
    const auto files = relative_paths(tree.value().files);

    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto& path : directories) {
        feed_bytes(hash, "D");
        feed_bytes(hash, path.generic_string());
        feed_bytes(hash, std::string_view{"\0", 1});
    }
    for (const auto& relative_path : files) {
        const auto read = file_system.read_file(profile_root / relative_path);
        if (!read) return core::Result<std::uint64_t, core::Error>::failure(read.error());
        feed_bytes(hash, "F");
        feed_bytes(hash, relative_path.generic_string());
        feed_bytes(hash, std::string_view{"\0", 1});
        feed_u64(hash, static_cast<std::uint64_t>(read.value().size()));
        feed_bytes(hash, read.value());
    }
    return core::Result<std::uint64_t, core::Error>::success(hash);
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
    auto baseline_before = fingerprint_profile(file_system_, profile_root);
    if (!baseline_before)
        return core::Result<RawSaveProfile, core::Error>::failure(baseline_before.error());
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
    auto baseline_after = fingerprint_profile(file_system_, profile_root);
    if (!baseline_after) return core::Result<RawSaveProfile, core::Error>::failure(baseline_after.error());
    if (baseline_before.value() != baseline_after.value())
        return core::Result<RawSaveProfile, core::Error>::failure(
            {core::ErrorCode::ConcurrentSaveChanged,
             "Save profile changed while its documents were being loaded", "SaveProfile",
             {{"profile_root", profile_root.string()}}});
    profile.baseline_fingerprint = baseline_before.value();
    return core::Result<RawSaveProfile, core::Error>::success(std::move(profile));
}

core::Result<bool, core::Error> RawSaveProfile::matches_disk_baseline(const IFileSystem& file_system) const {
    auto current = SaveProfileDiscovery::fingerprint_profile(file_system, descriptor.root_path);
    if (!current) return core::Result<bool, core::Error>::failure(current.error());
    return core::Result<bool, core::Error>::success(baseline_fingerprint == current.value());
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
