#include "ddse/application/campaign_model_builder.hpp"
#include "ddse/application/content_environment.hpp"
#include "ddse/application/content_scanner.hpp"
#include "ddse/application/mod_environment.hpp"
#include "ddse/application/save_profile.hpp"
#include "ddse/core/dson/dson_document_editor.hpp"
#include "ddse/core/dson/dson_reader.hpp"
#include "ddse/core/dson/dson_writer.hpp"
#include "ddse/infrastructure/base_content_database.hpp"
#include "ddse/infrastructure/mod_environment_database.hpp"
#include "ddse/infrastructure/native_file_system.hpp"
#include "ddse/infrastructure/sqlite_content_environment.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
using namespace ddse;
using DsonDocument = core::dson::DsonDocument;
using DsonField = core::dson::DsonField;
using Json = nlohmann::json;

struct Options {
    std::filesystem::path source{"test_save_profile/profile_0"};
    std::filesystem::path game{R"(D:\SteamLibrary\steamapps\common\DarkestDungeon)"};
    std::filesystem::path workshop{R"(D:\SteamLibrary\steamapps\workshop\content\262060)"};
    std::filesystem::path local{R"(D:\SteamLibrary\steamapps\common\DarkestDungeon\modes)"};
    std::filesystem::path output{"test_save_profile/stage11_advanced_game_tests"};
};

struct Scenario {
    std::string directory;
    std::string title;
    std::string detail;
    std::map<std::string, std::vector<std::byte>, std::less<>> documents;
};

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }

Options options_from(const std::vector<std::string>& args) {
    Options result;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i + 1 >= args.size()) fail("Missing argument value after " + args[i]);
        const auto key = args[i];
        const auto& encoded = args[++i];
        auto next = std::filesystem::path(std::u8string(
            reinterpret_cast<const char8_t*>(encoded.data()), encoded.size()));
        if (key == "--source-profile") result.source = next;
        else if (key == "--game-root") result.game = next;
        else if (key == "--workshop-root") result.workshop = next;
        else if (key == "--local-mod-root") result.local = next;
        else if (key == "--output-root") result.output = next;
        else fail("Unknown argument: " + key);
    }
    return result;
}

template <class T>
T must(core::Result<T, core::Error> result, std::string_view operation) {
    if (!result) fail(std::string{operation} + ": " + result.error().message);
    return std::move(result).value();
}
void must(core::Result<void, core::Error> result, std::string_view operation) {
    if (!result) fail(std::string{operation} + ": " + result.error().message);
}

std::filesystem::path absolute_path(const std::filesystem::path& path) {
    std::error_code error;
    auto result = std::filesystem::absolute(path, error);
    if (error) fail("Cannot resolve path " + path.string() + ": " + error.message());
    return result.lexically_normal();
}

std::string path_key(const std::filesystem::path& path) {
    auto value = absolute_path(path).generic_string();
#ifdef _WIN32
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    return value;
}
bool is_within(const std::filesystem::path& a, const std::filesystem::path& b) {
    const auto child = path_key(a), parent = path_key(b);
    return child == parent || (child.size() > parent.size() && child.starts_with(parent) && child[parent.size()] == '/');
}

const application::RawSaveDocument& doc(const application::RawSaveProfile& profile, std::string_view id) {
    auto it = profile.documents.find(std::string{id});
    if (it == profile.documents.end() || !it->second.decoded) fail("Missing decoded save document: " + std::string{id});
    return it->second;
}
DsonDocument clone_doc(const DsonDocument& source) {
    auto copy = source;
    for (std::size_t i = 0; i < source.fields.size(); ++i)
        if (source.fields[i].embedded_document)
            copy.fields[i].embedded_document = std::make_shared<DsonDocument>(clone_doc(*source.fields[i].embedded_document));
    return copy;
}
DsonField& field(DsonDocument& document, std::string_view path) {
    auto it = std::find_if(document.fields.begin(), document.fields.end(), [&](const auto& f) { return f.path == path; });
    if (it == document.fields.end()) fail("Missing DSON path: " + std::string{path});
    return *it;
}
const DsonField& field(const DsonDocument& document, std::string_view path) {
    auto it = std::find_if(document.fields.begin(), document.fields.end(), [&](const auto& f) { return f.path == path; });
    if (it == document.fields.end()) fail("Missing DSON path: " + std::string{path});
    return *it;
}
bool has_field(const DsonDocument& document, std::string_view path) {
    return std::any_of(document.fields.begin(), document.fields.end(), [&](const auto& f) { return f.path == path; });
}
void set_int(DsonDocument& d, std::string_view p, std::int32_t v) {
    auto& f = field(d,p); if (f.kind != core::dson::ValueKind::Integer) fail("Expected integer at " + std::string{p}); f.replace_value(v);
}
void set_bool(DsonDocument& d, std::string_view p, bool v) {
    auto& f = field(d,p); if (f.kind != core::dson::ValueKind::Boolean) fail("Expected boolean at " + std::string{p}); f.replace_value(v);
}
void set_char(DsonDocument& d, std::string_view p, char v) {
    auto& f = field(d,p); if (f.kind != core::dson::ValueKind::Character) fail("Expected character at " + std::string{p}); f.replace_value(v);
}
void set_string(DsonDocument& d, std::string_view p, std::string v) {
    auto& f = field(d,p); if (f.kind != core::dson::ValueKind::String) fail("Expected string at " + std::string{p}); f.replace_value(std::move(v));
}
std::vector<std::byte> encode(const DsonDocument& d, std::string_view id) {
    auto bytes = must(core::dson::DsonWriter{}.encode(d), "Encode " + std::string{id});
    (void)must(core::dson::DsonReader{}.parse(bytes, id), "DSON read-back " + std::string{id});
    return bytes;
}
std::string bytes_string(const std::vector<std::byte>& bytes) {
    std::string out(bytes.size(), '\0'); if (!bytes.empty()) std::memcpy(out.data(), bytes.data(), bytes.size()); return out;
}

std::size_t numeric_key(std::string_view text) {
    std::size_t value{};
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && ptr == text.data() + text.size() ? value : std::numeric_limits<std::size_t>::max();
}
std::string hero_base(std::string_view id) { return "base_root/heroes/" + std::string{id} + "/hero_file_data/raw_data"; }
DsonDocument& hero_data(DsonDocument& roster, std::string_view id) {
    auto& data = field(roster, hero_base(id)); if (!data.embedded_document) fail("Hero embedded DSON is unavailable: " + std::string{id});
    return *data.embedded_document;
}
std::int32_t signed_hash(std::string_view value) { return static_cast<std::int32_t>(core::dson::string_hash(value)); }

