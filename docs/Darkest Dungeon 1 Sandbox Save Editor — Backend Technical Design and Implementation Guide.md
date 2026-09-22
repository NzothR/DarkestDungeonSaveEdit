# Darkest Dungeon 1 Sandbox Save Editor
## Backend Technical Design and Implementation Guide / 后端技术设计与实施指南

**文档版本：** v1.0 Draft  
**状态：** Backend Architecture Baseline  
**目标读者：** 项目开发者、后续代码审查者  
**目标平台：** Windows / Linux 桌面环境  
**核心语言：** C++20  
**构建系统：** CMake  
**持久化索引：** SQLite  
**HTTP 适配层：** Drogon（最后阶段接入）  
**设计目标：** Mod-aware、Semantic、Non-destructive、Testable、Locally Hosted

---

# 1. 文档定位

本文档同时承担两种职责：

1. **Backend Technical Design**：规定系统边界、依赖方向、数据模型、持久化模型、错误模型与安全写回原则。
2. **Implementation Guide**：给出适合单人开发的落地顺序，并为每个阶段说明为什么现在做、如何测试、预期得到什么、失败时先检查哪里、何时可以进入下一阶段。

本文档不承担以下职责：

- 不规定最终网页视觉与交互细节；
- 不把未经验证的 DSON 字段路径写成正式 Schema；
- 不把 Drogon Controller 当作业务实现位置；
- 不把编辑器扩展成 Mod Manager；
- 不解释 C++、SQL、HTTP 的基础语法。

后端的最终职责是：

> 识别当前 Darkest Dungeon 1 游戏环境，建立 Vanilla、DLC 与 Mod 的有效内容视图，安全读取并语义化存档，通过明确操作修改工作副本，验证后备份并写回，同时向未来 UI 暴露稳定的应用服务。

---

# 2. 已确认事实、证据等级与未知项

## 2.1 当前 Campaign 样本

`profile_0(1).zip` 中实际包含：

```text
persist.curio_tracker.json
persist.estate.json
persist.game.json
persist.game_knowledge.json
persist.journal.json
persist.narration.json
persist.progression.json
persist.quest.json
persist.roster.json
persist.town.json
persist.town_event.json
persist.tutorial.json
persist.upgrades.json
novelty_tracker.json
persist.campaign_log.json
persist.campaign_mash.json
```

该样本可确认：

- 项目必须支持完整 Campaign profile 的多文档装载；
- `roster / estate / game / town / upgrades / progression / quest` 等核心文件真实存在；
- 单个 profile 同时包含核心业务文档、追踪文档、叙事文档和日志文档；
- 即使第一版不编辑某些文档，Raw Save 层也必须发现、读取、保留并纳入备份。

静态样本不能单独确认：

- 某个语义属性由哪些字段共同决定；
- 字段是否跨多个 persist 文件联动；
- 游戏写入时是否同步更新历史、索引或缓存字段；
- Mod 环境中相同字段是否保持同一结构；
- 修改后游戏是否接受该组合状态。

## 2.2 证据等级

所有 Mapping、Parser 特例和业务规则必须标注证据等级：

| 等级 | 含义 | 是否允许写入 |
| --- | --- | --- |
| `VERIFIED_FORMAT` | DSON 格式结构已由实现和 round-trip 验证 | 仅允许格式层操作 |
| `VERIFIED_SAMPLE` | 在真实样本中观察到字段或结构 | 默认只读 |
| `VERIFIED_MUTATION` | 通过游戏内受控 A/B 变更确认字段联动 | 可进入写入 Mapping |
| `VERIFIED_GAME` | 编辑器写回后由游戏成功加载并验证 | 可标记稳定 |
| `INFERRED` | 根据命名、邻近结构或外部资料推断 | 不允许自动写入 |
| `UNKNOWN` | 尚不理解 | Raw Preserve |

原则：

> “能够解析”不等于“能够安全编辑”；“在样本中见过”也不等于“已确认写入规则”。

## 2.3 当前非阻塞未知项

以下问题需要在实现中逐步验证，但不阻塞总体架构：

- 不同内容类型的重复 ID 在游戏中的精确覆盖细节；
- 部分 Mod 自定义文件的容错语法；
- Hero 等级、装备、技能与升级历史的跨文件联动；
- 新建 Hero 所需的最小完整合法模板；
- 某些运行时状态是否必须在编辑前清除；
- Mod load order 的所有来源与优先级。

这些未知项必须被收敛到 Parser、Mapping、Fixture 和 Diagnostics 中，不能散落为临时代码。

---

# 3. 产品边界与非目标

## 3.1 后端必须完成

- 定位游戏、DLC、Workshop Mod、本地 Mod 与存档目录；
- 建立稳定的 Base Catalog 和可更新的 Mod Environment；
- 解析内容定义、本地化与资产引用；
- 发现 Save Profile 与 Save Domain；
- 无损 Decode / Encode DSON；
- 构造 Raw Save 与 Semantic Save Model；
- 提供 Hero、饰品、资源、建筑等语义操作；
- 支持 Undo / Redo / Discard / Change Summary；
- 对修改执行结构、语义、引用和提交验证；
- 完整备份 profile 后安全写回；
- 通过 Application Service 暴露稳定用例；
- 最后通过 Drogon 向 UI 提供本地 HTTP API。

## 3.2 明确非目标

- 不替用户安装、删除、排序或修复 Mod；
- 不尝试自动修复所有损坏存档；
- 不因缺失 Mod 删除未知引用；
- 不把游戏进度限制当作绝对结构限制；
- 不在数据库中保存玩家存档作为事实来源；
- 不让 HTTP、SQLite 或 GUI 类型进入 Domain Model；
- 不在第一版实现远程服务、多用户或云同步。

---

# 4. 核心设计原则

## 4.1 Semantic Editor

用户操作的是：

```text
Hero Level
Weapon Stage
Combat Skill
Quirk
Disease
Trinket
Resource
Building Upgrade
```

不是：

```text
persist.roster.json / field[12] / child[3]
```

## 4.2 Preserve Unknown Data

编辑器只修改具有可写 Mapping 的已知字段。未知字段、重复字段、Mod 私有字段、未知类型和未识别文档均保持原始内容。

写回策略必须是：

```text
Original Raw AST
    ↓ clone
Patch Known Fields Only
    ↓ encode
Candidate Bytes
```

禁止根据 Semantic Model 重新生成整份存档文档。

## 4.3 Structural Safety 优先

约束分为三类：

| 类型 | 示例 | 默认行为 |
| --- | --- | --- |
| Structural | 类型、对象层级、唯一 ID、数组结构 | 必须满足 |
| Environment | 当前 Mod 下的容量、存在性、最大等级 | 警告或允许显式覆盖 |
| Gameplay | 周数、资源消耗、正常升级门槛 | Sandbox 模式不强制 |

## 4.4 Edit in Memory

单次点击只修改 Session 工作模型，不立即写盘。只有显式 Commit 才进入保存事务。

## 4.5 Explicit Operations

所有业务修改必须表示为 Operation。GUI、CLI、HTTP 均不得绕过 Application Service 直接修改 AST 或数据库。

## 4.6 Fail Closed on Write

读取时尽量容错并隔离错误；写入时必须保守。任何无法确认的候选文件、验证失败、备份失败或回读失败都不得报告成功。

---

# 5. 总体架构

系统采用 Ports and Adapters 风格，但不追求形式主义。核心目标是让依赖方向稳定、业务可单测、外部技术可替换。

