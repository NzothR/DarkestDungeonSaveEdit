#include "ddse/infrastructure/native_file_system.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ddse::infrastructure {
namespace {

core::Error fs_error(const std::filesystem::path& path, const std::error_code& ec) {
    core::ErrorCode code = core::ErrorCode::IoError;
    if (ec == std::errc::no_such_file_or_directory) code = core::ErrorCode::FileNotFound;
    else if (ec == std::errc::permission_denied) code = core::ErrorCode::PermissionDenied;
    core::Error error{code, ec.message(), "NativeFileSystem", {}};
    error.context.emplace("path", path.string());
    error.context.emplace("os_error", std::to_string(ec.value()));
    return error;
}

core::Error stream_error(const std::filesystem::path& path, bool missing) {
    const auto code = missing ? core::ErrorCode::FileNotFound : core::ErrorCode::IoError;
    return {code, missing ? "File does not exist or cannot be opened" : "File operation failed",
            "NativeFileSystem", {{"path", path.string()}}};
}

} // namespace

core::Result<bool, core::Error> NativeFileSystem::exists(const std::filesystem::path& path) const {
    std::error_code ec;
    const bool found = std::filesystem::exists(path, ec);
    if (ec) return core::Result<bool, core::Error>::failure(fs_error(path, ec));
    return core::Result<bool, core::Error>::success(found);
}

core::Result<std::string, core::Error> NativeFileSystem::read_file(const std::filesystem::path& path) const {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::error_code ec;
        const bool found = std::filesystem::exists(path, ec);
        if (ec) return core::Result<std::string, core::Error>::failure(fs_error(path, ec));
        const bool missing = !found;
        return core::Result<std::string, core::Error>::failure(stream_error(path, missing));
    }
    std::string bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    if (input.bad()) return core::Result<std::string, core::Error>::failure(stream_error(path, false));
    return core::Result<std::string, core::Error>::success(std::move(bytes));
}

core::Result<void, core::Error> NativeFileSystem::write_file(const std::filesystem::path& path,
                                                              const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return core::Result<void, core::Error>::failure(stream_error(path, false));
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) return core::Result<void, core::Error>::failure(stream_error(path, false));
    return core::Result<void, core::Error>::success();
}

core::Result<void, core::Error> NativeFileSystem::write_file_atomic(const std::filesystem::path& path,
                                                                    const std::string& bytes) {
    static std::atomic<std::uint64_t> sequence{};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto temporary = path;
    temporary += ".ddse-tmp-" + std::to_string(stamp) + "-" + std::to_string(sequence.fetch_add(1));

    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return core::Result<void, core::Error>::failure(stream_error(temporary, false));
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) {
            output.close();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return core::Result<void, core::Error>::failure(stream_error(temporary, false));
        }
        output.close();
        if (output.fail()) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return core::Result<void, core::Error>::failure(stream_error(temporary, false));
        }
    }

#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const std::error_code ec{static_cast<int>(GetLastError()), std::system_category()};
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<void, core::Error>::failure(fs_error(path, ec));
    }
#else
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<void, core::Error>::failure(fs_error(path, ec));
    }
#endif
    return core::Result<void, core::Error>::success();
}

core::Result<void, core::Error> NativeFileSystem::create_directories(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) return core::Result<void, core::Error>::failure(fs_error(path, ec));
    return core::Result<void, core::Error>::success();
}

core::Result<std::vector<std::filesystem::path>, core::Error>
NativeFileSystem::list_files(const std::filesystem::path& directory) const {
    std::error_code ec;
    std::filesystem::directory_iterator it{directory, ec};
    if (ec) return core::Result<std::vector<std::filesystem::path>, core::Error>::failure(fs_error(directory, ec));
    std::vector<std::filesystem::path> files;
    const std::filesystem::directory_iterator end;
    while (it != end) {
        const auto entry_path = it->path();
        std::error_code status_error;
        if (it->is_regular_file(status_error)) files.push_back(entry_path);
        if (status_error) return core::Result<std::vector<std::filesystem::path>, core::Error>::failure(fs_error(entry_path, status_error));
        it.increment(ec);
        if (ec) return core::Result<std::vector<std::filesystem::path>, core::Error>::failure(fs_error(directory, ec));
    }
    return core::Result<std::vector<std::filesystem::path>, core::Error>::success(std::move(files));
}

core::Result<std::vector<std::filesystem::path>, core::Error>
NativeFileSystem::list_directories(const std::filesystem::path& directory) const {
    std::error_code ec;
    std::filesystem::directory_iterator it{directory, ec};
    if (ec) return core::Result<std::vector<std::filesystem::path>, core::Error>::failure(fs_error(directory, ec));
    std::vector<std::filesystem::path> directories;
    const std::filesystem::directory_iterator end;
    while (it != end) {
        std::error_code status_error;
        if (it->is_directory(status_error)) directories.push_back(it->path());
        if (status_error) return core::Result<std::vector<std::filesystem::path>, core::Error>::failure(fs_error(it->path(), status_error));
        it.increment(ec);
        if (ec) return core::Result<std::vector<std::filesystem::path>, core::Error>::failure(fs_error(directory, ec));
    }
    std::sort(directories.begin(), directories.end());
    return core::Result<std::vector<std::filesystem::path>, core::Error>::success(std::move(directories));
}

core::Result<std::optional<std::filesystem::file_time_type>, core::Error>
NativeFileSystem::last_modified(const std::filesystem::path& path) const {
    std::error_code ec;
    const auto value = std::filesystem::last_write_time(path, ec);
    if (ec == std::errc::no_such_file_or_directory)
        return core::Result<std::optional<std::filesystem::file_time_type>, core::Error>::success(std::nullopt);
    if (ec) return core::Result<std::optional<std::filesystem::file_time_type>, core::Error>::failure(fs_error(path, ec));
    return core::Result<std::optional<std::filesystem::file_time_type>, core::Error>::success(value);
}

core::Result<std::uint64_t, core::Error> NativeFileSystem::file_size(const std::filesystem::path& path) const {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return core::Result<std::uint64_t, core::Error>::failure(fs_error(path, ec));
    return core::Result<std::uint64_t, core::Error>::success(static_cast<std::uint64_t>(size));
}

} // namespace ddse::infrastructure