struct PurchaseRow { std::string path; std::int32_t instance{}; std::int32_t tree{}; char code{}; bool purchased{}; };
std::vector<PurchaseRow> purchase_rows(const DsonDocument& d) {
    std::vector<PurchaseRow> rows;
    const auto& root = field(d, "base_root/purchases");
    for (auto index : root.children) {
        const auto& item = d.fields.at(index); if (item.kind != core::dson::ValueKind::Object) continue;
        const auto base = item.path;
        const DsonField* instance=nullptr; const DsonField* tree=nullptr; const DsonField* code=nullptr; const DsonField* purchased=nullptr;
        for(auto child:item.children) {
            const auto& value=d.fields.at(child);
            if(value.name=="instance_number") instance=&value;
            else if(value.name=="tree_id") tree=&value;
            else if(value.name=="requirement_code") code=&value;
            else if(value.name=="is_purchased") purchased=&value;
        }
        if(!instance||!tree||!code||!purchased) continue;
        const auto* iv = std::get_if<std::int32_t>(&instance->value); const auto* tv = std::get_if<std::int32_t>(&tree->value);
        const auto* cv = std::get_if<char>(&code->value); const auto* bv = std::get_if<bool>(&purchased->value);
        if (iv && tv && cv && bv) rows.push_back({base,*iv,*tv,*cv,*bv});
    }
    return rows;
}
bool purchase_state(const std::vector<PurchaseRow>& rows, std::int32_t instance, std::int32_t tree, char code) {
    auto it = std::find_if(rows.begin(), rows.end(), [&](const auto& r) { return r.instance==instance && r.tree==tree && r.code==code; });
    return it != rows.end() && it->purchased;
}
std::int32_t instance_for(const DsonDocument& upgrades, const domain::Hero& hero) {
    if (!hero.class_id.value) fail("Hero has no class id");
    const auto rows = purchase_rows(upgrades); const auto wanted = signed_hash(*hero.class_id.value + ".weapon");
    std::set<std::int32_t> matches;
    for (const auto& r : rows) if (r.tree == wanted) matches.insert(r.instance);
    if (matches.size() != 1) fail("Cannot uniquely map hero to purchase instance: " + *hero.class_id.value + " matches=" + std::to_string(matches.size()));
    return *matches.begin();
}
void set_purchase(DsonDocument& upgrades, std::int32_t instance, std::string_view tree_name, char code, bool value) {
    const auto tree = signed_hash(tree_name); auto rows = purchase_rows(upgrades);
    auto it = std::find_if(rows.begin(), rows.end(), [&](const auto& r) { return r.instance==instance && r.tree==tree && r.code==code; });
    if (it != rows.end()) { set_bool(upgrades,it->path+"/is_purchased",value); return; }
    const auto& root = field(upgrades,"base_root/purchases");
    std::size_t next = 0; for (auto child : root.children) next = std::max(next, numeric_key(upgrades.fields.at(child).name)+1);
    if (root.children.empty()) fail("Cannot append purchase row: purchase table has no template rows");
    const auto template_path = upgrades.fields.at(root.children.front()).path;
    const auto new_path = "base_root/purchases/" + std::to_string(next);
    auto added = core::dson::DsonDocumentEditor::append_clone(upgrades,"base_root/purchases",upgrades,template_path,std::to_string(next));
    if (!added) fail("Cannot add purchase node: " + added.error().message);
    set_int(upgrades,new_path+"/instance_number",instance); set_int(upgrades,new_path+"/tree_id",tree);
    set_char(upgrades,new_path+"/requirement_code",code); set_bool(upgrades,new_path+"/is_purchased",value);
}
void set_rank(DsonDocument& upgrades, std::int32_t instance, std::string_view tree, std::int32_t raw_rank) {
    if (raw_rank < 0 || raw_rank > 20) fail("Unsafe rank requested");
    const auto existing_rows=purchase_rows(upgrades);
    for (char code='0'; code<'0'+20; ++code) {
        bool value = code < '0' + raw_rank;
        if (value || purchase_state(existing_rows,instance,signed_hash(tree),code))
            set_purchase(upgrades,instance,tree,code,value);
    }
}
std::int32_t current_rank(const std::vector<PurchaseRow>& rows, std::int32_t instance, std::string_view tree) {
    const auto tree_hash=signed_hash(tree);
    std::set<char> purchased;
    for(const auto& row:rows) if(row.instance==instance&&row.tree==tree_hash&&row.purchased) purchased.insert(row.code);
    std::int32_t rank=0; while(rank<20&&purchased.contains(static_cast<char>('0'+rank))) ++rank;
    return rank;
}

std::vector<application::ContentDefinition> list(const application::IContentEnvironment& env, std::string_view type) {
    return must(env.list_content(type),"List " + std::string{type});
}
std::string pretty(const application::IContentEnvironment& env, const application::ContentDefinition& d) {
    if (!d.localization_key.empty()) {
        auto localized=env.resolve_localization(d.localization_key,"english");
        if (localized && localized.value() && !localized.value()->value.empty()) return localized.value()->value;
    }
    if (!d.display_name.empty()) return d.display_name;
    return d.id;
}
std::optional<std::int32_t> effective_combat_skill_level_count(
    const application::IContentEnvironment& env, const application::IFileSystem& fs,
    const domain::Hero& hero, const application::ContentDefinition& skill) {
    if (!hero.class_id.value) return std::nullopt;
    const auto suffix = skill.id.substr(skill.id.find(':') + 1);
    const auto tree_id = *hero.class_id.value + "." + suffix;
    const auto virtual_path = "upgrades/heroes/" + *hero.class_id.value + ".upgrades.json";
    auto resolved = must(env.resolve_asset(virtual_path), "Resolve effective hero upgrade tree " + virtual_path);
    if (!resolved) return std::nullopt;
    const auto bytes = must(fs.read_file(resolved->physical_path), "Read effective hero upgrade tree " + virtual_path);
    const auto trees_doc = Json::parse(bytes, nullptr, false);
    if (!trees_doc.is_object() || !trees_doc.contains("trees") || !trees_doc["trees"].is_array())
        fail("Malformed effective upgrade tree file: " + path_key(resolved->physical_path));
    const Json* target = nullptr;
    for (const auto& tree : trees_doc["trees"]) {
        if (tree.is_object() && tree.value("id", std::string{}) == tree_id) {
            target = &tree;
            break;
        }
    }
    if (!target) return std::nullopt; // Some combat skills are intentionally not upgradeable.
    if (!target->contains("requirements") || !(*target)["requirements"].is_array())
        fail("Combat skill upgrade tree has no requirements array: " + tree_id);

    std::set<std::int32_t> skill_levels;
    const auto skill_doc = Json::parse(skill.payload_json, nullptr, false);
    if (!skill_doc.is_object() || !skill_doc.contains("levels") || !skill_doc["levels"].is_array())
        fail("Combat skill definition has no levels array: " + skill.id);
    for (const auto& level_record : skill_doc["levels"]) {
        if (!level_record.is_object() || !level_record.contains("level")) continue;
        const auto& encoded = level_record["level"];
        const Json* scalar = &encoded;
        if (encoded.is_array()) {
            if (encoded.empty()) continue;
            scalar = &encoded.front();
        }
        std::int32_t level{};
        if (scalar->is_number_integer()) level = scalar->get<std::int32_t>();
        else if (scalar->is_string()) {
            const auto value = scalar->get<std::string>();
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), level);
            if (error != std::errc{} || end != value.data() + value.size()) continue;
        } else continue;
        skill_levels.insert(level);
    }
    if (skill_levels.empty()) fail("Combat skill has no readable level values: " + skill.id);
    std::int32_t next_level = 0;
    for (const auto level : skill_levels) {
        if (level != next_level++) fail("Combat skill level values are not contiguous from zero: " + skill.id);
    }

    const auto& requirements = (*target)["requirements"];
    if (requirements.empty() || requirements.size() != skill_levels.size() || requirements.size() > 20)
        fail("Skill levels and effective upgrade nodes disagree for " + tree_id +
             " (levels=" + std::to_string(skill_levels.size()) +
             ", nodes=" + std::to_string(requirements.size()) + ")");
    for (std::size_t i = 0; i < requirements.size(); ++i) {
        if (!requirements[i].is_object() || requirements[i].value("code", std::string{}) != std::string(1, static_cast<char>('0' + i)))
            fail("Combat skill requirement codes are not contiguous for " + tree_id);
    }
    return static_cast<std::int32_t>(skill_levels.size());
}
std::string mod_name(const application::ModEnvironmentScanResult& scan, std::string_view id) {
    auto it=std::find_if(scan.mods.begin(),scan.mods.end(),[&](const auto& m){return m.id==id;});
    return it==scan.mods.end()||it->display_name.empty()?std::string{id}:it->display_name;
}
const domain::Hero& hero_at(const domain::CampaignModel& model, std::size_t pos) {
    auto it=std::find_if(model.heroes.begin(),model.heroes.end(),[&](const auto& h){return h.roster_position==pos;});
    if(it==model.heroes.end()) fail("Roster position not found: "+std::to_string(pos+1));
    return *it;
}
std::string hero_label(const domain::Hero& h) {
    return h.name.value.value_or("(unnamed)")+" — "+h.class_id.value.value_or("unknown")+" (名单第"+std::to_string(h.roster_position+1)+"位, id="+h.persistent_id+")";
}
std::string def_payload(const domain::HeroQuirk& q) { return q.definition.definition_payload_json; }