```text
UI / CLI / Tests
        │
        ▼
Transport Adapters
  Drogon HTTP（最后接入）
        │
        ▼
Application Services
  OpenSave / Edit / Commit / RefreshEnvironment
        │
        ▼
Domain + Core Mechanisms
  Semantic Model / Operations / DSON / Overlay / Mapping
        │
        ▼
Ports
  Repositories / File System / Clock / Backup / Hash
        ▲
        │ implements
Infrastructure
  SQLite / Native FS / Logging / Drogon / OS Integration
```

## 5.1 四类核心数据

| 数据 | 回答的问题 | 事实来源 |
| --- | --- | --- |
| Raw DSON | 游戏实际保存了什么 | Save files |
| Content Catalog | 当前环境中什么内容存在 | Game / DLC / Mod files |
| Semantic Model | 玩家当前状态意味着什么 | Raw DSON + Effective Environment |
| Editor Session | 用户准备改变什么 | Operations in memory |

这四类数据不得合并成一个通用 JSON 对象。

## 5.2 依赖规则

允许：

```text
HTTP → Application
Application → Domain Ports
Save Adapter → Domain + DSON
Content Parser → Content Models
SQLite Repository → Repository Interfaces
```

禁止：

```text
Domain → Drogon
Domain → SQLite
DSON → Hero
Content Catalog → Editor Session
Controller → Raw DSON mutation
```

## 5.3 Composition Root

所有具体实现只在进程入口装配。概念结构：

```cpp
int main(int argc, char** argv) {
    AppConfiguration config = loadConfiguration(argc, argv);
    Logging logging(config.logging);

    NativeFileSystem fileSystem;
    SqliteDatabase baseDb(config.baseDatabasePath);
    SqliteDatabase environmentDb(config.environmentDatabasePath);

    BaseContentRepository baseRepo(baseDb);
    ModContentRepository modRepo(environmentDb);
    EffectiveContentEnvironment content(baseRepo, modRepo);

    DsonCodec dsonCodec;
    SaveProfileRepository saves(fileSystem, dsonCodec);
    BackupService backups(fileSystem, config.backupRoot);
    ValidationService validation(content);

    EditorApplication application(
        content, saves, backups, validation);

    DrogonHttpAdapter http(application, config.http);
    return http.run();
}
```

以上代码只表达装配关系，不是最终接口承诺。关键规则是：模块不知道谁创建了自己，也不知道自己是否被 HTTP、CLI 还是测试调用。

---

# 6. 进程、并发与生命周期

## 6.1 单机单用户模型

第一版采用单进程、本机回环地址、单用户模型：

- 服务只监听 `127.0.0.1`；
- 默认随机或配置端口；
- 同一时刻可打开一个主要 Editor Session；
- 内容扫描可异步执行，但 Commit 对同一 profile 必须串行；
- 不设计分布式锁、用户账户与远程鉴权。

## 6.2 长任务

以下任务可能较慢：

- 初次构建 Base Catalog；
- 更新大型 Mod 环境；
- 资产索引；
- 完整 profile 备份与写后验证。

它们由 Application 层表示为 Job：

```text
Job
├── id
├── type
├── status
├── progress
├── startedAt
├── finishedAt
├── diagnostics[]
└── resultSummary
```

第一版 HTTP 可使用轮询查询 Job 状态；只有出现明确需求时再增加 SSE。不要一开始引入 WebSocket。

## 6.3 Session 并发保护

打开存档时记录：

```text
file path
size
modified time
content hash
```

Commit 前重新检查。若外部程序或游戏修改了任一目标文件，则拒绝覆盖并要求重新打开或显式处理冲突。

---

# 7. 公共工程基础设施

## 7.1 统一标识与路径

业务层使用强类型标识，避免字符串混用：

```text
HeroId
ContentId
SourceId
SaveProfileId
DocumentId
OperationId
JobId
VirtualPath
```

`VirtualPath` 表示相对于某个 Source Root 的规范化路径。业务层不拼接 Windows 分隔符，也不保存临时绝对路径作为内容身份。

## 7.2 Result 与错误模型

预期失败使用结构化结果，不把所有错误压成 `std::runtime_error`。

```cpp
enum class ErrorCode {
    InvalidConfiguration,
    FileNotFound,
    PermissionDenied,
    DsonMalformed,
    UnsupportedDsonType,
    DatabaseLocked,
    MigrationFailed,
    ContentParseFailed,
    MappingNotWritable,
    ConcurrentSaveChanged,
    ValidationFailed,
    BackupFailed,
    CommitFailed
};

struct Error {
    ErrorCode code;
    std::string message;
    std::string module;
    std::map<std::string, std::string> context;
    std::shared_ptr<Error> cause;
};
```

异常保留给编程错误、不可恢复的不变量破坏或第三方库边界；跨模块的正常失败返回 `Result<T, Error>` 或等价类型。

## 7.3 日志

从阶段 0 引入日志。至少支持：

```text
TRACE DEBUG INFO WARN ERROR
```

结构化字段至少包括：

```text
module
operation
path
source_id
content_id
profile_id
job_id
error_code
```

日志不得包含完整存档字节、用户个人路径之外不必要的隐私内容或无限量 AST dump。

## 7.4 配置

配置与游戏数据分离：

```text
AppConfiguration
├── gameRoot
├── workshopRoots[]
├── localModRoots[]
├── saveRoots[]
├── backupRoot
├── dataRoot
├── language
├── maxBackupCount
├── autoEditSaveEnabled
├── autoEditSaveIntervalSeconds
├── logging
└── http
```

`language` 是界面 locale，只允许 `en_us` 和 `zh_cn`，默认 `zh_cn`。游戏内容查询使用独立的内容语言配置，
不与 UI locale 混用。`gameRoot`、`backupRoot` 是必填目录；Profile、Workshop Mod 和额外本地 Mod 目录可选，
当前产品流程中各只有一个路径，HTTP 适配器将其传递为单元素列表给扫描器。

Windows 的 `/api/select-directory` 由后端调用原生目录选择器并返回绝对路径，前端不直接接触本机文件系统 API。
选择 `save` 时必须是单个 `profile_*` 目录，并在返回前完成 Profile 结构校验。

首次启动在 `dataRoot/config.json` 写入配置，并初始化 `backupRoot` 与
`backupRoot/AutoEditSave`。配置写入采用原子替换；备份目录不可创建或不可写时，启动阶段返回结构化诊断。
`maxBackupCount` 默认 20，`autoEditSaveEnabled` 默认开启，`autoEditSaveIntervalSeconds` 默认 30。
配置错误应在启动期一次性汇总，不能等到深层 Parser 才以“不存在文件”表现。

## 7.5 SQLite 封装

基础封装只提供：

```text
Database
Statement
Transaction
MigrationRunner
ConnectionFactory
```

Domain 不依赖这些类型。Repository 实现负责 SQL 与 Domain Record 之间的映射。

---

# 8. DSON Core

## 8.1 职责

DSON Core 只负责：

- 二进制格式读取与写入；
- Header、Meta 区和 Data 区校验；
- 有序字段、重复字段与显式类型；
- Hash reference；
- Embedded DSON；
- 未知值的原始保留；
- 结构比较与诊断输出。

它不知道 Hero、饰品、建筑或 Mod。

## 8.2 Raw AST

禁止使用 `std::map<string, Value>` 作为对象表示，因为它会丢失字段顺序和重复字段。

```text
DsonDocument
├── header
├── root
├── sourceMetadata
└── originalBytes?

DsonObject
├── fields[]
└── rawMetadata

DsonField
├── nameHash
├── resolvedName?
├── valueKind
├── value
├── rawValue
├── rawMetadata
├── sourceOffset
└── dirty
```

