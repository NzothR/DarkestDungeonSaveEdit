# 核心编辑功能实测与 UI 集成参考

**状态：**Stage 12 已把此前通过游戏验证的核心操作接入 Operation、Mapping 与安全写回；负面怪癖删除后新增、英雄改名、压力数值和折磨状态写回已实现并生成待游戏验收存档。生存技能锁定和疾病增删暂缓。
**记录日期：**2026-09-21  
**测试环境：**`test_save_profile/profile_0`，存档内的 Mod 启用顺序为准。最近一轮扫描到 137 个已安装 Mod，其中 122 个启用；内容扫描诊断为 0。  
**已验证记录：**[Stage 11 测试档说明](../test_save_profile/stage11_advanced_game_tests/README.md)；**Stage 12 待验收档：**[操作写回测试档说明](../test_save_profile/stage12_operation_tests/README.md)、[本轮补充测试说明](../test_save_profile/stage12_followup_tests/README.md)

## 1. 目的与验收边界

本文记录玩家使用生成的测试存档在游戏内确认的编辑结果，并为后续 UI 接入提供语义边界和后端实现顺序。

游戏内通过表示相应 DSON 存档状态能被当前游戏与 Mod 环境正确读取、显示并保存。它不自动代表相应编辑操作已经接入 `CampaignEditSession`、`SaveAdapter` 或 UI。正式 UI 写入仍须经过 Mapping、Operation、校验、候选存档回读和安全提交流程。

此前 Stage 12 的 17 个操作档已由你确认通过。本轮新增的 3 个补充档也各自从当前源存档独立生成；源存档指纹在生成前后相同，每项另有 SafeSaveCommitter 校验过的写入前备份。补充档仍需你导入游戏验收。

## 2. 测试结果总览

### 2.1 基础编辑测试

| 功能 | 结果 | 备注 |
|---|---|---|
| 基础资源修改 | 通过 | 一次修改存档中的全部基础资源，并在游戏中核对目标数值。 |
| 新增 Mod 英雄 | 通过 | 新英雄加入名单末尾；职业、来源 Mod 和初始技能/装备状态可在游戏中确认。 |
| 替换正面与负面怪癖 | 原测试通过；负面删除后新增待验收 | 新接口不再把负面怪癖当作可替换定义，而是删除后在原有序位新增。 |
| 给英雄增加正面与负面怪癖 | 通过 | 按存档和 Mod 环境支持的怪癖容量选择目标英雄。 |
| 饰品库存新增饰品 | 通过 | 新饰品名称和 Mod 来源可在游戏内核对。 |

### 2.2 进阶编辑测试