Scenario scenario(std::string dir,std::string title,std::string detail,std::map<std::string,std::vector<std::byte>,std::less<>> docs) {
    return {std::move(dir),std::move(title),std::move(detail),std::move(docs)};
}
std::vector<std::byte> encoded_doc(const DsonDocument& d, std::string_view id) { return encode(d,id); }

Scenario make_combat_upgrade(const application::RawSaveProfile& p,const application::IContentEnvironment& env,const domain::Hero& hero,
                             const application::ContentDefinition& skill,const std::vector<PurchaseRow>& rows,
                             std::int32_t instance,std::int32_t max_levels) {
    auto upgrades=clone_doc(*doc(p,"persist.upgrades.json").decoded);
    const auto tree=*hero.class_id.value+"."+skill.id.substr(skill.id.find(':')+1);
    const auto before=current_rank(rows,instance,tree);
    for(std::int32_t i=0;i<max_levels;++i) set_purchase(upgrades,instance,tree,static_cast<char>('0'+i),true);
    std::ostringstream text; text<<"- 英雄：**"<<hero_label(hero)<<"**\n- 战斗技能：`"<<skill.id<<"`（"<<pretty(env,skill)<<"），当前生效定义有"<<max_levels<<"个等级节点。\n- 购买节点："<<before<<" → "<<max_levels<<"（满级）；英雄经验与等级字段不变。\n";
    return scenario("01_combat_skill_upgrade","升级战斗技能至该技能上限",text.str(),{{"persist.upgrades.json",encoded_doc(upgrades,"persist.upgrades.json")}});
}

Scenario make_combat_downgrade(const application::RawSaveProfile& p,const domain::Hero& hero,
                               const application::ContentDefinition& skill,const std::vector<PurchaseRow>& rows,std::int32_t instance,
                               std::int32_t max_levels) {
    if(max_levels<3) fail("Skill cannot be lowered to level 2: "+skill.id);
    auto tree=*hero.class_id.value+"."+skill.id.substr(skill.id.find(':')+1);
    auto upgrades=clone_doc(*doc(p,"persist.upgrades.json").decoded); set_rank(upgrades,instance,tree,2);
    auto text="- 英雄：**"+hero_label(hero)+"**\n- 战斗技能：`"+skill.id+"`，从"+std::to_string(max_levels)+"级满级降为2级；保留前2个购买节点，后续节点回锁。\n- 源存档原购买节点数："+std::to_string(current_rank(rows,instance,tree))+"；英雄等级、经验不变。\n";
    return scenario("02_combat_skill_downgrade","战斗技能满级降至2级并回锁后续节点",text,{{"persist.upgrades.json",encoded_doc(upgrades,"persist.upgrades.json")}});
}
Scenario make_combat_max_prerequisite(const application::RawSaveProfile& p,const domain::Hero& hero,
                                      const application::ContentDefinition& skill,std::int32_t instance,std::int32_t levels) {
    if(levels<3||levels>20) fail("Invalid skill maximum: "+skill.id);
    const auto tree=*hero.class_id.value+"."+skill.id.substr(skill.id.find(':')+1);
    auto upgrades=clone_doc(*doc(p,"persist.upgrades.json").decoded);
    for(std::int32_t rank=0;rank<levels;++rank) set_purchase(upgrades,instance,tree,static_cast<char>('0'+rank),true);
    return scenario("02a_combat_skill_max_prerequisite","第2项前置：将目标技能升至满级",
        "- 英雄：**"+hero_label(hero)+"**\n- 技能：`"+skill.id+"`，本 mod 定义上限为"+std::to_string(levels)+"级。\n- 此前置存档用于先确认满级状态；完成后再载入 `02_combat_skill_downgrade` 检查降至2级。\n",
        {{"persist.upgrades.json",encoded_doc(upgrades,"persist.upgrades.json")}});
}

std::vector<std::byte> change_inner(const application::RawSaveProfile& p,const domain::Hero& hero,
                                   const std::function<void(DsonDocument&)>& edit) {
    auto roster=clone_doc(*doc(p,"persist.roster.json").decoded); edit(hero_data(roster,hero.persistent_id));
    return encoded_doc(roster,"persist.roster.json");
}

Scenario make_camping(const application::RawSaveProfile& p,const domain::Hero& hero,std::string skill_id,bool unlock) {
    auto roster=clone_doc(*doc(p,"persist.roster.json").decoded); auto& inner=hero_data(roster,hero.persistent_id);
    const std::string map_path="base_root/skills/selected_camping_skills"; const auto entry_path=map_path+"/"+skill_id;
    const bool present=has_field(inner,entry_path);
    if(unlock&&present) fail("Camping skill is already stored as unlocked: "+skill_id);
    if(!unlock&&!present) fail("Camping skill is not currently unlocked: "+skill_id);
    if(unlock) {
        const auto& map=field(inner,map_path); if(map.children.empty()) fail("No camping skill entry template exists");
        const auto template_path=inner.fields.at(map.children.front()).path;
        auto added=core::dson::DsonDocumentEditor::append_clone(inner,map_path,inner,template_path,skill_id);
        if(!added) fail("Could not add camping skill to hero: "+added.error().message);
        set_int(inner,entry_path,0);
    } else {
        auto erased=core::dson::DsonDocumentEditor::erase(inner,entry_path);
        if(!erased) fail("Could not lock camping skill: "+erased.error().message);
    }
    auto text="- 英雄：**"+hero_label(hero)+"**\n- 生存技能：`"+skill_id+"`，"+(unlock?"加入已解锁技能集合":"从已解锁技能集合移除")+"。\n- 经验值与其他技能条目不变。\n";
    return scenario(unlock?"03_camping_skill_unlock":"04_camping_skill_lock",unlock?"解锁一个生存技能":"锁定一个生存技能",text,
                    {{"persist.roster.json",encoded_doc(roster,"persist.roster.json")}});
}

Scenario make_lock_quirk(const application::RawSaveProfile& p,const domain::Hero& hero,const domain::HeroQuirk& quirk) {
    auto json=Json::parse(def_payload(quirk),nullptr,false);
    if(quirk.is_disease||quirk.polarity!=domain::QuirkPolarity::Positive||json.is_discarded()||!json.value("can_modify_in_activity",false))
        fail("Selected quirk is not eligible for a lock test: "+quirk.id);
    auto bytes=change_inner(p,hero,[&](DsonDocument& inner){set_bool(inner,"base_root/quirks/"+quirk.id+"/is_locked",true);});
    auto text="- 英雄：**"+hero_label(hero)+"**\n- 正面怪癖：`"+quirk.id+"` 锁定。\n- 已检查定义的 `can_modify_in_activity=true`。\n";
    return scenario("05_lock_positive_quirk","锁定可锁定正面怪癖",text,{{"persist.roster.json",std::move(bytes)}});
}
Scenario make_remove_quirks(const application::RawSaveProfile& p,const domain::Hero& hero) {
    auto pos=std::find_if(hero.quirks.begin(),hero.quirks.end(),[](const auto& q){return !q.is_disease&&q.polarity==domain::QuirkPolarity::Positive;});
    auto neg=std::find_if(hero.quirks.begin(),hero.quirks.end(),[](const auto& q){return !q.is_disease&&q.polarity==domain::QuirkPolarity::Negative;});
    if(pos==hero.quirks.end()||neg==hero.quirks.end()) fail("Hero lacks both positive and negative quirks for remove test");
    auto bytes=change_inner(p,hero,[&](DsonDocument& inner){
        for(const auto* q:{&*pos,&*neg}) { auto r=core::dson::DsonDocumentEditor::erase(inner,"base_root/quirks/"+q->id); if(!r) fail("Could not remove quirk: "+r.error().message); }
    });
    auto text="- 英雄：**"+hero_label(hero)+"**\n- 移除正面怪癖 `"+pos->id+"` 与负面怪癖 `"+neg->id+"`。即使原本锁定也直接移除。\n";
    return scenario("06_remove_two_quirks","移除一个正面和一个负面怪癖",text,{{"persist.roster.json",std::move(bytes)}});
}