`DsonValue` 使用显式 variant，至少覆盖已知标量、数组、对象、向量、Pair、Embedded File 与 Unknown。

## 8.3 无损语义

“无损”分三级：

1. **Binary identical**：未修改时字节完全一致，优先目标。
2. **Structural identical**：重新编码后字段、顺序、类型、metadata、重复字段一致。
3. **Semantic identical**：已理解的值一致，但二进制或未知 metadata 变化。

核心 persist 文件的目标至少是 Structural identical。任何只能达到 Semantic identical 的原因必须记录为已知限制，且默认禁止写回生产存档。

## 8.4 Hash

Hash 解析返回候选集合：

```text
HashReference
├── rawHash
├── candidates[]
└── state: Resolved | Ambiguous | Unresolved
```

不能假设 `hash → 唯一字符串`。写回未修改字段时使用原始 hash；只有明确操作改变引用时才计算新 hash。

## 8.5 DSON 错误

典型错误必须携带 offset、section、document 和上下文：

- invalid magic / header；
- offset 越界；
- parent index 不一致；
- 数据长度溢出；
- 未知类型；
- Embedded DSON 截断；
- Hash 无法解析。

未知类型若能确定长度则 Raw Preserve；无法确定边界时整份文档只读，不能猜测跳过。

---

# 9. Save Raw Layer

## 9.1 Save Profile Discovery

`SaveProfileDiscovery` 扫描候选目录并产生：

```text
SaveProfileDescriptor
├── id
├── rootPath
├── documents[]
├── detectedDomains[]
├── modifiedAt
└── diagnostics[]
```

## 9.2 Domain Discovery

Domain 由文件组合和内部证据共同判断：

```text
Campaign
Circus
Raid
Shared
Unknown
```

一个 profile 可以同时拥有多个 Domain。未知文档仍注册到 `RawDocumentRegistry`。

## 9.3 Raw Document Registry

```text
RawSaveProfile
├── descriptor
├── documents: Map<DocumentId, DsonDocument>
├── baselineFingerprints
└── decodeDiagnostics
```

打开失败采用文档级隔离：非核心文档失败可使 profile 进入 Partial Read-only；核心文档失败则阻止相关 Domain 编辑，但仍允许诊断和备份。

## 9.4 Save 不是数据库

SQLite 只保存内容索引、扫描状态和环境诊断。玩家 Save 的事实来源始终是磁盘上的 persist 文件；不把 Save 导入 SQLite 后再以数据库副本为准。

---

# 10. Content Source 与统一 Parser

## 10.1 Content Source

```text
ContentSource
├── sourceId
├── type: Vanilla | DLC | WorkshopMod | LocalMod
├── rootPath
├── enabled
├── applyOrder
├── displayName
├── version?
└── metadata
```

Vanilla 是 `Base Source`；DLC 是 `Base Overlay Source`；Workshop 与本地 Mod 是 `Mod Source`。

## 10.2 Parser 统一

不能分别维护 `VanillaHeroParser / DlcHeroParser / ModHeroParser`。正确抽象是：

```text
HeroParser::parse(ContentSource)
TrinketParser::parse(ContentSource)
QuirkParser::parse(ContentSource)
SkillParser::parse(ContentSource)
LocalizationParser::parse(ContentSource)
AssetScanner::scan(ContentSource)
```

Source 类型影响上下文和输出目标，不复制 Parser。

## 10.3 Parser 输出

Parser 不直接写 SQL。它产生带 provenance 的中间记录：

```text
ParsedRecord<T>
├── logicalId
├── definition
├── provenance
├── warnings[]
└── completeness
```

Writer 在事务中批量写入目标数据库。这样 Parser 可用内存 Fixture 单测，也不会把解析失败与数据库事务耦合。

## 10.4 容错策略

- 未知可选属性：保留扩展信息并 Warning；
- 缺失必需属性：该记录 Invalid，不进入有效索引；
- 单文件失败：隔离该文件，继续扫描其他文件；
- 单 Mod 大量失败：环境仍可构建，但标记 Degraded；
- 重复 ID：保留 override chain，按已解析加载顺序选 winner；
- 加载顺序未知：不得按文件夹名猜测，环境标记 Unresolved。

---

# 11. 双 SQLite 数据库架构

物理上分为：

```text
data/
├── base_content.db
└── environment.db
```

## 11.1 为什么拆分

两类数据生命周期不同：

| 数据库 | 内容 | 更新频率 |
| --- | --- | --- |
| `base_content.db` | Vanilla + 所有官方 DLC 的稳定索引 | 首次构建或用户主动重建 |
| `environment.db` | 当前 Mod 环境、加载顺序、冲突、警告、启用 DLC 状态 | 用户主动“更新 Mod 环境” |

更新 Mod 环境不能重新扫描庞大的 Base 内容与资产。重建 Base 也不能破坏仍可用的旧库。

## 11.2 Base Catalog

Base Catalog 同时保存 Vanilla 与 DLC，但必须保留来源边界：

```text
content_source
source_file
hero_class
combat_skill
camping_skill
trinket
quirk
disease
resource
building
upgrade_node
localization
asset
string_hash
base_scan_metadata
schema_migration
```

关键字段原则：

```text
source_type
source_id
dlc_id nullable
logical_id
source_file_id
definition_fingerprint
parse_status
```

Base Catalog 包含所有官方 DLC；是否对当前编辑器可见由 Effective Environment 中的 enabled DLC 决定。

## 11.3 Mod Catalog 与环境状态

`environment.db` 建议包含：

```text
environment_metadata
enabled_dlc
mod_source
mod_load_order
mod_file
hero_class
combat_skill
camping_skill
trinket
quirk
disease
resource
building
upgrade_node
localization
asset
content_override
content_conflict
parse_warning
scan_job
schema_migration
```

只保存当前选定环境的有效 Mod 索引与必要 override chain，不保存玩家 Save。

## 11.4 Schema 原则

- 主键使用稳定内部键，业务查询使用 `(content_type, logical_id, source_id)` 唯一约束；
- 所有记录可追溯到 source file；
- 不把完整源文件无差别复制进数据库；
- 搜索需要的当前语言文本可建立 FTS 或规范化辅助表；
- Schema migration 只能向前执行，失败时回滚；
- `schema_version` 与 `parser_version` 分开记录；
- 时间戳只用于诊断，不作为唯一缓存有效性依据。

## 11.5 原子重建

Base 重建：

```text
scan → base_content.db.new → integrity check → smoke query
     → atomic replace base_content.db
```

Mod 环境更新：

```text
scan → environment.db.new → resolve → validate → smoke query
     → atomic replace environment.db
```

失败时保留旧库并报告诊断。禁止在用户当前可用数据库上边扫描边清空。

## 11.6 Fingerprint

```text
BaseFingerprint
├── gameBuild
├── executableVersion
├── sourceFileCount
├── totalSize
├── stableManifestHash
├── parserVersion
└── schemaVersion
```

环境 Fingerprint 额外包含：

```text
enabledDlc
enabledMods
modLoadOrder
relevantFileMetadata
```

第一版不需要后台监听；用户主动触发更新，系统只提示可能过期。

---

# 12. Overlay 与 Effective Content Environment

## 12.1 三个概念

```text
Base Catalog       = Vanilla + DLC definitions
Mod Catalog        = current resolved Mod definitions
Effective Environment = enabled Base + Mod overrides
```

Effective Environment 是运行时视图，不是第三套完整数据库。

## 12.2 文件层优先

