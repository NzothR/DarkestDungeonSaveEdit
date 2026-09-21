#pragma once

#include "ddse/application/campaign_edit_session.hpp"
#include "ddse/application/file_system.hpp"
#include "ddse/application/save_profile.hpp"
#include "ddse/core/dson/dson_writer.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ddse::application {

struct CandidateDocument {
    std::string id;
    std::string bytes;
    core::dson::BinaryDiffReport binary_diff;
    std::vector<std::string> expected_field_paths;
};

struct SaveCandidate {
    std::vector<CandidateDocument> documents;
    std::vector<std::string> affected_documents;
};

class SaveAdapter {
public:
    [[nodiscard]] core::Result<SaveCandidate, core::Error>
    build_candidate(const RawSaveProfile& profile, const ChangeSet& changes) const;
};

struct SaveCommitResult {
    std::filesystem::path target_profile_root;
    std::filesystem::path backup_directory;
    std::vector<std::string> committed_documents;
    std::vector<core::dson::BinaryDiffReport> binary_diffs;
};

// Commits only to an explicitly supplied profile copy. The loaded source profile remains untouched.
class SafeSaveCommitter {
public:
    explicit SafeSaveCommitter(IFileSystem& file_system) : file_system_(file_system) {}

    [[nodiscard]] core::Result<SaveCommitResult, core::Error>
    commit(const RawSaveProfile& source_profile, const ChangeSet& changes,
           const std::filesystem::path& target_profile_root,
           const std::filesystem::path& backup_directory) const;

private:
    IFileSystem& file_system_;
};

} // namespace ddse::application