Scenario make_gear(const application::RawSaveProfile& p,const domain::Hero& hero,std::int32_t new_weapon,std::int32_t new_armour,
                   std::int32_t instance,bool upgrade) {
    auto roster=clone_doc(*doc(p,"persist.roster.json").decoded); auto& inner=hero_data(roster,hero.persistent_id);
    set_int(inner,"base_root/weapon_rank",new_weapon); set_int(inner,"base_root/armour_rank",new_armour);
    auto upgrades=clone_doc(*doc(p,"persist.upgrades.json").decoded);
    set_rank(upgrades,instance,*hero.class_id.value+".weapon",new_weapon);
    set_rank(upgrades,instance,*hero.class_id.value+".armour",new_armour);
    std::ostringstream text; text<<"- 英雄：**"<<hero_label(hero)<<"**\n- 武器等级："<<hero.weapon_rank.value.value_or(-1)+1<<" → "<<new_weapon+1
       <<"；防具等级："<<hero.armour_rank.value.value_or(-1)+1<<" → "<<new_armour+1<<"。\n- 同步修改英雄字段及武器/防具购买节点。\n";
    auto prefix=upgrade?"07":"08";
    return scenario(std::string{prefix}+(upgrade?"_gear_upgrade":"_gear_downgrade"),upgrade?"升级武器与防具":"降级武器与防具",text.str(),
                    {{"persist.roster.json",encoded_doc(roster,"persist.roster.json")},{"persist.upgrades.json",encoded_doc(upgrades,"persist.upgrades.json")}});
}

Scenario make_add_disease(const application::RawSaveProfile& p,const application::IContentEnvironment& env,
                          const domain::Hero& hero,const application::ContentDefinition& disease) {
    auto bytes=change_inner(p,hero,[&](DsonDocument& inner){
        const std::string quirks="base_root/quirks"; const auto& qroot=field(inner,quirks);
        if(!qroot.children.empty()) {
            const auto template_path=inner.fields.at(qroot.children.front()).path;
            auto a=core::dson::DsonDocumentEditor::append_clone(inner,quirks,inner,template_path,disease.id);
            if(!a) fail("Cannot add disease object: "+a.error().message);
            auto root=quirks+"/"+disease.id;
            if(has_field(inner,root+"/is_disease")) set_bool(inner,root+"/is_disease",true);
            if(has_field(inner,root+"/is_locked")) set_bool(inner,root+"/is_locked",false);
            if(has_field(inner,root+"/is_new")) set_bool(inner,root+"/is_new",true);
            if(has_field(inner,root+"/trinketId")) set_int(inner,root+"/trinketId",-1);
        } else fail("No quirk object template is available to represent the disease");
    });
    auto text="- 英雄：**"+hero_label(hero)+"**\n- 新增原版疾病：**"+pretty(env,disease)+"**（`"+disease.id+"`；定义来源 `"+disease.provenance.source_id+"`）。\n";
    return scenario("09_add_vanilla_disease","添加一种原版疾病",text,{{"persist.roster.json",std::move(bytes)}});
}

Scenario make_level_swap(const application::RawSaveProfile& p,const domain::Hero& high,const domain::Hero& low) {
    constexpr std::int32_t high_xp=14,low_xp=8; auto roster=clone_doc(*doc(p,"persist.roster.json").decoded);
    set_int(hero_data(roster,high.persistent_id),"base_root/resolveXp",high_xp);
    set_int(hero_data(roster,low.persistent_id),"base_root/resolveXp",low_xp);
    auto text="- 降级：**"+hero_label(high)+"**，Resolve 6 → 3，XP "+std::to_string(high.resolve_xp.value.value_or(-1))+" → 14。\n- 升级：**"+hero_label(low)+"**，Resolve 1 → 2，XP "+std::to_string(low.resolve_xp.value.value_or(-1))+" → 8。\n- 按原版经验阈值定位等级，只改经验字段。\n";
    return scenario("11_swap_hero_levels","调整一高一低两名英雄等级",text,{{"persist.roster.json",encoded_doc(roster,"persist.roster.json")}});
}

Scenario make_building_downgrade(const application::RawSaveProfile& p) {
    constexpr std::int32_t estate_instance=0; auto upgrades=clone_doc(*doc(p,"persist.upgrades.json").decoded);
    const std::string tree="stage_coach.rostersize";
    const auto rows=purchase_rows(*doc(p,"persist.upgrades.json").decoded); std::int32_t before=0;
    for(char c='a';c<='e';++c) if(purchase_state(rows,estate_instance,signed_hash(tree),c)) ++before;
    if(before<2) fail("Stagecoach roster size is not upgraded enough for test 15");
    for(char c='a';c<='e';++c) {
        const bool value=c=='a';
        if(value || purchase_state(rows,estate_instance,signed_hash(tree),c)) set_purchase(upgrades,estate_instance,tree,c,value);
    }
    return scenario("15_downgrade_town_upgrade","马车名单容量降至1级并回锁后续节点",
        "- 建筑：**马车（Stagecoach）**，名单容量 `stage_coach.rostersize`。\n- 已购买节点："+std::to_string(before)+" → 1；保留 a，回锁 b–e。\n",
        {{"persist.upgrades.json",encoded_doc(upgrades,"persist.upgrades.json")}});
}

std::string item_id_path(const DsonDocument& hero,const std::string& path) {
    if(has_field(hero,path+"/id")) return path+"/id";
    const auto& f=field(hero,path); if(f.kind==core::dson::ValueKind::String) return path;
    fail("Unsupported hero trinket item record: "+path);
}
void replace_item_id(DsonDocument& hero,const std::string& path,std::string id) {
    auto item_id=item_id_path(hero,path); set_string(hero,item_id,std::move(id));
}
void clear_object_children(DsonDocument& document,const std::string& path) {
    while(true) {
        const auto& parent=field(document,path); if(parent.children.empty()) return;
        const auto child_path=document.fields.at(parent.children.front()).path;
        auto erased=core::dson::DsonDocumentEditor::erase(document,child_path);
        if(!erased) fail("Could not clear cloned object "+child_path+": "+erased.error().message);
    }
}
std::string hero_item_path(const DsonDocument& inner,std::string_view id,bool last_match=false) {
    const auto& items=field(inner,"base_root/trinkets/items");
    std::vector<std::string> matches;
    for(auto index:items.children) {
        const auto& item=inner.fields.at(index); std::string stored=item.name;
        if(item.kind==core::dson::ValueKind::Object&&has_field(inner,item.path+"/id")) {
            if(const auto* value=std::get_if<std::string>(&field(inner,item.path+"/id").value)) stored=*value;
        } else if(const auto* value=std::get_if<std::string>(&item.value)) stored=*value;
        if(stored==id) matches.push_back(item.path);
    }
    if(matches.empty()) fail("Could not resolve equipped trinket in hero DSON: "+std::string{id});
    return last_match?matches.back():matches.front();
}
Scenario make_destroy_hero_trinket(const application::RawSaveProfile& p,const domain::Hero& hero) {
    if(hero.trinkets.empty()) fail("Hero has no equipped trinket to destroy");
    auto roster=clone_doc(*doc(p,"persist.roster.json").decoded); auto& inner=hero_data(roster,hero.persistent_id);
    const auto path=hero_item_path(inner,hero.trinkets.back().id,true);
    auto erased=core::dson::DsonDocumentEditor::erase(inner,path); if(!erased) fail("Could not destroy last equipped trinket: "+erased.error().message);
    return scenario("17_destroy_hero_trinket","销毁英雄装备栏最后一个饰品","- 英雄：**"+hero_label(hero)+"**\n- 销毁最后一个已装备饰品 `"+hero.trinkets.back().id+"`，不会送回仓库。\n",
        {{"persist.roster.json",encoded_doc(roster,"persist.roster.json")}});
}
Scenario make_destroy_inventory_trinket(const application::RawSaveProfile& p,const domain::CampaignModel& model) {
    if(model.trinket_inventory.empty()) fail("Estate inventory has no trinket to destroy");
    const auto& first=model.trinket_inventory.front(); auto estate=clone_doc(*doc(p,"persist.estate.json").decoded);
    auto erased=core::dson::DsonDocumentEditor::erase(estate,"base_root/trinkets/items/"+first.raw_key);
    if(!erased) fail("Could not destroy first inventory trinket: "+erased.error().message);
    return scenario("20_destroy_first_inventory_trinket","销毁饰品物品栏第一个饰品","- 销毁物品栏第1个饰品：`"+first.id.value.value_or("unknown")+"`（原始键 `"+first.raw_key+"`），不只是移除或转移。\n",
        {{"persist.estate.json",encoded_doc(estate,"persist.estate.json")}});
}