先按真实加载顺序建立 Effective VFS：

```text
Vanilla
  ↓ DLC overlays
  ↓ low-priority mods
  ↓ ...
  ↓ high-priority mods
```

然后解析最终可见文件；对于不会以整文件覆盖而是以 ID 合并的内容类型，再在内容层解析 duplicate ID。

不能只做“数据库里同 ID 最后一条获胜”，因为游戏覆盖可能先发生在文件层。

## 12.3 查询规则

单项查询：

```text
Mod effective index
    ↓ missing
Enabled Base catalog
    ↓ missing
Unresolved reference
```

列表查询：

```text
Effective = Mod + (Enabled Base - ModLogicalIds)
```

不能简单 concat，否则被覆盖内容会重复出现。

## 12.4 Repository 门面

编辑器只依赖：

```text
IContentEnvironment
├── findHeroClass(id)
├── findTrinket(id)
├── findQuirk(id)
├── listTrinkets(filter)
├── resolveLocalization(key, language)
├── resolveAsset(logicalId)
└── explainProvenance(type, id)
```

内部可以使用两个 SQLite connection，或在 Repository 层显式进行 Mod-first fallback。跨库 SQL `ATTACH` 可用于少量查询优化，但不应隐藏业务覆盖语义。

## 12.5 Localization

解析顺序：

```text
Highest effective Mod localization
    ↓
Enabled DLC localization
    ↓
Vanilla current language
    ↓
English fallback
    ↓
Raw ID
```

本地化缺失不使内容定义无效，只产生 Warning。

## 12.6 Asset

Asset Index 保存逻辑引用，不复制游戏资产：

```text
AssetRecord
├── logicalId
├── assetType
├── sourceId
├── virtualPath
├── variants[]
└── fingerprint
```

解析同样遵循 Mod-first、Base fallback。文件不存在时返回 Missing Asset 诊断，不导致整个实体解析失败。

---

# 13. Mapping、Raw Save 与 Semantic Model

## 13.1 Mapping Registry

每个可编辑语义属性必须登记：

```text
MappingSpecification
├── semanticProperty
├── saveDomain
├── documentId
├── objectLocator
├── fieldSelector
├── expectedDsonType
├── readConversion
├── writeConversion
├── coupledMappings[]
├── validators[]
├── evidenceLevel
├── writable
└── notes
```

字段路径不得散落在 Operation、Controller 或 GUI DTO 中。

## 13.2 Controlled Mutation

写入 Mapping 的建立流程：

```text
Prepare Save A
    ↓ perform one controlled action in game
Prepare Save B
    ↓ DSON structural diff
Identify candidate fields
    ↓ repeat / reverse action
Confirm coupled changes
    ↓ editor mutation on copy
Load in game
    ↓
Promote evidence level
```

一次只改变一个变量，例如武器 +1、装备某个饰品、增加一个怪癖。多个同时变化的存档不适合作为 Mapping 证据。

## 13.3 Semantic Model

第一阶段模型：

```text
Campaign
├── Heroes
├── Estate
├── Town
├── Resources
├── TrinketInventory
├── Buildings
└── ProgressionSummary
```

Hero：

```text
Hero
├── identity
├── definitionRef
├── presentation
├── progression
├── equipment
├── combatSkills
├── campingSkills
├── quirks
├── diseases
├── trinkets
├── condition
├── runtimeState
├── resolutionState
└── extensions
```

## 13.4 Partial Entity

```text
Resolved     → fully editable
Partial      → only verified safe properties editable
Unresolved   → read-only, raw reference preserved
Invalid      → diagnostics only
```

一个 Mod Hero 解析失败不能阻止其他 Hero 的读取与编辑。

## 13.5 规则来源

最大等级、装备阶段、技能阶段、槽位容量等来自 Effective Environment 与实际定义。禁止硬编码 `maxLevel = 6`、`maxSkill = 5` 等原版常数。

## 13.6 Hero 创建与复制

- Clone Hero：深复制已验证结构，生成新 persistent identity，并清理不能复制的 runtime 引用；
- Add Hero：只能由 `HeroFactory` 根据环境定义和经过验证的模板构造；
- 在最小合法模板未通过 `VERIFIED_GAME` 前，Add Hero 保持实验或关闭；
- Delete Hero 属于高风险 Operation，需检查队列、队伍、活动和跨文档引用。

---

# 14. Operation、Session 与 ChangeSet

## 14.1 Editor Session

```text
EditorSession
├── profileDescriptor
├── rawBaseline
├── semanticBaseline
├── workingModel
├── operationLog
├── undoStack
├── redoStack
├── validationState
└── dirtyDocuments
```

## 14.2 Operation

```text
EditorOperation
├── id
├── name
├── target
├── before
├── after
├── risk
├── affectedSemanticProperties[]
└── timestamp
```

原子 Operation 示例：

```text
SetHeroLevel
SetWeaponStage
ReplaceQuirk
RemoveDisease
AddInventoryTrinket
SetResourceAmount
UnlockBuildingNode
```

复合 Operation 示例：

```text
MaxHero
MaxBuilding
RestoreNormalCondition
```

复合操作必须作为一个 Undo 单元。

## 14.3 ChangeSet

Commit 时从 Operation Log 生成 ChangeSet：

```text
ChangeSet
├── operations[]
├── semanticDiff
├── affectedMappings[]
├── affectedDocuments[]
├── riskSummary
└── warnings[]
```

不能通过比较 GUI 当前值来猜测修改。

## 14.4 Undo / Redo

- Operation 必须可确定性 apply/revert；
- 新操作发生后清空 redo stack；
- 失败的 Operation 不得进入 history；
- Undo 只影响工作模型，不直接触发磁盘写入；
- Commit 成功后当前状态成为新 baseline，历史策略由配置决定，第一版可清空。

---

# 15. Validation Architecture

验证分层：

| 层 | 检查内容 | 失败影响 |
| --- | --- | --- |
| Raw | Header、offset、类型、对象层级 | 文档不可写 |
| Mapping | locator 唯一、类型匹配、证据可写 | 对应操作不可用 |
| Semantic | 等级范围、阶段存在、ID 唯一 | Operation 或 Commit 失败 |
| Reference | class/trinket/quirk/asset 来源 | Warning 或只读 |
| Operation | 前置条件、目标状态、风险 | 操作拒绝 |
| Commit | candidate decode、cross-document consistency | 禁止写盘 |
| Read-back | 实际磁盘内容与 candidate 一致 | Commit 失败并进入恢复流程 |

Severity：

```text
Info / Warning / Error / Fatal
```

Risk 与 Severity 分开：

```text
Safe / Cautious / Experimental / Unknown
```

高风险不一定是错误；结构错误也不能因为用户选择 Sandbox 就放行。

---

# 16. Save Transaction 与备份

## 16.1 Commit Pipeline

```text
Freeze Session Revision
    ↓
Build ChangeSet
    ↓
Validate Semantic + Mapping
    ↓
Clone Raw AST
    ↓
Patch through Save Adapter
    ↓
Validate Raw Candidate
    ↓
Encode all affected documents in memory
    ↓
Decode candidates again
    ↓
Validate structure + semantic projection
    ↓
Recheck external file fingerprints
    ↓
Create complete profile backup
    ↓
Write temporary files
    ↓
Replace target files in deterministic order
    ↓
Read back and validate
    ↓
Commit new baseline
```

## 16.2 备份

每次 Commit 备份整个 profile：

```text
backup/<timestamp-profile-id>/
├── files/
├── manifest.json
└── changes.json
```

`manifest.json` 至少记录：