| 编号 | 功能与本轮目标 | 游戏内结果 |
|---:|---|---|
| 1 | 将 Scourge（名单第 2 位）的 `Scourge_Skill_1` 升至该 Mod 定义的满级，英雄等级不变 | **通过**。实际测试从 2 个购买节点升至 5 个；原存档没有符合条件的低级、零购买节点技能，因此使用了部分已升级的技能。 |
| 2 | 将 Astraljk（名单第 1 位）的满级战斗技能降至 2 级，并回锁后续节点 | **通过**。 |
| 3 | 解锁一个生存技能，不改变英雄等级 | **通过**。以训练购买状态表示“已学会”。 |
| 4 | 锁定一个已经学会的生存技能 | **暂缓**。最新测试仍未实现预期的训练营锁定效果，不能作为可用功能或 UI 操作发布。 |
| 5 | 锁定一个符合游戏锁定条件的正面怪癖 | **通过**。 |
| 6 | 直接移除一个正面和一个负面怪癖 | **通过**。沙盒编辑器不按游戏疗养院规则限制移除。 |
| 7 | 升级英雄武器和防具，目标等级不同 | **通过**。Scourge 的武器 2→4、防具 2→5。 |
| 8 | 降级英雄武器和防具，目标等级不同 | **通过**。Astraljk 的武器 5→2、防具 5→1。 |
| 9 | 给英雄增加一种原版疾病 | **暂缓**。测试档未在游戏中显示新增疾病；目前不提供疾病新增入口。 |
| 10 | 移除英雄疾病 | **待有疾病样本后测试**。当前测试存档没有疾病英雄。 |
| 11 | 一名英雄降级、另一名英雄升级 | **通过**。通过修改经验值实现等级变化。 |
| 12 | 解锁一个小镇建筑 | **通过**。目标为 `geologic_studyhall`。 |
| 13 | 锁定一个已解锁的小镇建筑 | **通过**。与第 12 项使用同一目标建筑。 |
| 14 | 修改小镇建筑升级进度 | **通过**。马车名单容量 `stage_coach.rostersize` 设为 3 级，与第 15 项的 1 级状态对照。 |
| 15 | 降低马车名单容量并回锁后续升级节点 | **通过**。节点 a 保留，b–e 回锁。 |
| 16 | 给英雄装备原版通用饰品和 Mod 职业专属饰品 | **通过**。Scourge 装备 `aakeskiol` 和职业限制为 Scourge 的 Mod 饰品 `Scourge_1`。 |
| 17 | 销毁英雄装备栏最后一个饰品 | **通过**。饰品被销毁，不回到库存。 |
| 18 | 解锁小镇建筑系统 | **通过**。系统状态开放，但未建造具体建筑。 |
| 19 | 锁定小镇建筑系统 | **通过**。清除 `persist.town.json` 中的 `base_root/districts` 状态。 |
| 20 | 销毁饰品物品栏第一个饰品 | **通过**。库存条目被销毁。 |

### 2.3 暂缓项

- **生存技能锁定（第 4 项）**：停止继续尝试和暴露该功能。以后的实现需要先查清游戏训练状态的实际存档语义，再单独生成小型对照档并做游戏内验证。
- **疾病新增（第 9 项）和疾病移除（第 10 项）**：等待一个包含真实疾病的存档样本。拿到样本后，先确认疾病记录格式，再优先测试移除现有疾病；暂不依赖疾病新增用例。
- **取消装备生存技能**：之前第 4 项的写入实际表现为从已装备技能列表取消选中。它与“训练营锁定”是两个不同功能，可在将来作为独立操作实现和验收，不沿用“锁定”名称。

## 3. 给 UI 和 Application 层的语义约定

### 3.1 技能状态必须拆分

战斗技能等级、生存技能是否学会、生存技能是否装备，是不同状态，不应共用一个“技能等级”字段或一个 UI 开关。

| 用户操作 | 保存语义 | 备注 |
|---|---|---|
| 提升/降低战斗技能等级 | `persist.upgrades.json` 中对应职业、技能和英雄实例的购买节点 | 必须根据当前有效 Mod 定义获取等级上限；不同技能的上限可能不同。不要顺带修改英雄等级。 |
| 学会生存技能 | 生存训练购买状态，保存在升级购买数据中 | 第 3 项游戏内通过。 |
| 装备/取消装备生存技能 | 英雄的 `selected_camping_skills` 集合 | 与训练是否学会分开；第 4 项旧写入展示了取消装备效果。 |
| 锁定已学会的生存技能 | **尚未确认** | 第 4 项暂缓，不应映射成删除 `selected_camping_skills` 条目。 |

### 3.2 经验值、等级与装备购买状态

- 英雄存档保存 `resolveXp`；UI 显示的 Resolve 等级应由当前游戏进度规则计算。编辑“等级”时应提供语义操作，再将目标等级转换为有效经验值。
- 武器/防具等级同时涉及英雄嵌入存档字段和 `persist.upgrades.json` 购买节点。将这类变更作为一个复合操作提交，不能只改展示字段。
- 建筑升级等级由购买节点表示。Town 中的当前状态、District 是否开放、单个 District 是否已建造和建筑升级历史应分别建模。