std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary); if(!in) fail("Cannot read "+path.string());
    return {std::istreambuf_iterator<char>{in},std::istreambuf_iterator<char>{}};
}
void copy_tree(const std::filesystem::path& source,const std::filesystem::path& target) {
    if(std::filesystem::exists(target)) fail("Refusing to overwrite profile: "+target.string());
    std::filesystem::create_directories(target);
    for(const auto& entry:std::filesystem::recursive_directory_iterator(source)) {
        if(entry.is_symlink()) fail("Symlink in save profile: "+entry.path().string());
        const auto dest=target/entry.path().lexically_relative(source);
        if(entry.is_directory()) std::filesystem::create_directories(dest);
        else if(entry.is_regular_file()) {std::filesystem::create_directories(dest.parent_path());std::filesystem::copy_file(entry.path(),dest);}
        else fail("Unsupported save profile entry: "+entry.path().string());
    }
}
void validate_unchanged_files(const std::filesystem::path& source,const std::filesystem::path& target,
                              const std::set<std::string,std::less<>>& changed) {
    std::set<std::string,std::less<>> original,copy;
    for(const auto& e:std::filesystem::recursive_directory_iterator(source)) if(e.is_regular_file()) original.insert(e.path().lexically_relative(source).generic_string());
    for(const auto& e:std::filesystem::recursive_directory_iterator(target)) if(e.is_regular_file()) copy.insert(e.path().lexically_relative(target).generic_string());
    if(original!=copy) fail("Generated profile file set differs from source");
    for(const auto& relative:original) if(!changed.contains(relative)&&file_bytes(source/relative)!=file_bytes(target/relative)) fail("Unrelated file changed: "+relative);
}

std::string readme(const application::RawSaveProfile& source,const application::ModEnvironmentScanResult& mods,
                   const std::vector<Scenario>& scenarios,std::uint64_t backup_fingerprint,const Options& opts) {
    std::ostringstream out;
    out<<"# Stage 11 进阶修改测试存档\n\n每个测试 profile 均独立从源存档复制生成；`00_control` 是逐字节对照组。源存档与备份只读。测试 profile 需要逐个放入游戏测试。\n\n"
       <<"- 源存档：`"<<absolute_path(opts.source).string()<<"`，指纹 `0x"<<std::hex<<source.baseline_fingerprint<<std::dec<<"`\n"
       <<"- 备份指纹：`0x"<<std::hex<<backup_fingerprint<<std::dec<<"`\n"
       <<"- Mod 顺序取自存档 `persist.game.json/applied_ugcs_1_0`；启用 "<<mods.effective_order.size()<<" 个，扫描到 "<<mods.mods.size()<<" 个已安装 mod，解析诊断 "<<mods.diagnostics.size()<<" 条。\n"
       <<"- 本次生成：1–9、11、15–18、20。10、12–14、19 按已确认的依赖关系暂缓。\n\n"
       <<"## 建议检查顺序\n\n"
       <<"1. 用 `00_control/profile_0` 确认当前存档仍可进入城镇。\n2. 一次只替换一个测试 profile；退出游戏后再换下一个。\n"
       <<"3. 逐项检查下面列出的英雄、技能、等级、建筑或饰品；保存后退出并重新载入，确认变更持久。\n"
       <<"4. 15通过后再生成14；18通过并提供游戏保存后的 profile 后，再生成19及12、13；9通过后再基于通过后的存档生成10。\n\n"
       <<"## 测试清单\n\n";
    for(const auto& s:scenarios) out<<"### `"<<s.directory<<"/profile_0` — "<<s.title<<"\n\n"<<s.detail<<"\n**请确认：**存档正常载入、目标改动显示正确、没有关联数据异常；保存并重载后仍成立。\n\n";
    out<<"## 暂缓项目\n\n- **10**：当前源存档没有疾病英雄；等9通过后从带疾病的游戏保存档生成。\n- **12–13**：具体小镇建筑解锁/锁定，依赖18解锁小镇建筑系统并由你在游戏内确认。\n- **14**：等15的升级节点回锁测试通过后再生成。\n- **19**：等你确认18通过并提供游戏保存后的存档，再基于该状态重新锁定系统。\n\n"
       <<"## Mod 启用顺序\n\n";
    for(const auto& entry:mods.save_order) {
        const auto id=entry.matched_mod_id.empty()?entry.identity:entry.matched_mod_id;
        out<<entry.position+1<<". `"<<entry.identity<<"` — "<<mod_name(mods,id)<<"\n";
    }
    out<<"\n生成器：`scripts/generate_advanced_game_test_saves.ps1`。默认拒绝覆盖已存在输出目录。\n";
    return out.str();
}