```text
editorVersion
timestamp
profileId
sourceFingerprints
gameBuild
enabledDlc
enabledMods
environmentFingerprint
```

`changes.json` 仅用于审计与展示，不用于恢复。

### 16.2.1 自动编辑保存

自动编辑保存与 Commit 备份使用同一个配置中的 `backupRoot`，但存放在独立目录：

```text
backupRoot/AutoEditSave/
└── recovery.json
```

自动编辑保存只在 Session 从 clean 变为 dirty 后启动。建议在第一次编辑后立即写入一个原子恢复点，
然后按 `autoEditSaveIntervalSeconds` 周期性写入。恢复点记录来源 Profile、来源 fingerprint、revision、
时间和可恢复 Session 快照；它不会修改游戏存档。显式 Commit 成功后清理当前恢复点并停止该 Session 的定时任务。

进程启动时扫描恢复点。恢复前必须重新打开来源 Profile 并比较 fingerprint；不匹配时进入外部修改诊断，
禁止静默套用快照。用户放弃恢复时写入作废标记，避免下次启动重复提示。

## 16.3 多文件写入

普通文件系统没有跨文件原子事务。策略是：

- 所有 candidate 在写盘前完成；
- 完整备份成功后才开始替换；
- 每个目标先写同目录临时文件并 flush；
- 能使用原子 rename 时使用；
- 固定替换顺序并记录 journal；
- 任一步失败均报告 Partial Commit，不伪装成功；
- 提供基于备份的一键恢复。

## 16.4 写后失败

如果写后 read-back 验证失败：

1. 禁止更新 Session baseline；
2. 保留 candidate、journal 与错误日志；
3. 提示存档当前状态不可信；
4. 自动恢复是否执行应保守，第一版建议由用户确认；
5. 恢复后再次 read-back 验证。

---

# 17. Application Service Layer

Application 层表达完整用例并管理事务边界。概念用例：

```text
InitializeApplication
InspectInstallation
RebuildBaseCatalog
RefreshModEnvironment
ListSaveProfiles
OpenSave
GetCampaignSummary
GetHero
ApplyOperation
Undo
Redo
ValidateSession
PreviewCommit
CommitSave
RestoreBackup
CloseSession
```

Application Service：

- 组合 Domain 与 Ports；
- 管理 Session、Job、事务与权限；
- 返回稳定 DTO 或 View Model；
- 不包含 SQL；
- 不解析 HTTP request；
- 不直接操作平台路径细节。

CLI、测试与 Drogon 必须调用同一层。

---

# 18. Drogon HTTP Adapter（最后阶段）

## 18.1 接入时机

只有以下条件满足后才接入 Drogon：

- DSON 与 Raw Save 可独立测试；
- Content Environment 可查询；
- 至少一组 Campaign Mapping 可写；
- Application Service 可由测试或临时 CLI 完成完整用例；
- Save Transaction 已通过真实副本测试。

Drogon 是 Transport Adapter，不是后端业务骨架。

## 18.2 Controller 边界

Controller 只做：

```text
HTTP request
→ parse / basic DTO validation
→ call Application Service
→ map result/error
→ HTTP response
```

Controller 禁止：

- 打开 SQLite；
- 扫描 Mod；
- 解析 DSON；
- 修改 Semantic Model；
- 创建备份；
- 决定覆盖规则。

## 18.3 API 风格

第一版使用 REST + JSON。推荐资源域：

```text
/api/status
/api/configuration
/api/environment
/api/jobs
/api/saves
/api/sessions
/api/backups
```

具体 endpoint 与 DTO 在 HTTP 阶段根据稳定 Application Service 设计，不在核心阶段提前冻结。

## 18.4 本地安全

- 默认只绑定 loopback；
- 校验 Origin；
- 使用启动期随机 session token 或同等本地保护，避免任意网页调用写接口；
- 写操作要求明确 session revision，防止陈旧页面覆盖新状态；
- 文件路径不直接作为 URL 参数暴露；
- 不提供任意文件读取 endpoint；
- 生产日志不返回堆栈与内部路径给 UI。

## 18.5 ORM 决策

第一版 Repository 使用自有轻量 SQLite wrapper。Drogon ORM 即使后续采用，也只能存在于 Infrastructure Repository 实现中，不能成为 Domain 或 Application 的依赖。除非有明确收益，不为“统一技术栈”而迁移已经稳定的 Repository。

---

# 19. 推荐工程目录

```text
/
├── CMakeLists.txt
├── cmake/
├── config/
├── data/
│   ├── base_content.db
│   └── environment.db
├── include/ddse/
│   ├── application/
│   ├── domain/
│   ├── ports/
│   └── core/
├── src/
│   ├── application/
│   ├── domain/
│   │   ├── campaign/
│   │   ├── content/
│   │   ├── operations/
│   │   └── validation/
│   ├── core/
│   │   ├── dson/
│   │   ├── mapping/
│   │   ├── overlay/
│   │   └── errors/
│   ├── infrastructure/
│   │   ├── filesystem/
│   │   ├── sqlite/
│   │   ├── logging/
│   │   ├── content_scanner/
│   │   ├── backup/
│   │   └── http_drogon/
│   └── composition_root/
├── migrations/
│   ├── base/
│   └── environment/
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── fixtures/
│   │   ├── dson/
│   │   ├── saves/
│   │   ├── content/
│   │   └── environments/
│   ├── golden/
│   └── helpers/
├── tools/
│   ├── dson_inspect/
│   ├── save_diff/
│   └── content_dump/
└── docs/
    ├── architecture/
    ├── mappings/
    ├── fixtures/
    └── adr/
```

开发工具可以调用 Core，但生产 Core 不能依赖 tools。

---

# 20. 分阶段实施路线

每个阶段形成小闭环：

```text
设计 → 实现 → 阶段测试 → 可观察结果 → 冻结最小接口 → 下一阶段
```

“冻结”不是永不修改，而是只有在新证据出现时有意识地修改，并补回归测试。

## Stage 0：工程骨架与可观测性

### 目标

建立 C++20、CMake、测试、日志、错误模型、配置、文件系统抽象和 SQLite 最小封装。

### 为什么先做

后续每个模块都需要一致的错误、日志、路径与测试入口。晚补这些设施会导致每个模块形成不同习惯。

### 推荐顺序

1. 建立 targets：core、application、infrastructure、tests；
2. 接入测试框架；
3. 定义 `Result/Error`；
4. 定义日志接口与实现；
5. 建立 `IFileSystem` 与本地实现；
6. 建立 SQLite RAII wrapper 与 migration runner；
7. 创建最小 Composition Root，不启动 HTTP。

### 阶段测试与预期结果

- Debug/Release 可构建；
- 单元测试可被 CTest 发现；
- 临时数据库能 migration、transaction rollback；
- 文件系统错误转换为统一 Error；
- 日志包含 module 与 operation。

### 常见错误与诊断

- 依赖循环：检查 CMake target link direction；
- SQLite statement 生命周期错误：检查 RAII 与 connection owner；
- 错误信息丢失：检查第三方错误到 `Error` 的 cause chain；
- 测试依赖真实用户路径：改用 temp directory 与 fixture。

### Definition of Done

- [ ] 干净环境一条命令构建并测试
- [ ] core 不链接 Drogon
- [ ] transaction rollback 有自动测试
- [ ] 文件、数据库、配置错误可结构化报告

## Stage 1：DSON Reader 与结构诊断

### 目标

解析所有当前样本 persist 文件，建立有序 Raw AST，但暂不写回。

### 为什么现在做