### 3.3 怪癖、疾病和饰品

- 怪癖 ID 是稳定的内容键；界面显示本地化名称时仍需保留 ID、来源 Mod 和正负面/可锁定等属性。
- 怪癖锁定与移除是两种操作。第 5 项验证了可锁定正面怪癖；第 6 项验证沙盒移除允许直接删除正面或负面怪癖。
- 疾病目前只有“读取样本结构”的基础，新增未通过、移除未测试；UI 必须将疾病编辑保持为不可用，直到有疾病样本和游戏验证。
- 饰品库存与英雄装备栏分别位于庄园和英雄数据中。装备、从英雄卸下、从库存销毁、销毁已装备饰品不能混成一个删除按钮。
- 新增英雄饰品时，应使用有效内容环境检查饰品职业限制。第 16 项使用了与目标英雄相符的 Mod 专属饰品和一个原版通用饰品。

### 3.4 小镇建筑与小镇建筑系统

- **小镇建筑系统开放状态**由 `persist.town.json` 的 `base_root/districts` 状态承载；第 18/19 项通过。
- **单个小镇建筑是否建成**由 `base_root/districts/buildings/{districtId}/built` 承载；第 12/13 项通过。
- **建筑升级进度**由 `persist.upgrades.json` 购买节点承载；第 14/15 项通过。

UI 应把“开放系统”“建造/锁定一栋建筑”“改变建筑升级”分开呈现和提交。

前端建筑名单以集中列表呈现普通建筑，不显示错误的统一升降级值。普通建筑编辑窗从原版 `upgrades/building/*.upgrades.json` 读取独立升级链，节点左键升一级、右键降一级，Shift 左键升满、Shift 右键降到零级；名单顶部的一键操作升满所有普通建筑，单栋窗口可升满当前建筑。区域建筑进入独立窗口，名单状态来自 `base_root/districts/buildings/{districtId}/built`，系统锁定状态来自 `base_root/districts`；窗口提供逐栋解锁/锁定、批量解锁、批量锁定并清除区域系统状态，以及系统开关。所有操作进入 Campaign Session，复用撤销/重做与备份后写回流程。

## 4. Operation、Mapping 与安全写回接入情况

本轮测试通过的行为现在有一部分已贯通 Application Operation → Mapping → SaveAdapter → SafeSaveCommitter。游戏测试证明数据语义可用；下表说明哪些操作已具备正式安全写回接口。

### 4.1 已接通写回的操作