void run(const Options& opts) {
    for(const auto* p:{&opts.source,&opts.game,&opts.workshop,&opts.local}) if(!std::filesystem::is_directory(*p)) fail("Required input directory missing: "+p->string());
    if(std::filesystem::exists(opts.output)) fail("Output exists; refusing overwrite: "+opts.output.string());
    const auto out_abs=absolute_path(opts.output);
    for(const auto* input:{&opts.source,&opts.game,&opts.workshop,&opts.local}) if(is_within(out_abs,*input)||is_within(*input,out_abs)) fail("Output must be disjoint from read-only input: "+out_abs.string());

    infrastructure::NativeFileSystem fs; application::SaveProfileDiscovery discovery{fs};
    auto profile=must(discovery.load(opts.source),"Load source profile");
    if(profile.status!=application::ProfileReadStatus::Complete) fail("Source profile is incomplete");
    const auto start_fingerprint=must(application::SaveProfileDiscovery::fingerprint_profile(fs,opts.source),"Fingerprint source");
    if(start_fingerprint!=profile.baseline_fingerprint) fail("Source fingerprint changed while loading");
    auto backup=opts.source.parent_path()/"backup"/opts.source.filename(); std::uint64_t backup_fp=0;
    if(std::filesystem::is_directory(backup)) {auto b=application::SaveProfileDiscovery::fingerprint_profile(fs,backup);if(b)backup_fp=b.value();}

    const auto temp=std::filesystem::temp_directory_path()/("ddse-stage11-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temp);
    application::BaseContentScanner base_scanner{fs}; auto base_scan=must(base_scanner.scan(application::BaseContentScanConfig::defaults(opts.game)),"Scan base and DLC");
    auto base_summary=must(infrastructure::BaseContentDatabaseBuilder{}.rebuild(temp/"base.db",base_scan),"Build temporary base DB");
    application::ModEnvironmentScanConfig mc; mc.workshop_root=opts.workshop; mc.local_mod_roots={opts.local}; mc.save_profile_root=opts.source; mc.prefer_manager_export=false; mc.base_content_database=temp/"base.db";
    application::ModEnvironmentScanner ms{fs}; auto mod_scan=must(ms.scan(mc),"Scan save mods");
    if(mod_scan.effective_order_source!="save_profile"||mod_scan.effective_order.size()!=mod_scan.save_order.size()) fail("Cannot resolve every enabled mod in save order");
    auto mod_summary=must(infrastructure::ModEnvironmentDatabaseBuilder{}.rebuild(temp/"mods.db",temp/"base.db",mod_scan),"Build temporary mod DB");
    application::ContentEnvironmentSelection selection; selection.language="english"; selection.fallback_language="english";
    infrastructure::SqliteContentEnvironment env({temp/"base.db",temp/"mods.db",selection});
    auto model=application::CampaignModelBuilder{}.build(profile,env); if(model.heroes.empty()||model.state==domain::ModelState::Invalid) fail("Source campaign model is not usable");
    const auto skills=list(env,"skill"),quirks=list(env,"quirk"),trinkets=list(env,"trinket");
    std::cout<<"Source roster="<<model.heroes.size()<<", enabled mods="<<mod_scan.effective_order.size()<<", definitions="<<mod_summary.definitions<<", diagnostics="<<mod_scan.diagnostics.size()<<"\n";

    auto upgrades_source=doc(profile,"persist.upgrades.json").decoded.value(); auto rows=purchase_rows(upgrades_source);
    std::vector<Scenario> scenarios;
    std::map<std::string,std::optional<std::int32_t>,std::less<>> skill_level_cache;
    const auto skill_maximum = [&](const domain::Hero& hero,const application::ContentDefinition& skill) -> std::optional<std::int32_t> {
        const auto key=*hero.class_id.value+":"+skill.id;
        auto it=skill_level_cache.find(key);
        if(it==skill_level_cache.end()) it=skill_level_cache.emplace(key,effective_combat_skill_level_count(env,fs,hero,skill)).first;
        return it->second;
    };
    const domain::Hero* beginner=nullptr; const application::ContentDefinition* upgrade_skill=nullptr; std::int32_t beginner_instance{};
    std::int32_t upgrade_max_levels{};
    const application::ContentDefinition* lowest_partial_skill=nullptr; const domain::Hero* lowest_partial_hero=nullptr;
    std::int32_t lowest_partial_instance{}; std::int32_t lowest_partial_max_levels{}; std::int32_t lowest_purchase_rank=std::numeric_limits<std::int32_t>::max();
    for(const auto& h:model.heroes) if(h.resolve_xp.value&&*h.resolve_xp.value<=8&&h.class_id.value) {
        std::int32_t inst{}; try{inst=instance_for(upgrades_source,h);}catch(...){continue;}
        for(const auto& s:skills) if(s.id.starts_with(*h.class_id.value+":")) {
            const auto maximum=skill_maximum(h,s); if(!maximum||*maximum<2) continue;
            const auto suffix=s.id.substr(s.id.find(':')+1); const auto rank=current_rank(rows,inst,*h.class_id.value+"."+suffix);
            if(rank==0) {beginner=&h;upgrade_skill=&s;beginner_instance=inst;upgrade_max_levels=*maximum;break;}
            if(rank<*maximum&&rank<lowest_purchase_rank) {
                lowest_partial_hero=&h;lowest_partial_skill=&s;lowest_partial_instance=inst;lowest_partial_max_levels=*maximum;lowest_purchase_rank=rank;
            }
        }
        if(upgrade_skill) break;
    }
    if(!upgrade_skill) {
        if(lowest_partial_skill) {
            std::cerr<<"No completely unupgraded combat skill exists among heroes with XP<=8; lowest partially upgraded candidate is "
                     <<hero_label(*lowest_partial_hero)<<" / "<<lowest_partial_skill->id<<" ("<<lowest_purchase_rank<<" purchased nodes).\n";
            beginner=lowest_partial_hero;upgrade_skill=lowest_partial_skill;beginner_instance=lowest_partial_instance;upgrade_max_levels=lowest_partial_max_levels;
        } else fail("No low-level hero has a combat skill below its effective maximum");
    }
    const auto& high=hero_at(model,0); const auto& low=hero_at(model,1); const auto high_instance=instance_for(upgrades_source,high); const auto low_instance=instance_for(upgrades_source,low);
    const domain::Hero* downgrade_hero=nullptr; const application::ContentDefinition* downgrade_skill=nullptr; std::int32_t downgrade_instance{};
    const domain::Hero* prerequisite_hero=nullptr; const application::ContentDefinition* prerequisite_skill=nullptr; std::int32_t prerequisite_instance{};
    std::int32_t downgrade_max_levels{},prerequisite_max_levels{};
    for(const auto& h:model.heroes) if(h.resolve_xp.value&&*h.resolve_xp.value>=36&&h.class_id.value) {
        std::int32_t inst{};try{inst=instance_for(upgrades_source,h);}catch(...){continue;}
        for(const auto& s:skills) if(s.id.starts_with(*h.class_id.value+":")) {
            const auto maximum=skill_maximum(h,s); if(!maximum||*maximum<3) continue;
            const auto suffix=s.id.substr(s.id.find(':')+1);
            const auto rank=current_rank(rows,inst,*h.class_id.value+"."+suffix);
            if(rank==*maximum) {downgrade_hero=&h;downgrade_skill=&s;downgrade_instance=inst;downgrade_max_levels=*maximum;break;}
            if(rank>=2&&!prerequisite_skill) {prerequisite_hero=&h;prerequisite_skill=&s;prerequisite_instance=inst;prerequisite_max_levels=*maximum;}
        }
        if(downgrade_skill) break;
    }
    if(!downgrade_skill&&prerequisite_skill) {
        downgrade_hero=prerequisite_hero;downgrade_skill=prerequisite_skill;downgrade_instance=prerequisite_instance;downgrade_max_levels=prerequisite_max_levels;
        scenarios.push_back(make_combat_max_prerequisite(profile,*downgrade_hero,*downgrade_skill,downgrade_instance,downgrade_max_levels));
    }
    if(!downgrade_skill) fail("No suitable maxed or downgradeable combat skill was found among high-level heroes");

    scenarios.push_back(make_combat_upgrade(profile,env,*beginner,*upgrade_skill,rows,beginner_instance,upgrade_max_levels));
    scenarios.push_back(make_combat_downgrade(profile,*downgrade_hero,*downgrade_skill,rows,downgrade_instance,downgrade_max_levels));

    const domain::Hero* camp_unlock_hero=nullptr; std::string camp_unlock_skill;
    const domain::Hero* camp_lock_hero=nullptr; std::string camp_lock_skill;
    for(const auto& h:model.heroes) if(h.class_id.value&&h.resolve_xp.value&&*h.resolve_xp.value<=8) {
        std::set<std::string,std::less<>> unlocked; for(const auto& sk:h.camping_skills) unlocked.insert(sk.id);
        for(const auto& s:skills) if(s.id.starts_with(*h.class_id.value+":")&&s.localization_key.starts_with("camping_skill_name_")) {
            const auto suffix=s.id.substr(s.id.find(':')+1);
            if(!unlocked.contains(suffix)){camp_unlock_hero=&h;camp_unlock_skill=suffix;break;}
        }
        if(!camp_unlock_hero) for(const auto& s:skills) if(s.localization_key.starts_with("camping_skill_name_")) {
            const auto payload=Json::parse(s.payload_json,nullptr,false);
            if(payload.is_discarded()||!payload.is_object()||!payload.contains("hero_classes")||!payload["hero_classes"].is_array()) continue;
            const bool class_match=std::any_of(payload["hero_classes"].begin(),payload["hero_classes"].end(),[&](const Json& v){return v.is_string()&&v.get<std::string>()==*h.class_id.value;});
            const auto suffix=s.id.substr(s.id.find(':')+1);
            if(class_match&&!unlocked.contains(suffix)){camp_unlock_hero=&h;camp_unlock_skill=suffix;break;}
        }
        if(camp_unlock_hero) break;
    }
    for(const auto& h:model.heroes) if(h.resolve_xp.value&&*h.resolve_xp.value>=36&&!h.camping_skills.empty()) {
        camp_lock_hero=&h;camp_lock_skill=h.camping_skills.front().id;break;
    }
    if(!camp_unlock_hero||!camp_lock_hero) fail("Could not find eligible low-level locked and high-level unlocked camping skills in effective content");
    scenarios.push_back(make_camping(profile,*camp_unlock_hero,camp_unlock_skill,true));
    scenarios.push_back(make_camping(profile,*camp_lock_hero,camp_lock_skill,false));

    const domain::Hero* lock_hero=nullptr; const domain::HeroQuirk* lock_quirk=nullptr;
    for(const auto& h:model.heroes) {
        for(const auto& q:h.quirks) {
            auto j=Json::parse(def_payload(q),nullptr,false);
            if(!q.is_disease&&q.polarity==domain::QuirkPolarity::Positive&&!q.is_locked.value.value_or(false)&&!j.is_discarded()&&j.value("can_modify_in_activity",false)) {lock_hero=&h;lock_quirk=&q;break;}
        }
        if(lock_hero) break;
    }
    if(!lock_hero||!lock_quirk) fail("No positive quirk with can_modify_in_activity=true is available to lock");
    scenarios.push_back(make_lock_quirk(profile,*lock_hero,*lock_quirk));
    scenarios.push_back(make_remove_quirks(profile,high));

    scenarios.push_back(make_gear(profile,low,3,4,low_instance,true));
    scenarios.push_back(make_gear(profile,high,1,0,high_instance,false));

    std::vector<application::ContentDefinition> vanilla_diseases;
    for(const auto& d:base_scan.definitions) if(d.kind=="disease") {
        auto j=Json::parse(d.payload_json,nullptr,false);
        if(j.is_object()&&!j.value("is_disease",false)) continue;
        application::ContentDefinition value; value.type=d.kind;value.id=d.id;value.display_name=d.display_name;
        value.localization_key=d.localization_key;value.payload_json=d.payload_json;
        value.provenance={d.source_id,"base",d.virtual_path,true};vanilla_diseases.push_back(std::move(value));
    }
    if(vanilla_diseases.empty()) fail("No vanilla disease definition is available in the read-only base/DLC scan");
    const domain::Hero* disease_hero=nullptr;
    for(const auto& h:model.heroes) if(h.roster_position<5&&std::none_of(h.quirks.begin(),h.quirks.end(),[](const auto&q){return q.is_disease;})){disease_hero=&h;break;}
    if(!disease_hero) fail("No early hero without a disease is available");
    const application::ContentDefinition* vanilla_disease=nullptr;
    for(const auto& disease:vanilla_diseases) if(std::none_of(disease_hero->quirks.begin(),disease_hero->quirks.end(),[&](const auto&q){return q.id==disease.id;})){vanilla_disease=&disease;break;}
    if(!vanilla_disease) fail("All scanned vanilla disease IDs already occur on the selected hero");
    scenarios.push_back(make_add_disease(profile,env,*disease_hero,*vanilla_disease));

    scenarios.push_back(make_level_swap(profile,high,low)); scenarios.push_back(make_building_downgrade(profile));

    std::optional<application::ContentDefinition> generic_value;
    for(const auto& t:base_scan.definitions) if(t.kind=="trinket") {
        const auto payload=Json::parse(t.payload_json,nullptr,false);
        if(payload.is_discarded()||!payload.is_object()||!payload.contains("hero_class_requirements")||
           !payload["hero_class_requirements"].is_array()||!payload["hero_class_requirements"].empty()) continue;
        application::ContentDefinition d;d.type=t.kind;d.id=t.id;d.display_name=t.display_name;
        d.localization_key=t.localization_key;d.payload_json=t.payload_json;d.provenance={t.source_id,"base",t.virtual_path,true};
        generic_value=std::move(d);break;
    }
    if(!generic_value) fail("No generic vanilla trinket found in read-only base/DLC data");
    const auto* generic=&*generic_value;
    const domain::Hero* equip_hero=nullptr; const application::ContentDefinition* specific=nullptr;
    for(const auto& h:model.heroes) if(h.trinkets.empty()&&h.class_id.value) {
        auto it=std::find_if(trinkets.begin(),trinkets.end(),[&](const auto&t){
            if(t.provenance.layer_type!="mod"||!t.provenance.selected||t.id==generic->id)return false;
            return std::any_of(t.relationships.begin(),t.relationships.end(),[&](const auto&r){return r.relationship_type=="restricted_to"&&r.id==*h.class_id.value;});
        });
        if(it!=trinkets.end()){equip_hero=&h;specific=&*it;break;}
    }
    if(!equip_hero||!specific) fail("No empty hero has an enabled mod class-specific trinket");
    // Resolve both equipment definitions before changing the save. The actual raw object template comes from an existing hero.
    auto roster_for_equip=clone_doc(*doc(profile,"persist.roster.json").decoded); auto& target_inner=hero_data(roster_for_equip,equip_hero->persistent_id);
    const std::string target_items="base_root/trinkets/items";
    const auto& source_roster=*doc(profile,"persist.roster.json").decoded;
    std::string template_path; std::string template_hero_id;
    for(const auto& h:model.heroes) if(!h.trinkets.empty()) {
        const auto& src_inner=*field(source_roster,hero_base(h.persistent_id)).embedded_document;
        const auto& source_items=field(src_inner,"base_root/trinkets/items");
        if(source_items.children.empty()) continue;
        template_path=src_inner.fields.at(source_items.children.front()).path; template_hero_id=h.persistent_id; break;
    }
    if(template_path.empty()) fail("No existing equipped trinket record is available as an encoding template");
    auto& source_inner=*field(roster_for_equip,hero_base(template_hero_id)).embedded_document;
    for(const auto* item:{generic,specific}) {
        std::size_t index=0; for(auto child:field(target_inner,target_items).children)
            index=std::max(index,numeric_key(target_inner.fields.at(child).name)+1);
        std::string new_key=std::to_string(index);
        auto a=core::dson::DsonDocumentEditor::append_clone(target_inner,target_items,source_inner,template_path,new_key);
        if(!a) fail("Cannot append equipped trinket object: "+a.error().message);
        replace_item_id(target_inner,target_items+"/"+new_key,item->id);
    }
    auto equip_detail="- 英雄：**"+hero_label(*equip_hero)+"**，饰品栏原为空。\n- 原版通用饰品：`"+generic->id+"`（"+pretty(env,*generic)+"）。\n- Mod 职业专属饰品：`"+specific->id+"`（"+pretty(env,*specific)+"），来源 **"+mod_name(mod_scan,specific->provenance.source_id)+"**（`"+specific->provenance.source_id+"`），限制职业 `"+*equip_hero->class_id.value+"`。\n";
    scenarios.push_back(scenario("16_equip_trinkets","装备一个原版通用和一个职业限定 mod 饰品",equip_detail,{{"persist.roster.json",encoded_doc(roster_for_equip,"persist.roster.json")}}));
    const auto& destroy_hero=high; scenarios.push_back(make_destroy_hero_trinket(profile,destroy_hero));

    // The source profile has no districts subtree. Materialize the same object hierarchy read_town consumes,
    // with every installed effective district represented as not built.
    auto town=clone_doc(*doc(profile,"persist.town.json").decoded);
    const auto& town_buildings=field(town,"base_root/buildings"); if(town_buildings.children.empty()) fail("Town has no object template for district save state");
    const auto template_building_path=town.fields.at(town_buildings.children.front()).path;
    auto bool_template=std::find_if(town.fields.begin(),town.fields.end(),[](const auto& f){return f.kind==core::dson::ValueKind::Boolean;});
    if(bool_template==town.fields.end()) fail("Town document has no boolean field usable for district state");
    const auto bool_template_path=bool_template->path;
    auto root_add=core::dson::DsonDocumentEditor::append_clone(town,"base_root",town,template_building_path,"districts");
    if(!root_add) fail("Cannot add town districts state: "+root_add.error().message);
    clear_object_children(town,"base_root/districts");
    auto buildings_add=core::dson::DsonDocumentEditor::append_clone(town,"base_root/districts",town,"base_root/districts","buildings");
    if(!buildings_add) fail("Cannot add district building map: "+buildings_add.error().message);
    std::set<std::string,std::less<>> district_ids;
    for(const auto& entry:std::filesystem::recursive_directory_iterator(opts.game/"dlc")) {
        if(!entry.is_regular_file()||!entry.path().filename().string().ends_with(".districts.json")) continue;
        std::ifstream input(entry.path(),std::ios::binary); if(!input) fail("Cannot read district definition: "+entry.path().string());
        Json root; try{input>>root;}catch(const std::exception&){continue;}
        if(!root.is_object()||!root.contains("buildings")||!root["buildings"].is_array()) continue;
        for(const auto& district:root["buildings"]) if(district.is_object()&&district.contains("name")&&district["name"].is_string())
            district_ids.insert(district["name"].get<std::string>());
    }
    if(district_ids.empty()) fail("No district definitions found in the effective content environment");
    for(const auto& id:district_ids) {
        auto add=core::dson::DsonDocumentEditor::append_clone(town,"base_root/districts/buildings",town,template_building_path,id);
        if(!add) continue; // A duplicate ID can arise from multiple DLC district sources.
        const auto district_path="base_root/districts/buildings/"+id; clear_object_children(town,district_path);
        auto built=core::dson::DsonDocumentEditor::append_clone(town,district_path,town,bool_template_path,"built");
        if(!built) fail("Cannot add built=false state for district "+id+": "+built.error().message);
        set_bool(town,district_path+"/built",false);
    }
    if(field(town,"base_root/districts/buildings").children.empty()) fail("District map did not contain any structures");
    scenarios.push_back(scenario("18_unlock_district_system","解锁小镇建筑（区域建筑）功能", "- 在 `persist.town.json` 建立 `districts/buildings/<districtId>/built` 状态，并将各区域建筑的 `built` 设为 false。\n- 这只开放系统状态，不会建造任何区域建筑。请在游戏中确认区域建筑入口与列表可用。\n",{{"persist.town.json",encoded_doc(town,"persist.town.json")}}));
    scenarios.push_back(make_destroy_inventory_trinket(profile,model));

    std::filesystem::create_directories(out_abs.parent_path()); auto staging=out_abs; staging += ".building-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(staging); copy_tree(opts.source,staging/"00_control"/profile.descriptor.id);
    if(must(application::SaveProfileDiscovery::fingerprint_profile(fs,staging/"00_control"/profile.descriptor.id),"Check control copy")!=profile.baseline_fingerprint) fail("Control copy fingerprint mismatch");
    for(const auto& s:scenarios) {
        std::cout<<"Generating "<<s.directory<<"...\n"<<std::flush;
        auto dest=staging/s.directory/profile.descriptor.id; copy_tree(opts.source,dest); std::set<std::string,std::less<>> changed;
        for(const auto&[id,bytes]:s.documents) {
            const auto filename=doc(profile,id).path.filename(); changed.insert(filename.generic_string());
            must(fs.write_file_atomic(dest/filename,bytes_string(bytes)),"Write "+s.directory+"/"+id);
        }
        validate_unchanged_files(opts.source,dest,changed);
        auto reread=must(discovery.load(dest),"Reload "+s.directory); if(reread.status!=application::ProfileReadStatus::Complete) fail("Generated profile is not complete: "+s.directory);
        if(doc(reread,"persist.game.json").bytes!=doc(profile,"persist.game.json").bytes) fail("Mod order bytes changed in "+s.directory);
        if(s.directory=="01_combat_skill_upgrade"||s.directory=="02_combat_skill_downgrade"||s.directory=="02a_combat_skill_max_prerequisite") {
            const auto actual_rows=purchase_rows(*doc(reread,"persist.upgrades.json").decoded);
            const auto* test_hero=s.directory=="01_combat_skill_upgrade"?beginner:downgrade_hero;
            const auto* test_skill=s.directory=="01_combat_skill_upgrade"?upgrade_skill:downgrade_skill;
            const auto test_instance=s.directory=="01_combat_skill_upgrade"?beginner_instance:downgrade_instance;
            const auto expected_rank=s.directory=="01_combat_skill_upgrade"?upgrade_max_levels:
                (s.directory=="02_combat_skill_downgrade"?2:downgrade_max_levels);
            const auto tree=*test_hero->class_id.value+"."+test_skill->id.substr(test_skill->id.find(':')+1);
            if(current_rank(actual_rows,test_instance,tree)!=expected_rank)
                fail("Combat skill purchase nodes do not match requested level in "+s.directory);
        }
        const auto projected=application::CampaignModelBuilder{}.build(reread,env);
        if(projected.state==domain::ModelState::Invalid||projected.heroes.size()!=model.heroes.size()) fail("Generated profile did not rebuild a valid roster model: "+s.directory);
        for(std::size_t hero_index=0;hero_index<model.heroes.size();++hero_index) {
            if(projected.heroes[hero_index].persistent_id!=model.heroes[hero_index].persistent_id||
               projected.heroes[hero_index].class_id.value!=model.heroes[hero_index].class_id.value)
                fail("Generated profile changed roster identity/order unexpectedly: "+s.directory);
        }
        const auto changedfp=must(application::SaveProfileDiscovery::fingerprint_profile(fs,dest),"Fingerprint "+s.directory);
        if(changedfp==profile.baseline_fingerprint) fail("Scenario changed no bytes: "+s.directory);
        if(s.directory=="18_unlock_district_system") {
            if(projected.districts.size()!=district_ids.size()) fail("District structure did not project all expected district states");
            if(std::any_of(projected.districts.begin(),projected.districts.end(),[](const auto& d){return !d.built.value||*d.built.value;})) fail("District unlock profile should contain only built=false states");
        }
    }
    auto source_match=must(profile.matches_disk_baseline(fs),"Verify source unchanged"); if(!source_match) fail("Source profile changed during generation");
    must(fs.write_file_atomic(staging/"README.md",readme(profile,mod_scan,scenarios,backup_fp,opts)),"Write checklist");
    std::filesystem::rename(staging,out_abs);
    const auto base_defs=base_summary.hero_classes+base_summary.skills+base_summary.trinkets+base_summary.quirks+base_summary.diseases+base_summary.resources+base_summary.buildings;
    std::cout<<"Generated "<<scenarios.size()<<" independent scenarios plus control at "<<out_abs.string()<<"\n"
             <<"Base definitions="<<base_defs<<", mod definitions="<<mod_summary.definitions<<", enabled mods="<<mod_scan.effective_order.size()<<", diagnostics="<<mod_scan.diagnostics.size()<<"\n";
    std::error_code ignored; std::filesystem::remove_all(temp,ignored);
}

int run(const std::vector<std::string>& args) {
    try {
        run(options_from(args)); return 0;
    }
    catch(const std::exception& e) { std::cerr<<"Advanced test save generation failed: "<<e.what()<<'\n'; return 1; }
}
}

#ifdef _WIN32
int main() {
    int argc=0; auto argv=CommandLineToArgvW(GetCommandLineW(),&argc); if(!argv) return 2;
    std::vector<std::string> args;
    for(int i=1;i<argc;++i) {int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,nullptr,0,nullptr,nullptr); if(size<=0){LocalFree(argv);return 2;} std::string s(static_cast<std::size_t>(size),'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,s.data(),size,nullptr,nullptr);s.resize(static_cast<std::size_t>(size-1));args.push_back(std::move(s));}
    LocalFree(argv); return run(args);
}
#else
int main(int argc,char**argv){std::vector<std::string> args;for(int i=1;i<argc;++i)args.emplace_back(argv[i]);return run(args);}
#endif