Save Discovery、Mapping 与 Semantic Model 的输入都是 DSON AST；上层不能建立在不稳定 Reader 上。

### 推荐顺序

1. Header 与 section 边界；
2. Meta1 / Meta2；
3. 字段层级与 parent relation；
4. 显式 value kind；
5. string hash；
6. arrays / vectors / pairs；
7. embedded DSON；
8. AST inspector 与结构摘要。

### 阶段测试与预期结果

- `profile_0` 的 16 个文件逐个产生确定的 parse report；
- 同一文件重复解析得到相同结构摘要；
- malformed fixture 返回 offset 和 section；
- 未识别类型不造成越界读取。

### 常见错误与诊断

- parent object mismatch：检查 meta parent index 与对象栈；
- 字段名错误：检查 hash 算法和字节序；
- 最后字段截断：检查长度、对齐与终止字节；
- 某文件可解析但树错位：输出 source offset 与 metadata 对照。

### Definition of Done

- [ ] 核心样本全部可解析或有精确、可复现的失败报告
- [ ] 字段顺序与重复字段可见
- [ ] Unknown 不会静默丢弃
- [ ] Parser 对损坏输入无越界与崩溃

## Stage 2：DSON Writer 与 Round-trip

### 目标

实现 Encode，并证明未修改 AST 能安全 round-trip。

### 为什么现在做

在 Writer 可靠前继续做业务写入会把格式错误与 Mapping 错误混在一起。

### 推荐顺序

1. 先支持 Reader 已确认的类型；
2. 保留 metadata 与原始顺序；
3. 实现 dirty field 编码策略；
4. 构造 `decode → encode → decode` 比较器；
5. 记录 binary difference 原因；
6. 对 Unknown 制定保留或只读策略。

### 阶段测试与预期结果

- 所有 fixture structural round-trip 通过；
- 未修改文档优先 binary identical；
- 单个已知标量变更只影响预期范围；
- encode 失败不产生磁盘文件。

### 常见错误与诊断

- 输出尺寸漂移：检查重复字段、对齐、metadata；
- 再解码失败：检查 offset 回填与 section 长度；
- 未修改字段改变：检查类型重新推断和 canonicalization；
- Unknown 无法输出：将整份文档降级为 read-only。

### Definition of Done

- [ ] Round-trip matrix 全绿
- [ ] 差异报告能定位字段与 offset
- [ ] Unknown 策略明确并测试
- [ ] Writer 没有任何 Hero/Content 业务知识

## Stage 3：Save Discovery 与 Raw Save Profile

### 目标

发现 profile、识别 Domain、装载全部文档并记录 baseline fingerprint。

### 为什么现在做

DSON 已稳定后，才能把单文件能力组织成完整存档会话。

### 测试与预期结果

- `profile_0` 识别为 Campaign；
- Circus fixture 识别为 Circus；
- 混合 profile 支持多 Domain；
- 未知文件被注册而非丢弃；
- 单个非核心文档损坏产生 Partial 状态。

### Definition of Done

- [ ] Profile descriptor 稳定
- [ ] Document registry 不依赖固定文件全集
- [ ] baseline fingerprint 可检测外部修改
- [ ] Raw 层仍无 Semantic Hero 逻辑

## Stage 4：Base Content Scanner

### 目标

用统一 Parser 扫描 Vanilla 与 DLC，原子构建 `base_content.db`。

### 为什么现在做

Semantic Save 的 ID、等级、技能与资产解释依赖内容环境。先做 Base 可建立最简单、稳定的查询基线。

### 推荐顺序

1. ContentSource 与 Source File；
2. Hero Class；
3. Skill；
4. Trinket；
5. Quirk / Disease；
6. Resource / Building；
7. Localization；
8. Asset；
9. Hash index；
10. Atomic database rebuild。

### 可观察结果

命令或测试报告应能输出数量与抽样查询，例如：

```text
sources: ...
hero classes: ...
trinkets: ...
quirks: ...
localization entries: ...
assets: ...
```

并能解释一个内容 ID 的来源、名称与资产路径。

### 常见错误

- 数量异常少：检查 glob、大小写和 source roots；
- 相同文件重复扫描：检查 VirtualPath 规范化；
- 中文缺失：检查语言文件与 fallback，不要把内容判无效；
- DB 构建中断后旧库消失：违反 `.new + atomic replace` 规则。

### Definition of Done

- [ ] 重复扫描结果确定
- [ ] Base 来源与 DLC 边界可查询
- [ ] 旧库在失败时保持可用
- [ ] Parser 可脱离 SQLite 单测

## Stage 5：Mod Environment Scanner 与 Overlay

### 目标

扫描当前 Mod、解析 load order、构建 Effective VFS 与 `environment.db`。

### 为什么现在做

先有 Base 基线，才能单独验证 Mod 覆盖、冲突与 fallback，而不把两类问题混在一起。

### 推荐顺序

1. Workshop / Local source discovery；
2. enabled 状态；
3. load order evidence；
4. Effective VFS；
5. 统一 Parser；
6. duplicate ID resolution；
7. localization / asset override；
8. warnings；
9. environment atomic rebuild。

### 测试与预期结果

- 构造 Mod A/Mod B fixture 验证同路径 winner；
- 验证不同文件同 ID 的内容 winner；
- `Base: A,B,C; Mod: B,D` 得到 `A,B(mod),C,D`；
- load order 不明时产生明确 Unresolved，不按文件夹名猜；
- 200 个模拟 Mod 的扫描失败被隔离并形成汇总。

### Definition of Done

- [ ] 更新 Mod 不修改 base DB
- [ ] override chain 可解释
- [ ] Environment degraded 状态可见
- [ ] 列表无重复有效 ID
- [ ] Localization 与 Asset 使用同一覆盖原则

## Stage 6：Effective Content Environment

### 目标

通过统一 Repository 门面提供 Mod-first、Base fallback 查询。

### 为什么现在做

上层不应知道数据位于哪一个数据库；先稳定门面，Semantic Model 才不会依赖存储细节。

### 测试与预期结果

- enabled DLC 过滤正确；
- 单项、列表、搜索结果一致；
- provenance 可以解释 winner 与 shadowed definitions；
- Hash ambiguity 不被强行解析；
- 缺失资产不阻止内容查询。

### Definition of Done

- [ ] Application 只依赖 `IContentEnvironment`
- [ ] Repository contract tests 覆盖内存与 SQLite 实现
- [ ] 搜索和详情返回同一有效定义

## Stage 7：Campaign Mapping Research

### 目标

使用 `profile_0` 与受控 A/B 样本建立正式 Mapping Registry。

### 为什么现在做

此时格式和环境均已稳定，可以把差异解释为真实业务变化，而不是 Codec 或内容解析噪声。

### 推荐顺序

从低风险到高风险：

1. 资源数值；
2. 英雄名称；
3. Stress；
4. 饰品库存；
5. 已有 Hero 等级；
6. 武器 / 护甲；
7. 战斗技能；
8. Quirk / Disease；
9. 建筑；
10. Clone / Create / Delete Hero。

### 阶段产物

每个属性产生一份 mapping record、A/B diff、证据等级、写后游戏验证结果和回归 fixture。

### 常见错误

- 一次改多个变量：无法归因，重新制作样本；
- 只看字段名：可能漏掉索引或历史字段；
- 只验证游戏能启动：还需进入对应界面确认状态；
- 将一个版本的观察推广到所有 Mod：证据范围标注不足。

### Definition of Done

- [ ] 第一批可编辑属性达到 `VERIFIED_MUTATION`
- [ ] 每个 writable mapping 有 fixture
- [ ] coupled mappings 明确
- [ ] 未验证属性仍为只读

