#include "ddse/application/content_scanner.hpp"

#include "ddse/core/dson/dson_document.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <iterator>

namespace ddse::application {
namespace {

using Json = nlohmann::json;

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string normalized_extension(const std::filesystem::path& path) {
    return lower_ascii(path.extension().string());
}

std::string normalize_virtual_path(const std::filesystem::path& path) {
    auto result = path.generic_string();
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

std::uint64_t fingerprint(std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool is_text_payload(std::string_view bytes) {
    for (std::size_t i = 0; i < bytes.size();) {
        const auto c = static_cast<unsigned char>(bytes[i]);
        if (c == 0 || (c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f')) return false;
        if (c < 0x80) { ++i; continue; }
        std::size_t continuation_count = 0;
        std::uint32_t codepoint = 0;
        if ((c & 0xe0) == 0xc0) { continuation_count = 1; codepoint = c & 0x1f; }
        else if ((c & 0xf0) == 0xe0) { continuation_count = 2; codepoint = c & 0x0f; }
        else if ((c & 0xf8) == 0xf0) { continuation_count = 3; codepoint = c & 0x07; }
        else return false;
        if (i + continuation_count >= bytes.size()) return false;
        for (std::size_t j = 1; j <= continuation_count; ++j) {
            const auto next = static_cast<unsigned char>(bytes[i + j]);
            if ((next & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if ((continuation_count == 1 && codepoint < 0x80) ||
            (continuation_count == 2 && codepoint < 0x800) ||
            (continuation_count == 3 && codepoint < 0x10000) ||
            codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
        i += continuation_count + 1;
    }
    return true;
}

// Some older Darkest Dungeon mods ship their .darkest definitions in the
// Windows code page (usually CP936/GBK) instead of UTF-8.  The parser treats
// non-ASCII bytes as opaque token data, so these files are still safe to scan
// as long as they do not contain binary NUL/control bytes.
bool is_legacy_darkest_text_payload(std::string_view bytes) {
    for (const auto byte : bytes) {
        const auto c = static_cast<unsigned char>(byte);
        if (c == 0 || (c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f'))
            return false;
    }
    return true;
}

void add_definition(BaseContentScanResult& result, std::string kind, std::string id,
                    const ContentSourceRoot& source, std::string_view virtual_path,
                    std::string localization_key = {}, std::string display_name = {},
                    std::string payload_json = {}) {
    if (id.empty()) return;
    result.definitions.push_back({std::move(kind), std::move(id), source.id,
                                  std::string{virtual_path}, std::move(display_name),
                                  std::move(localization_key), std::move(payload_json)});
}

void add_asset_reference(BaseContentScanResult& result, std::string_view source_id,
                         std::string_view definition_type, std::string_view content_id,
                         std::string_view definition_virtual_path, std::string_view role,
                         std::string_view reference_type, std::string virtual_path,
                         std::string_view origin) {
    if (content_id.empty() || virtual_path.empty()) return;
    result.asset_references.push_back({std::string{source_id}, std::string{definition_type},
        std::string{content_id}, std::string{definition_virtual_path}, std::string{role},
        std::string{reference_type}, std::move(virtual_path), std::string{origin}});
}

void add_relationship(BaseContentScanResult& result, std::string_view source_id,
                      std::string_view parent_type, std::string_view parent_id,
                      std::string_view relationship_type, std::string_view child_type,
                      std::string_view child_id, std::string_view virtual_path) {
    if (parent_id.empty() || child_id.empty()) return;
    result.relationships.push_back({std::string{source_id}, std::string{parent_type},
        std::string{parent_id}, std::string{relationship_type}, std::string{child_type},
        std::string{child_id}, std::string{virtual_path}});
}

std::string json_string(const Json& value, std::string_view key) {
    const auto it = value.find(std::string{key});
    if (it == value.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

bool json_bool(const Json& value, std::string_view key, bool fallback = false) {
    const auto it = value.find(std::string{key});
    if (it == value.end() || !it->is_boolean()) return fallback;
    return it->get<bool>();
}

std::string json_dump(const Json& value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string parent_component(std::string_view path, std::string_view component) {
    const auto lower_path = lower_ascii(std::string{path});
    const auto at = lower_path.find(component);
    if (at == std::string::npos) return {};
    const auto start = at + component.size();
    const auto end = lower_path.find('/', start);
    return std::string{path.substr(start, end == std::string::npos ? path.size() - start : end - start)};
}

std::string path_parent_name(std::string_view path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string_view::npos) return {};
    const auto previous = path.find_last_of('/', slash - 1);
    return std::string{path.substr(previous == std::string_view::npos ? 0 : previous + 1,
                                   slash - (previous == std::string_view::npos ? 0 : previous + 1))};
}

void collect_json_definitions(const Json& node, const ContentSourceRoot& source,
                              std::string_view virtual_path, BaseContentScanResult& result,
                              std::string_view context_key = {}) {
    if (node.is_object()) {
        const auto lower_path = lower_ascii(std::string{virtual_path});
        const auto id = json_string(node, "id");
        if (!id.empty()) {
            if (lower_path.find("trinkets/") != std::string::npos &&
                lower_path.find(".entries.trinkets.json") != std::string::npos) {
                add_definition(result, "trinket", id, source, virtual_path,
                               "str_inventory_title_trinket" + id, {}, json_dump(node));
                add_asset_reference(result, source.id, "trinket", id, virtual_path,
                                    "inventory_icon", "file",
                                    "panels/icons_equip/trinket/inv_trinket+" + id + ".png",
                                    "convention");
                if (const auto requirements = node.find("hero_class_requirements");
                    requirements != node.end() && requirements->is_array()) {
                    for (const auto& requirement : *requirements) {
                        if (requirement.is_string())
                            add_relationship(result, source.id, "trinket", id, "restricted_to",
                                             "hero_class", requirement.get<std::string>(), virtual_path);
                    }
                }
            } else if (lower_path.find("quirk/") != std::string::npos &&
                       lower_path.find("quirk_library") != std::string::npos &&
                       (context_key == "quirks" || node.contains("is_disease"))) {
                const bool disease = json_bool(node, "is_disease");
                add_definition(result, disease ? "disease" : "quirk", id, source, virtual_path,
                               "str_quirk_name_" + id, {}, json_dump(node));
            } else if (lower_path.find("trait/") != std::string::npos &&
                       lower_path.find("trait_library") != std::string::npos && context_key == "traits") {
                add_definition(result, "trait", id, source, virtual_path,
                               "trait_name_" + id, {}, json_dump(node));
            } else if (lower_path.find("camping_skills") != std::string::npos &&
                       !lower_path.ends_with("default.camping_skills.json")) {
                const auto hero = parent_component(virtual_path, "heroes/");
                const auto owner = hero.empty() ? std::string{"shared"} : hero;
                const auto skill_id = owner + ":" + id;
                add_definition(result, "skill", skill_id, source, virtual_path,
                               "camping_skill_name_" + owner + "_" + id,
                               {}, json_dump(node));
                if (!hero.empty())
                    add_relationship(result, source.id, "hero_class", hero, "camping_skill",
                                     "skill", skill_id, virtual_path);
            } else if ((lower_path.find("/inventory/") != std::string::npos ||
                        lower_path.find("estate_items") != std::string::npos) &&
                       (context_key == "items" || context_key == "entries")) {
                add_definition(result, "resource", id, source, virtual_path,
                        "str_inventory_title_" + id, {}, json_dump(node));
            }
        }

        for (auto it = node.begin(); it != node.end(); ++it)
            collect_json_definitions(it.value(), source, virtual_path, result, it.key());
    } else if (node.is_array()) {
        for (const auto& child : node)
            collect_json_definitions(child, source, virtual_path, result, context_key);
    }
}

std::string utf8_from_codepoint(std::uint32_t cp) {
    std::string result;
    if (cp <= 0x7f) result.push_back(static_cast<char>(cp));
    else if (cp <= 0x7ff) {
        result.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        result.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        result.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        result.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        result.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        result.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        result.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
    return result;
}

std::string decode_xml_entities(std::string_view input) {
    std::string result;
    for (std::size_t i = 0; i < input.size();) {
        if (input[i] != '&') { result.push_back(input[i++]); continue; }
        const auto semicolon = input.find(';', i + 1);
        if (semicolon == std::string_view::npos) { result.push_back(input[i++]); continue; }
        const auto entity = input.substr(i + 1, semicolon - i - 1);
        if (entity == "amp") result.push_back('&');
        else if (entity == "lt") result.push_back('<');
        else if (entity == "gt") result.push_back('>');
        else if (entity == "quot") result.push_back('"');
        else if (entity == "apos") result.push_back('\'');
        else if (!entity.empty() && entity[0] == '#') {
            std::uint32_t cp{};
            const bool hex = entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X');
            const auto digits = entity.substr(hex ? 2 : 1);
            const auto* begin = digits.data();
            const auto* end = begin + digits.size();
            const auto parsed = std::from_chars(begin, end, cp, hex ? 16 : 10);
            if (parsed.ec == std::errc{} && parsed.ptr == end && cp <= 0x10ffff &&
                !(cp >= 0xd800 && cp <= 0xdfff)) result += utf8_from_codepoint(cp);
            else result.append(input.substr(i, semicolon - i + 1));
        } else result.append(input.substr(i, semicolon - i + 1));
        i = semicolon + 1;
    }
    return result;
}

std::map<std::string, std::string, std::less<>> parse_xml_attributes(std::string_view tag) {
    std::map<std::string, std::string, std::less<>> attributes;
    std::size_t pos = 0;
    while (pos < tag.size() && !std::isspace(static_cast<unsigned char>(tag[pos]))) ++pos;
    while (pos < tag.size()) {
        while (pos < tag.size() && (std::isspace(static_cast<unsigned char>(tag[pos])) || tag[pos] == '/')) ++pos;
        const auto name_start = pos;
        while (pos < tag.size() && tag[pos] != '=' && !std::isspace(static_cast<unsigned char>(tag[pos]))) ++pos;
        if (name_start == pos) break;
        const std::string name{tag.substr(name_start, pos - name_start)};
        while (pos < tag.size() && std::isspace(static_cast<unsigned char>(tag[pos]))) ++pos;
        if (pos == tag.size() || tag[pos] != '=') continue;
        ++pos;
        while (pos < tag.size() && std::isspace(static_cast<unsigned char>(tag[pos]))) ++pos;
        if (pos == tag.size() || (tag[pos] != '\'' && tag[pos] != '"')) continue;
        const char quote = tag[pos++];
        const auto value_start = pos;
        while (pos < tag.size() && tag[pos] != quote) ++pos;
        if (pos == tag.size()) break;
        attributes.emplace(name, decode_xml_entities(tag.substr(value_start, pos - value_start)));
        ++pos;
    }
    return attributes;
}

bool is_self_closing_xml_tag(std::string_view tag) {
    while (!tag.empty() && std::isspace(static_cast<unsigned char>(tag.back()))) tag.remove_suffix(1);
    return !tag.empty() && tag.back() == '/';
}

core::Result<std::vector<ScannedLocalization>, core::Error>
parse_localization_xml(std::string_view bytes, const ContentSourceRoot& source,
                       std::string_view virtual_path) {
    std::vector<ScannedLocalization> entries;
    std::size_t cursor = 0;
    bool found_language = false;
    while ((cursor = bytes.find("<language", cursor)) != std::string_view::npos) {
        found_language = true;
        const auto language_tag = cursor;
        const auto language_end = bytes.find('>', language_tag);
        if (language_end == std::string_view::npos)
            return core::Result<std::vector<ScannedLocalization>, core::Error>::failure(
                {core::ErrorCode::ContentParseFailed, "Unterminated language element", "ContentParser",
                 {{"path", std::string{virtual_path}}}});
        const auto language_tag_view = bytes.substr(language_tag + 1, language_end - language_tag - 1);
        const auto language_attrs = parse_xml_attributes(language_tag_view);
        const auto language_it = language_attrs.find("id");
        if (language_it == language_attrs.end() || language_it->second.empty())
            return core::Result<std::vector<ScannedLocalization>, core::Error>::failure(
                {core::ErrorCode::ContentParseFailed, "Language element has no id", "ContentParser",
                 {{"path", std::string{virtual_path}}}});
        if (is_self_closing_xml_tag(language_tag_view)) {
            cursor = language_end + 1;
            continue;
        }
        const auto language_close = bytes.find("</language>", language_end + 1);
        if (language_close == std::string_view::npos)
            return core::Result<std::vector<ScannedLocalization>, core::Error>::failure(
                {core::ErrorCode::ContentParseFailed, "Language element has no closing tag", "ContentParser",
                 {{"path", std::string{virtual_path}}}});
        const auto language = language_it->second;

        std::size_t entry_cursor = language_end + 1;
        while ((entry_cursor = bytes.find("<entry", entry_cursor)) != std::string_view::npos && entry_cursor < language_close) {
            const auto tag_end = bytes.find('>', entry_cursor);
            if (tag_end == std::string_view::npos || tag_end >= language_close)
                return core::Result<std::vector<ScannedLocalization>, core::Error>::failure(
                    {core::ErrorCode::ContentParseFailed, "Unterminated entry element", "ContentParser",
                     {{"path", std::string{virtual_path}}}});
            const auto attrs = parse_xml_attributes(bytes.substr(entry_cursor + 1, tag_end - entry_cursor - 1));
            const auto key = attrs.find("id");
            const auto entry_tag = bytes.substr(entry_cursor + 1, tag_end - entry_cursor - 1);
            if (is_self_closing_xml_tag(entry_tag)) {
                if (key != attrs.end())
                    entries.push_back({language, key->second, {}, source.id, std::string{virtual_path}});
                entry_cursor = tag_end + 1;
                continue;
            }
            const auto close = bytes.find("</entry>", tag_end + 1);
            if (close == std::string_view::npos || close > language_close)
                return core::Result<std::vector<ScannedLocalization>, core::Error>::failure(
                    {core::ErrorCode::ContentParseFailed, "Entry has no closing element", "ContentParser",
                     {{"path", std::string{virtual_path}}}});
            if (key != attrs.end()) {
                auto value = bytes.substr(tag_end + 1, close - tag_end - 1);
                const auto cdata = value.find("<![CDATA[");
                bool is_cdata = false;
                if (cdata != std::string_view::npos) {
                    const auto end_cdata = value.find("]]>", cdata + 9);
                    if (end_cdata == std::string_view::npos)
                        return core::Result<std::vector<ScannedLocalization>, core::Error>::failure(
                            {core::ErrorCode::ContentParseFailed, "Unterminated CDATA section", "ContentParser",
                             {{"path", std::string{virtual_path}}}});
                    value = value.substr(cdata + 9, end_cdata - cdata - 9);
                    is_cdata = true;
                }
                entries.push_back({language, key->second, is_cdata ? std::string{value} : decode_xml_entities(value),
                                   source.id, std::string{virtual_path}});
            }
            entry_cursor = close + std::string_view{"</entry>"}.size();
        }
        cursor = language_close + std::string_view{"</language>"}.size();
    }
    if (!found_language)
        return core::Result<std::vector<ScannedLocalization>, core::Error>::failure(
            {core::ErrorCode::ContentParseFailed, "String table has no language element", "ContentParser",
             {{"path", std::string{virtual_path}}}});
    return core::Result<std::vector<ScannedLocalization>, core::Error>::success(std::move(entries));
}

struct DarkestRecord {
    std::string type;
    std::map<std::string, std::vector<std::string>, std::less<>> fields;
};

std::vector<std::string> tokenize_darkest_line(std::string_view line, bool& malformed) {
    std::vector<std::string> tokens;
    for (std::size_t pos = 0; pos < line.size();) {
        while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
        if (pos == line.size() || line[pos] == '#') break;
        if (line[pos] == '"') {
            ++pos;
            std::string token;
            bool closed = false;
            while (pos < line.size()) {
                const char c = line[pos++];
                if (c == '"') { closed = true; break; }
                if (c == '\\' && pos < line.size()) {
                    const char escaped = line[pos++];
                    token.push_back(escaped == 'n' ? '\n' : escaped == 't' ? '\t' : escaped);
                } else token.push_back(c);
            }
            if (!closed) { malformed = true; return {}; }
            tokens.push_back(std::move(token));
        } else {
            const auto start = pos;
            while (pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
            tokens.emplace_back(line.substr(start, pos - start));
        }
    }
    return tokens;
}

core::Result<std::vector<DarkestRecord>, core::Error>
parse_darkest(std::string_view bytes, std::string_view virtual_path) {
    std::vector<DarkestRecord> records;
    std::size_t cursor = 0;
    std::size_t line_number = 0;
    while (cursor <= bytes.size()) {
        ++line_number;
        const auto end = bytes.find('\n', cursor);
        const auto line = bytes.substr(cursor, end == std::string_view::npos ? bytes.size() - cursor : end - cursor);
        bool malformed = false;
        auto tokens = tokenize_darkest_line(line, malformed);
        if (malformed)
            return core::Result<std::vector<DarkestRecord>, core::Error>::failure(
                {core::ErrorCode::ContentParseFailed, "Unterminated quoted string in .darkest file", "ContentParser",
                 {{"path", std::string{virtual_path}}, {"line", std::to_string(line_number)}}});
        if (tokens.size() >= 2 && tokens[0].ends_with(':')) {
            tokens[0].pop_back();
            DarkestRecord record;
            record.type = tokens[0];
            std::string current_field;
            for (std::size_t i = 1; i < tokens.size(); ++i) {
                auto& token = tokens[i];
                if (!token.empty() && token[0] == '.') {
                    current_field = token.substr(1);
                    record.fields.try_emplace(current_field);
                } else if (!current_field.empty()) record.fields[current_field].push_back(token);
            }
            records.push_back(std::move(record));
        }
        if (end == std::string_view::npos) break;
        cursor = end + 1;
    }
    return core::Result<std::vector<DarkestRecord>, core::Error>::success(std::move(records));
}

std::string field_first(const DarkestRecord& record, std::string_view name) {
    const auto it = record.fields.find(name);
    return it == record.fields.end() || it->second.empty() ? std::string{} : it->second.front();
}

void collect_darkest_definitions(const std::vector<DarkestRecord>& records,
                                 const ContentSourceRoot& source, std::string_view virtual_path,
                                 BaseContentScanResult& result) {
    const auto lower_path = lower_ascii(std::string{virtual_path});
    if (lower_path.find("heroes/") != std::string::npos && lower_path.ends_with(".info.darkest")) {
        const auto hero = path_parent_name(virtual_path);
        const auto loc_key = "hero_class_name_" + hero;
        Json hero_payload = Json::object();
        Json::array_t hero_records;
        for (const auto& record : records) {
            Json fields = Json::object();
            for (const auto& [field_name, values] : record.fields) fields[field_name] = values;
            hero_records.push_back(Json{{"type", record.type}, {"fields", std::move(fields)}});
        }
        hero_payload["records"] = std::move(hero_records);
        add_definition(result, "hero_class", hero, source, virtual_path, loc_key, {}, hero_payload.dump());
        add_asset_reference(result, source.id, "hero_class", hero, virtual_path,
                            "class_asset_bundle", "directory", "heroes/" + hero + "/", "convention");
        std::map<std::string, Json::array_t, std::less<>> skill_levels;
        for (const auto& record : records) {
            if (record.type != "combat_skill" && record.type != "combat_move_skill") continue;
            const auto id = field_first(record, "id");
            if (id.empty()) continue;
            Json level = Json::object();
            for (const auto& [field_name, values] : record.fields) level[field_name] = values;
            skill_levels[id].push_back(std::move(level));
        }
        for (auto& [id, levels] : skill_levels) {
            const auto skill_id = hero + ":" + id;
            const auto localization_key = "combat_skill_name_" + hero + "_" + id;
            add_definition(result, "skill", skill_id, source, virtual_path, localization_key,
                           {}, Json{{"levels", std::move(levels)}}.dump());
            add_relationship(result, source.id, "hero_class", hero, "combat_skill", "skill",
                             skill_id, virtual_path);
        }
    }
    for (const auto& record : records) {
        if (record.type != "inventory_item") continue;
        const auto type = field_first(record, "type");
        const auto id = field_first(record, "id");
        if (type.empty()) continue;
        const auto content_id = type + (id.empty() ? std::string{} : ":" + id);
        const auto localization_key = "str_inventory_title_" + type + id;
        Json::object_t payload;
        for (const auto& [field_name, values] : record.fields) payload[field_name] = values;
        add_definition(result, "resource", content_id, source, virtual_path,
                       localization_key, {}, Json{payload}.dump());
    }
}

bool has_extension(const std::vector<std::string>& extensions, std::string_view extension) {
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

bool is_excluded_directory(const BaseContentScanConfig& config, const std::filesystem::path& directory) {
    const auto name = lower_ascii(directory.filename().string());
    return std::any_of(config.excluded_directories.begin(), config.excluded_directories.end(),
                       [&](const auto& excluded) { return name == lower_ascii(excluded); });
}

core::Result<void, core::Error> collect_files(const IFileSystem& file_system,
                                               const BaseContentScanConfig& config,
                                               const ContentSourceRoot& source,
                                               const std::vector<std::filesystem::path>& scan_roots,
                                               BaseContentScanResult& result) {
    std::vector<std::filesystem::path> pending = scan_roots;
    while (!pending.empty()) {
        auto directory = std::move(pending.back());
        pending.pop_back();
        auto directories = file_system.list_directories(directory);
        if (!directories) return core::Result<void, core::Error>::failure(directories.error());
        for (const auto& child : directories.value())
            if (!is_excluded_directory(config, child)) pending.push_back(child);
        auto files = file_system.list_files(directory);
        if (!files) return core::Result<void, core::Error>::failure(files.error());
        std::sort(files.value().begin(), files.value().end());
        for (const auto& path : files.value()) {
            const auto extension = normalized_extension(path);
            const bool is_asset = has_extension(config.asset_extensions, extension);
            const bool is_content = has_extension(config.content_extensions, extension);
            if (!is_asset && !is_content) continue;
            std::error_code relative_error;
            const auto relative = std::filesystem::relative(path, source.path, relative_error);
            if (relative_error)
                return core::Result<void, core::Error>::failure(
                    {core::ErrorCode::IoError, relative_error.message(), "BaseContentScanner",
                     {{"path", path.string()}}});
            const auto virtual_path = normalize_virtual_path(relative);
            auto size = file_system.file_size(path);
            if (!size) {
                result.diagnostics.push_back({source.id, virtual_path, size.error().message});
                continue;
            }
            ScannedSourceFile source_file{source.id, virtual_path, extension, size.value(), 0, is_asset};
            if (is_asset) {
                result.assets.push_back({source.id, virtual_path, extension, size.value()});
                result.source_files.push_back(std::move(source_file));
                continue;
            }

            auto read = file_system.read_file(path);
            if (!read) {
                result.diagnostics.push_back({source.id, virtual_path, read.error().message});
                result.source_files.push_back(std::move(source_file));
                continue;
            }
            source_file.content_fingerprint = fingerprint(read.value());
            const auto lower_virtual_path = lower_ascii(virtual_path);
            const bool is_darkest_definition_payload =
                extension == ".darkest" &&
                (lower_virtual_path.ends_with(".info.darkest") ||
                 lower_virtual_path.starts_with("inventory/") ||
                 lower_virtual_path.find("/inventory/") != std::string::npos);
            const bool is_text = is_text_payload(read.value()) ||
                (is_darkest_definition_payload && is_legacy_darkest_text_payload(read.value()));
            if (!is_text) {
                result.diagnostics.push_back({source.id, virtual_path,
                    "Binary payload with a text-designated extension was retained without parsing"});
            } else if (extension == ".json") {
                try {
                    const auto document = Json::parse(read.value());
                    collect_json_definitions(document, source, virtual_path, result);
                    if (lower_virtual_path.ends_with(".building.json")) {
                        const auto building = path_parent_name(virtual_path);
                        add_definition(result, "building", building, source, virtual_path,
                                       "town_name_" + building, {}, json_dump(document));
                    }
                } catch (const Json::exception& error) {
                    result.diagnostics.push_back({source.id, virtual_path, error.what()});
                }
            } else if (is_darkest_definition_payload) {
                auto parsed = parse_darkest(read.value(), virtual_path);
                if (!parsed) result.diagnostics.push_back({source.id, virtual_path, parsed.error().message});
                else collect_darkest_definitions(parsed.value(), source, virtual_path, result);
            } else if (extension == ".xml" && lower_virtual_path.find(".string_table.xml") != std::string::npos) {
                auto parsed = parse_localization_xml(read.value(), source, virtual_path);
                if (!parsed) result.diagnostics.push_back({source.id, virtual_path, parsed.error().message});
                else {
                    result.localizations.insert(result.localizations.end(),
                                                std::make_move_iterator(parsed.value().begin()),
                                                std::make_move_iterator(parsed.value().end()));
                }
            }
            result.source_files.push_back(std::move(source_file));
        }
    }
    return core::Result<void, core::Error>::success();
}

} // namespace

BaseContentScanConfig BaseContentScanConfig::defaults(std::filesystem::path root) {
    BaseContentScanConfig config;
    config.game_root = std::move(root);
    config.vanilla_directories = {
        "activity_log", "addons", "audio", "campaign", "colours", "curios", "cursors", "dungeons",
        "effects", "fe_flow", "fonts", "fx", "game_over", "heroes", "inventory", "loading_screen",
        "localization", "loot", "maps", "monsters", "overlays", "panels", "props", "raid",
        "raid_results", "scripts", "scrolls", "shared", "trinkets", "upgrades", "user_information", "video"};
    return config;
}

core::Result<BaseContentScanResult, core::Error>
BaseContentScanner::scan(const BaseContentScanConfig& config) const {
    BaseContentScanResult result;
    std::vector<ContentSourceRoot> sources;
    auto exists = file_system_.exists(config.game_root);
    if (!exists) return core::Result<BaseContentScanResult, core::Error>::failure(exists.error());
    if (!exists.value())
        return core::Result<BaseContentScanResult, core::Error>::failure(
            {core::ErrorCode::FileNotFound, "Game root directory does not exist", "BaseContentScanner",
             {{"path", config.game_root.string()}}});

    ContentSourceRoot vanilla{"vanilla", "Vanilla", ContentSourceType::Vanilla, config.game_root};
    std::vector<std::filesystem::path> vanilla_scan_roots;
    for (const auto& directory : config.vanilla_directories) {
        if (std::any_of(config.excluded_directories.begin(), config.excluded_directories.end(),
                        [&](const auto& excluded) { return lower_ascii(directory) == lower_ascii(excluded); }) ||
            lower_ascii(directory) == lower_ascii(config.dlc_directory)) continue;
        const auto path = config.game_root / directory;
        auto found = file_system_.exists(path);
        if (!found) return core::Result<BaseContentScanResult, core::Error>::failure(found.error());
        if (found.value()) vanilla_scan_roots.push_back(path);
    }
    if (!vanilla_scan_roots.empty()) sources.push_back(std::move(vanilla));

    const auto dlc_root = config.game_root / config.dlc_directory;
    auto has_dlc_root = file_system_.exists(dlc_root);
    if (!has_dlc_root) return core::Result<BaseContentScanResult, core::Error>::failure(has_dlc_root.error());
    if (has_dlc_root.value()) {
        auto dlc_directories = file_system_.list_directories(dlc_root);
        if (!dlc_directories) return core::Result<BaseContentScanResult, core::Error>::failure(dlc_directories.error());
        for (const auto& directory : dlc_directories.value()) {
            if (is_excluded_directory(config, directory)) continue;
            const auto name = directory.filename().string();
            sources.push_back({"dlc:" + name, name, ContentSourceType::Dlc, directory});
        }
    }

    std::sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) {
        if (a.type != b.type) return a.type < b.type;
        if (a.id == "vanilla") return true;
        if (b.id == "vanilla") return false;
        return a.id < b.id;
    });
    for (const auto& source : sources) {
        result.sources.push_back({source.id, source.name, source.type, source.path.string()});
        std::vector<std::filesystem::path> roots;
        if (source.id == "vanilla") roots = std::move(vanilla_scan_roots);
        else roots.push_back(source.path);
        auto collected = collect_files(file_system_, config, source, roots, result);
        if (!collected) return core::Result<BaseContentScanResult, core::Error>::failure(collected.error());
    }
    std::sort(result.source_files.begin(), result.source_files.end(), [](const auto& a, const auto& b) {
        return std::tie(a.source_id, a.virtual_path) < std::tie(b.source_id, b.virtual_path);
    });
    std::sort(result.definitions.begin(), result.definitions.end(), [](const auto& a, const auto& b) {
        return std::tie(a.kind, a.id, a.source_id, a.virtual_path, a.display_name, a.localization_key, a.payload_json) <
               std::tie(b.kind, b.id, b.source_id, b.virtual_path, b.display_name, b.localization_key, b.payload_json);
    });
    std::stable_sort(result.localizations.begin(), result.localizations.end(), [](const auto& a, const auto& b) {
        return std::tie(a.language, a.key, a.source_id, a.virtual_path) <
               std::tie(b.language, b.key, b.source_id, b.virtual_path);
    });
    std::sort(result.assets.begin(), result.assets.end(), [](const auto& a, const auto& b) {
        return std::tie(a.source_id, a.virtual_path) < std::tie(b.source_id, b.virtual_path);
    });
    std::sort(result.asset_references.begin(), result.asset_references.end(), [](const auto& a, const auto& b) {
        return std::tie(a.definition_type, a.content_id, a.source_id, a.definition_virtual_path,
                        a.asset_role, a.reference_type, a.virtual_path, a.reference_origin) <
               std::tie(b.definition_type, b.content_id, b.source_id, b.definition_virtual_path,
                        b.asset_role, b.reference_type, b.virtual_path, b.reference_origin);
    });
    result.asset_references.erase(std::unique(result.asset_references.begin(), result.asset_references.end(),
        [](const auto& a, const auto& b) {
            return std::tie(a.definition_type, a.content_id, a.source_id, a.definition_virtual_path,
                            a.asset_role, a.reference_type, a.virtual_path, a.reference_origin) ==
                   std::tie(b.definition_type, b.content_id, b.source_id, b.definition_virtual_path,
                            b.asset_role, b.reference_type, b.virtual_path, b.reference_origin);
        }), result.asset_references.end());
    std::sort(result.relationships.begin(), result.relationships.end(), [](const auto& a, const auto& b) {
        return std::tie(a.parent_type, a.parent_id, a.relationship_type, a.child_type, a.child_id,
                        a.source_id, a.virtual_path) <
               std::tie(b.parent_type, b.parent_id, b.relationship_type, b.child_type, b.child_id,
                        b.source_id, b.virtual_path);
    });
    result.relationships.erase(std::unique(result.relationships.begin(), result.relationships.end(),
        [](const auto& a, const auto& b) {
            return std::tie(a.parent_type, a.parent_id, a.relationship_type, a.child_type, a.child_id,
                            a.source_id, a.virtual_path) ==
                   std::tie(b.parent_type, b.parent_id, b.relationship_type, b.child_type, b.child_id,
                            b.source_id, b.virtual_path);
        }), result.relationships.end());
    std::sort(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& a, const auto& b) {
        return std::tie(a.source_id, a.virtual_path) < std::tie(b.source_id, b.virtual_path);
    });
    return core::Result<BaseContentScanResult, core::Error>::success(std::move(result));
}

std::string_view to_string(ContentSourceType type) noexcept {
    switch (type) {
    case ContentSourceType::Vanilla: return "Vanilla";
    case ContentSourceType::Dlc: return "Dlc";
    }
    return "Unknown";
}

} // namespace ddse::application