| Operation | Mapping | 状态 |
|---|---|---|
| `SetCampaignValueOperation` / `Estate.Resource.Amount` | `persist.estate.json` 钱包金额 | 可写候选并可提交；游戏已验证 |
| `SetCampaignValueOperation` / `Hero.ResolveXp` | 英雄内嵌 DSON 的 `resolveXp` | 可写候选并可提交；等级变化已验证，等级门槛由上层规则决定 |
| `SetCampaignValueOperation` / `Hero.Stress` | 英雄压力值 | 可设置非负有限值；设为 0 即清空压力，候选写回待游戏内验收 |
| `SetHeroAfflictionStateOperation` | 折磨 ID、严重度与任务内美德 ID | 只支持折磨和非折磨两种状态；设置/清除状态时清理冲突字段，候选写回待游戏内验收 |
| `SetCampaignValueOperation` / `Hero.Name` | 英雄内嵌 DSON 的 `actor/name` | 可生成候选档；独立改名等待游戏内验收 |
| `SetHeroQuirkLockedOperation` | 英雄怪癖 `is_locked` | 可写候选并可提交；Operation 会检查正面、非疾病、定义允许锁定 |
| `RemoveHeroQuirkOperation` | 完整怪癖对象 | 可写候选并可提交；按 ID 精确移除完整子树 |
| `SetDistrictBuiltOperation` | `persist.town.json` 的 `built` | 可写候选并可提交；需要存档中已经存在小镇建筑系统状态 |
| `DestroyTrinketOperation`（英雄装备栏） | 英雄 `trinkets/items/{key}` | 可写候选并可提交；销毁，不转入库存 |
| `DestroyTrinketOperation`（饰品物品栏） | 庄园 `trinkets/items/{key}` | 可写候选并可提交；按原始键销毁，不重排其他条目 |
| `UnequipHeroCampingSkillOperation` | 英雄 `selected_camping_skills` 集合 | 可写候选并可提交；只取消装备，不改变训练解锁 |
| `campaign.hero.add` | 英雄名单及英雄内嵌数据 | 使用程序内置、版本化的 0 级空白 DSON 模板，再按有效职业定义初始化；不得依赖当前存档已有英雄。空 roster 必须可新增。模板及初始化规则通过保存重载和游戏内验证前，不得标记为已验证 |
| `campaign.hero.add_or_replace_quirk` | 英雄怪癖集合 | 新增时初始化记录元数据；正面替换沿用替换机制，负面替换执行删除再按原顺序位置克隆新增 |
| `campaign.trinket.add_inventory` | 庄园饰品库存 | 按有效内容定义追加一个库存条目 |
| `campaign.hero.equip_trinket` | 英雄饰品栏 | 追加装备记录并校验饰品职业限制 |
| `campaign.hero.set_equipment_ranks` | 英雄武器/防具字段及购买节点 | 复合操作同步；按有效升级树处理稀疏节点 |
| `campaign.hero.set_combat_skill_rank` | 战斗技能购买节点 | 按有效 Mod 升级树与技能等级定义校验上限 |
| `campaign.hero.set_camping_skill_learned` | 生存训练购买节点 | 学习状态与已装备技能集合分开处理 |
| `campaign.town.set_upgrade_rank` | 小镇升级购买节点 | 按有效升级树设置进度并回锁后续节点 |
| `campaign.town.set_district_system_open` | 小镇建筑系统状态集合 | 按有效 District 定义开放或清除系统状态 |

`CampaignMappingDescriptor::capability()` 为 UI 提供四级能力：只读、仅 Session、可生成候选、可安全提交。`semantically_writable` 表示实现可以按 Mapping 生成候选；`game_mutation_verified` 表示可以进入安全提交。二者不能互相代替。

`campaign_operation_capabilities()` 是稳定的操作目录，列出 `Available`、`NotImplemented` 和 `Deferred` 三种状态及其 Mapping/原因。购买节点可以按 `tree_id + instance_number + requirement_code` 定位；稀疏存档缺少将购买的节点时，Operation 会克隆同一文档中的模板行并改写行标识。SaveAdapter 再检查映射路径、字段类型、before-value 和 DSON 回读。

### 4.2 暂缓与未验证能力

- 生存技能训练锁定仍为 `Deferred`：不要把它映射为取消装备技能。
- 疾病增删仍为 `Deferred`：等待包含疾病记录的真实存档样本并完成游戏内验证。
- 英雄名称、压力值、折磨 ID 和折磨严重度可在 Session 中生成候选档，目前没有游戏修改证据。编辑器不直接设置任务内美德；设置/清除折磨时只会清空该字段，避免把美德作为营地状态保存。普通安全提交仍会拒绝这些候选映射；`AcceptanceTestCandidate` 只用于写入隔离测试副本，仍执行源副本指纹检查、完整备份和写后回读。
- HP 和伤害等动态计算状态保持只读，不提供写入 Operation。

其他已通过游戏验证的 Stage 11 行为，以及本轮接通的结构编辑和成长编辑，均已进入能力目录并使用同一个 SafeSaveCommitter 流程。本轮追加 3 个测试档：负面怪癖删除再新增、折磨状态与压力 100、名单首位英雄改名。

### 4.3 Operation 和提交接口