## Stage 8：Semantic Campaign Model

### 目标

将 Raw Save 与 Effective Environment 组合为 Partial-tolerant Semantic Model。

### 为什么现在做

Mapping 与内容查询已提供可靠输入，此时 Domain 不再建立在猜测字段上。

### 测试与预期结果

- `profile_0` 生成 campaign summary；
- 已知 ID 解析为名称、来源、规则与资产引用；
- 缺失 Mod 引用保留 Raw ID 并变为 Unresolved；
- 一个坏 Hero 不阻塞整个 roster；
- Semantic → Raw locator 可追踪用于诊断。

### Definition of Done

- [x] Domain 无 SQLite/Drogon 类型
- [x] Partial Entity 权限正确
- [x] 最大等级等规则无硬编码
- [x] 只读投影与原始字段可追踪

## Stage 9：Operation、Undo、Validation

### 目标

建立 Edit-in-Memory 业务闭环。

### 为什么现在做

先证明 Domain 能稳定读取，再让修改通过明确 Operation 进入；避免业务代码直接 patch AST。

### 测试与预期结果

- [x] 每个 Operation 的 apply/revert 对称；
- [x] Composite Operation 一次 Undo；
- [x] invalid target 不改变模型；
- [x] missing mapping 拒绝操作；
- [x] ChangeSet 精确列出 affected documents；
- [x] 未相关实体保持不变。

### Definition of Done

- [x] GUI 无关的操作测试全绿
- [x] Undo/Redo 无状态漂移
- [x] Risk 与 Validation 分离
- [x] Session revision 可阻止陈旧修改

Stage 9 实现了只作用于内存语义模型的资源与英雄标量字段操作。所有字段仍没有游戏内修改证据；风险分析会明确报告这一点，Stage 10 才负责将 ChangeSet 安全写回存档。

## Stage 10：Save Adapter 与 Safe Commit

### 目标

完成从 ChangeSet 到 clone AST patch、candidate validation、backup、write、read-back 的事务。

### 为什么现在做

此前所有阶段都可在内存中失败；本阶段首次把候选字节写入磁盘。Stage 10 的提交目标必须是与源 profile 完全一致的独立副本，不能直接写入用户的源存档。真实游戏存档写入仍须等待后续受控游戏测试，并由 Mapping 单独记录写入证据。

### 测试与预期结果

- 在存档副本上提交单字段修改；
- binary/structural diff 只涉及预期范围；
- backup 失败时零写入；
- 第二文件写入失败时能识别 Partial Commit；
- 外部修改 fingerprint 后拒绝覆盖；
- write-back 文件能重新 decode 并投影出目标语义。

### Definition of Done

- [x] 所有写入均经过事务，且只写入调用方明确提供的独立 profile 副本
- [x] 完整 profile backup 可恢复；包含子目录、未知文件和空目录，并以 fingerprint 校验
- [x] candidate 在写盘前全部生成并验证
- [x] read-back validation 存在
- [x] 写入中断和回读失败会报告失败或 Partial Commit，并返回恢复备份位置

Stage 10 自动测试使用临时 profile 副本，不修改项目提供的测试存档，也不代表游戏内写入已验证。当前没有生成游戏测试脚本；是否及如何做游戏内验证留待单独确定。

## Stage 11：Application Services

### 目标

把环境、存档、Session、Operation、Commit 组装成完整用例。

### 为什么现在做

这是从“各模块可用”走向“软件协调运行”的关键阶段，也是 Composition Root 真正发挥作用的地方。

### 测试与预期结果

通过测试驱动或临时 CLI 完成：

```text
initialize
→ load environment
→ list profiles
→ open profile
→ inspect hero
→ apply operation
→ preview changes
→ commit to copy
→ reopen and verify
```

### Definition of Done

- [ ] 完整流程不需要 HTTP
- [ ] 每个用例有清晰事务边界
- [ ] Job、Session 与 Repository 生命周期明确
- [ ] Composition Root 可替换测试 doubles

## Stage 12：功能纵切片扩展

### 目标

按纵切片逐步加入业务功能，而不是先建所有类。

推荐顺序：

```text
Resources
→ Existing Hero basic fields
→ Trinket Inventory
→ Equipment and Skills
→ Quirk / Disease
→ Buildings
→ Clone Hero
→ Add / Delete Hero
```

每个切片必须同时完成：

```text
Mapping
Domain property
Operation
Validation
Save Adapter patch
Commit integration test
Regression fixture
```

只完成 Domain class 而没有安全写回，不算功能完成。

### Stage 12 实施记录（2026-09-21）

- [x] 接通已通过游戏验证的英雄、怪癖、饰品、技能、装备等级、小镇升级和小镇建筑系统操作。
- [x] 购买节点按 `tree_id + instance_number + requirement_code` 定位，并可按有效 Mod 升级树上限处理稀疏存档行。
- [x] 结构编辑经过 Operation/Mapping allowlist、SaveAdapter 候选回读和 SafeSaveCommitter 完整备份/目标副本写回。
- [x] 从 33 人 roster、122 个已启用 Mod 的测试存档生成 17 个独立操作验收档与 1 个对照档；生成时 Mod 扫描诊断为 0，源档指纹前后未变。
- [x] 原有 17 个操作档已完成游戏内验收；补充档覆盖负面怪癖删除再新增、英雄压力/折磨状态和首位英雄改名。
- [ ] 玩家仍需按 `test_save_profile/stage12_followup_tests/README.md` 验收这 3 个补充档。
- [x] 未验收字段使用显式 `AcceptanceTestCandidate` 生成隔离测试副本；默认 SafeSaveCommitter 仍只接受已通过游戏验收的 Mapping。
- 暂缓项继续保持不可用：生存技能训练锁定和疾病增删。

自动检查确认候选 DSON 可重新解析、Domain Model 可重新投影、预期变更与购买节点状态一致；它不替代游戏内验收。

## Stage 13：总体资格测试

### 目标

验证系统在真实组合环境下可靠，而不是重复各模块单测。

测试矩阵至少包括：

```text
Vanilla
All official DLC
Lightly modded
Heavily modded
Custom hero mods
Custom trinket mods
Missing-mod save
Large roster
Campaign profile
Circus-only profile
Malformed non-core document
Externally modified open profile
```

发布资格：

- 所有稳定功能在游戏中回读验证；
- 无 silent data loss；
- 备份恢复演练通过；
- 旧环境数据库迁移通过；
- 大型环境扫描时间和内存处于可接受范围；
- 崩溃后不会遗留“看似成功”的状态。

## Stage 14：Drogon 与 UI 接洽

### 目标

学习并接入成熟 C++ Web 框架，把稳定 Application Service 暴露给前端。

### 推荐顺序

1. 启动与优雅关闭；
2. health/status；
3. 错误到 HTTP status/JSON 的统一映射；
4. environment 与 job 查询；
5. save/session 只读查询；
6. operation endpoint；
7. preview/commit；
8. origin/token/revision protection；
9. 前端联调；
10. 打包与进程生命周期。

### 测试与预期结果

- Controller test 不访问真实磁盘；
- Application integration test 不启动 HTTP；
- API contract 对错误码稳定；
- 重复提交、陈旧 revision、无效 token 被拒绝；
- 长扫描通过 job polling 可观察；
- UI 关闭后服务能够正确退出或按产品策略驻留。

### Definition of Done

- [ ] Controller 无业务逻辑
- [ ] Core/Application 不链接 Drogon
- [ ] 所有写接口有本地请求保护
- [ ] HTTP DTO 与 Domain Model 分离
- [ ] 不启动 UI 也能跑后端测试

---

# 21. 测试策略

测试细节已经嵌入每个阶段。本节只规定跨阶段原则。

## 21.1 测试金字塔

```text
大量：纯函数、Parser、Operation、Validator 单测
中量：SQLite Repository、Scanner、Adapter 集成测试
少量：真实 profile commit、游戏回读、HTTP E2E
```

## 21.2 Fixture 管理

每个 fixture 配套 metadata：

```text
origin
gameVersion
dlc
mods
expectedDomains
expectedFeatures
anonymization
knownLimitations
evidenceLevel
```

Fixture 中的真实用户名称、路径和不必要个人信息应匿名化。原始存档不直接进入公开仓库，除非得到明确授权。

## 21.3 Golden 与结构断言

不要只断言“大文件 hash 相等”。同时保存可读结构摘要，使失败能指出：

```text
document
object path
field occurrence
type
old/new value
source offset
```

## 21.4 回归规则

每个已修复 bug 至少产生一个最小回归 fixture 或单元测试。仅在 issue 中描述复现步骤不算完成。

---

# 22. Diagnostics 与可支持性

后端诊断信息必须能回答：

- 为什么某个内容存在或不存在？
- 它来自哪个 Source 和文件？
- 哪个 Mod 覆盖了哪个定义？
- 为什么某个属性只读？
- 哪个 Mapping 被使用？证据等级是什么？
- Commit 修改了哪些文档？
- 为什么扫描或写回失败？

建议提供只读诊断 DTO：

```text
ContentExplanation
MappingExplanation
SessionDiagnostics
CommitReport
EnvironmentHealth
```

诊断能力不是开发期临时代码，而是 Mod-heavy 工具的正式功能。

---

# 23. 关键 Architecture Decisions

## ADR-001：C++20 Core，Drogon 延后

**决定：** 核心后端先以独立 C++20 library 与 Application Service 成立，最后接入 Drogon。  
**原因：** 避免 Controller 驱动架构，并把学习重点放在模块装配、依赖方向和完整用例。  
**后果：** 早期用测试或轻量开发工具调用 Application 层。

## ADR-002：Base 与 Environment 分库

**决定：** 使用 `base_content.db` 与 `environment.db`。  
**原因：** Vanilla/DLC 与 Mod 环境生命周期不同。  
**后果：** Repository 负责组合查询；更新 Mod 不重扫 Base。

## ADR-003：Effective Environment 是运行时视图

**决定：** 不维护第三套完整 effective database。  
**原因：** 避免复制 Base、复杂失效和来源丢失。  
**后果：** 单项、列表、Localization、Asset 查询统一实现 override/fallback。

## ADR-004：Raw AST 有序且保留未知数据

**决定：** DSON object 使用 ordered field list。  
**原因：** 字段顺序、重复字段、metadata 与未知类型可能影响无损写回。  
**后果：** 不能用普通 JSON object 替代 Raw AST。

## ADR-005：写入必须由 Mapping 与 Operation 驱动

**决定：** 所有写入需 writable Mapping，并由 Operation 生成 ChangeSet。  
**原因：** 防止字段路径散落与不可审计修改。  
**后果：** 新功能必须完成纵切片才能标记 Done。

## ADR-006：全 profile 备份

**决定：** 每次 Commit 备份完整 profile。  
**原因：** 存档体积小，多文件联动与恢复可靠性更重要。  
**后果：** 备份失败则不写盘。

## ADR-007：用户主动更新环境

**决定：** 第一版不后台监听游戏和 Mod 目录。  
**原因：** 编辑器不是环境管理器；主动更新更可预测。  
**后果：** 使用 Fingerprint 提示过期，并提供重建按钮。

---

# 24. 工程规则清单

1. GUI、HTTP 与 CLI 永远不直接访问 Raw DSON。
2. Domain 永远不依赖 Drogon、SQLite 或平台文件 API。
3. 未知 Raw 数据不得因编辑器不理解而删除。
4. 所有 Save mutation 必须经过 Operation。
5. 所有可写字段必须存在带证据等级的 Mapping。
6. 所有磁盘写入必须经过 Save Transaction。
7. 任何目标文件写入前必须完成 candidate 编码与完整备份。
8. Gameplay gate 不等于 structural constraint。
9. 等级、技能、装备和容量不得硬编码原版常数。
10. Mod、DLC、Localization、Asset 属于核心环境系统。
11. Parser 不直接依赖数据库，Repository 不决定游戏解析语义。
12. 更新 Mod 环境不得修改 Base Catalog。
13. 无法确认 Mod 顺序时必须暴露不确定性，不能猜测。
14. 一个实体失败不得无条件拖垮整个 profile。
15. 失败的写回不得报告成功。

---

# 25. 当前可立即开始的工作

正式编码从 Stage 0 开始。最小首轮里程碑不是“打开网页”，而是：

```text
C++20 project builds
    ↓
tests and structured logging work
    ↓
profile_0 files can be enumerated
    ↓
first DSON header/meta parser produces diagnostics
```

随后按以下主线推进：

```text
DSON Reader
→ DSON Writer / Round-trip
→ Raw Save Profile
→ Base Catalog
→ Mod Environment
→ Effective Content Environment
→ Campaign Mapping
→ Semantic Model
→ Operations / Validation
→ Safe Commit
→ Application Services
→ Feature slices
→ Qualification
→ Drogon / UI
```

最需要优先做扎实的四块是：

```text
DSON Core
Content / Overlay Parser
Campaign Mapping
Save Transaction
```

它们决定编辑器能否长期安全演进；Drogon 和 UI 负责把已经成立的能力交付给用户，而不定义这些能力本身。

---

# 26. F1 数据库初始化与 Mod 查询实现

F1 的初始化由本地服务托管的 `DatabaseInitializationManager` 执行，并通过轮询接口暴露状态。任务分为原版基础库和当前 Mod 环境库两个阶段，分别记录耗时与诊断。

- `dataRoot/base_content.db` 存在且能打开并包含基础 schema 时复用；文件缺失或 schema 不完整才重新扫描游戏目录。
- `dataRoot/mod_environment.db` 按当前 Profile、Workshop 和本地 Mod 配置重建，原版库不因 Mod 环境刷新而重复扫描。
- 单个 Mod 无法找到或内容损坏会记录诊断，仍提交可用的其他 Mod；后续语义编辑根据内容是否可解析决定能力是否开放。
- `GET /api/database/mods` 只查询环境库中的 `mod_order_entries` 和 `mod_sources`，不再让 HTTP Controller 直接扫描存档或 Mod 目录。
- Mod 封面通过数据库中的 `root_path` 查找受限文件名（`preview.*`、`cover.*`、`mod_preview.*`），缺失封面不影响 Mod 条目。

初始化状态完成后返回原版、Mod 环境和总耗时，供后续性能优化比较；完整配置启动服务时自动开始初始化，保存设置也会重新触发任务。

---

# 27. 最终基线

本项目不是“能修改几个 DSON 字段的网页后端”，而是一个本地运行的游戏状态编辑系统：

> **Preserve raw data, resolve the effective game environment, model save semantics, apply explicit operations, and validate every commit.**

对应中文：

> **保留原始数据，解析当前有效游戏环境，建模存档语义，通过明确操作修改，并验证每一次提交。**

本文档自 v1.0 起作为后端实现的 Architecture Baseline。任何绕过 Application Service、Mapping Registry 或 Save Transaction 的捷径，都应视为架构偏离并在合并前纠正。