- UI 调用 `CampaignEditSession::apply(operation, expected_revision)`，用 revision 避免基于过期界面状态提交。
- `apply()` 的返回值用于展示本次操作；提交时读取 `session.pending_changes()`，它是相对本次 Session 初始模型合并后的净变更。连续修改同一字段会折叠成一条 before/after；Undo 到初始值会从净 ChangeSet 移除。
- `SaveAdapter::build_candidate(profile, pending_changes)` 只接受 Registry 中有候选写入能力的映射。结构删除会在深拷贝后的 DSON AST 上执行，并在重新编码、解码后检查除了目标子树外的字段值和未知原始字段没有变化。
- 集合编辑由带 operation ID、语义 Mapping、DSON 路径和类型的 `CampaignDocumentMutation` 表示；Operation 校验器和 SaveAdapter 都检查映射作用域与允许的变更类型。UI 不应自行拼接路径或直接构造这些变更，应由 Application 层命令/工厂根据稳定 ID 和当前模型生成 Operation。
- `SafeSaveCommitter::commit()` 默认只接受带游戏内修改证据的映射；先检查源和目标副本未漂移，再生成并验证候选、备份整个目标 profile、原子写入目标副本并回读验证。`AcceptanceTestCandidate` 是测试生成器专用显式模式，仅允许候选标量映射写入隔离副本，不会解锁普通 UI 提交。源 profile 不会成为写入目标。
- 结构变更的 Undo/Redo 作用于内存模型；Undo 后请从 `pending_changes()` 取得净 ChangeSet。不要把 Undo 的单步反向事件直接交给 SaveAdapter。

测试用例本身也应保持语义明确：

- `Upgrade` 与 `Downgrade` 设置目标等级，并确保购买节点连续、后续节点状态正确。
- `Remove` 表示销毁或从语义集合删除；若要保留到库存，应另设 `Unequip`/`MoveToInventory`。
- 内容选择器按当前 Mod 顺序和有效覆盖结果展示内容，并可查看来源；持久化仍使用稳定内容 ID。
- 长列表按 roster position 和稳定 Hero ID 定位；不要把名单位置作为唯一实体主键。
- 对不支持或未验证的操作禁用提交，并说明暂不可用原因；不要让 UI 静默退化为另一个相近操作。

## 5. 建议的后续集成顺序

Stage 12 已完成写回纵切片。后续建议转向 UI/Application 命令服务和 Stage 13 总体资格测试：

1. 为结构型操作提供正式 Application 工厂，使 UI 只传英雄 ID、内容 ID 和目标值，不接触 DSON 路径。
2. 使用 Stage 12 测试档逐项完成游戏内回读验收并记录结果。
3. 覆盖原版、DLC、缺失 Mod、大 roster、外部修改和备份恢复等 Stage 13 资格场景。
4. 生存技能训练锁定和疾病编辑继续保持关闭，直到取得合适样本并完成单独验证。

每项都先完成 Operation、Mapping、验证、Save Adapter 和自动回归，再接 UI；UI 负责展示语义、调用 Application 服务和呈现验证结果，不承载存档规则。

## 6. 相关资料

- [后端技术设计与实施指南](<Darkest Dungeon 1 Sandbox Save Editor — Backend Technical Design and Implementation Guide.md>)：Operation、Mapping、Save Adapter、安全提交和 Stage 12 功能纵切片。
- [v1.0 产品与技术设计](Darkest_Dungeon_1_Sandbox_Save_Editor_Development_Design_v1.0.md)：需求、领域语义与 UI 交互基线；技术栈部分以现行后端指南为准。
- [Stage 11 已验证档及逐项记录](../test_save_profile/stage11_advanced_game_tests/README.md)。
- [Stage 12 Operation 写回档与验收清单](../test_save_profile/stage12_operation_tests/README.md)：原有 17 个独立测试档和 1 个对照档。
- [Stage 12 补充验收清单](../test_save_profile/stage12_followup_tests/README.md)：负面怪癖删除再新增、压力状态和英雄改名各一份独立测试档。
