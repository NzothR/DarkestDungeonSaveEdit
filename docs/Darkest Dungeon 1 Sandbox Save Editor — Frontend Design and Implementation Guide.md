# Darkest Dungeon 1 Sandbox Save Editor
## Frontend Design and Implementation Guide / 前端设计与实施指南

**文档状态：** Frontend baseline，供前端实现与后端 Application 接口联调使用  
**更新日期：** 2026-09-21  
**关联后端文档：** [Backend Technical Design and Implementation Guide](Darkest%20Dungeon%201%20Sandbox%20Save%20Editor%20%E2%80%94%20Backend%20Technical%20Design%20and%20Implementation%20Guide.md)  
**产品需求基线：** [v1.0 Product and Technical Design](Darkest_Dungeon_1_Sandbox_Save_Editor_Development_Design_v1.0.md)  
**实测语义参考：** [Core Editing Game-Test Results and UI Integration Reference](核心编辑功能实测与UI集成参考.md)

---

# 1. 文档定位

本文规定前端的产品范围、界面流程、交互语义、状态管理和与 C++ Application Service 的通信边界。界面以原版 Darkest Dungeon 小镇为视觉基线，并提供适合存档编辑的直接操作入口。

本指南根据 v1.0 需求、当前后端设计以及玩家确认的游戏内测试结果整理。若旧设计中的操作规则与本文冲突，以本文记录的最新决定为准；后端内部实现细节和安全写回规则仍以现行后端指南为准。

前端是语义操作的发起方和结果的呈现方，不解析 DSON、不查询 SQLite、不直接改写存档文件，也不复制后端业务规则。后端是否实现并允许提交某项操作，由 Application 返回的能力信息决定。

---

# 2. 产品目标与范围

## 2.1 目标

- 提供原版小镇氛围和主要视觉布局，让用户能按游戏中的概念找到资源、建筑、英雄和饰品。
- 让玩家直接操作存档中的资源和已验证英雄属性，并清楚看到当前值、编辑结果、Mod 来源和校验反馈。
- 编辑立即作用于当前内存 Session；用户可随时撤销或重做，只有显式保存才写入目标存档。
- 首版通过 Drogon 启动本地 HTTP 服务，由默认浏览器打开 HTML/CSS/JavaScript 前端。
- 在前端完成主要编辑流程后再进行 Stage 13 综合资格测试。Stage 14 原定的后端/UI 接洽并入前端开发过程。

## 2.2 首版范围

- 初始化/环境检查界面、设置界面、小镇编辑界面。
- 存档 Profile 发现、选择、打开与 Session 状态显示。
- 庄园资源、小镇建筑与升级、英雄名单和已接通的英雄编辑、饰品库存及英雄饰品栏。
- 内容本地化、原版/Mod 资源、工具提示、搜索和来源显示。
- 操作历史、撤销/重做、校验、保存预览、显式安全写回和错误诊断。

首版优先形成可启动、可浏览和可逐步接通编辑操作的界面骨架。建筑、英雄详情、批量饰品等功能按纵切片逐步接入，不要求初始化界面完成时所有操作都已可提交。

## 2.3 非目标

- 前端直接读写 DSON、SQLite 或游戏存档文件。
- 修改游戏安装目录、DLC 或 Mod 原文件；它们仅作为只读内容来源。
- 支持游戏内运行时状态、HP、伤害等动态计算字段写入。
- 首版支持疾病编辑、生存技能训练锁定或未经游戏验证的删除英雄写回。
- 导入 Mod 管理器导出的 Mod 顺序 JSON。

---

# 3. 设计基线与已知功能状态

## 3.1 已由玩家确认通过的编辑行为

下列能力已通过生成存档并在游戏中查看确认，或已经有此前的实测通过记录：

- 一次编辑全部庄园基础资源。
- 新增 Mod 英雄、编辑英雄等级、武器/防具等级及战斗技能等级。
- 学会生存技能；单独取消已学技能的装备状态。它不是“锁定训练技能”。
- 正面怪癖锁定（仅定义允许锁定者）、怪癖移除、怪癖增加和替换。
- 负面怪癖替换必须显示为“删除旧怪癖，再在原位置增加新怪癖”；负面怪癖不带替换标志。
- 英雄折磨/非折磨状态、压力值和英雄名称修改。压力为 0 是清空压力的特例；压力编辑范围为 0–200。
- 饰品库存增加/销毁、英雄装备饰品及销毁已装备饰品。销毁不会转入库存。
- 小镇建筑系统开放状态、单栋建筑建成状态、建筑升级进度。
- 已验证的技能/装备/建筑降级会按有效升级树处理购买节点与后续锁定状态。

这些记录表达游戏结果，不表示每一项都已完成 UI、Application API 或能力目录接入。集成时必须查询当前后端能力目录和 Mapping 状态。

## 3.2 暂缓与待验收

| 能力 | 前端处理 |
| --- | --- |
| 生存技能训练锁定 | 不提供写入入口；不能把取消装备技能伪装成锁定。 |
| 疾病新增/移除 | 暂不提供写入入口，等待含真实疾病的样本与后续游戏验证。 |
| 英雄删除/解雇 | 后端或产品流程未完成游戏内验收；功能验收延后到 UI 完成后。若界面先行接入，应按能力状态显示禁用，不能直接允许正式写回。 |
| HP、伤害等动态计算值 | 只读展示（若后端提供），不显示可编辑控件。 |
| 生存技能取消装备 | 与学习、训练锁定分开展示和命名；能力可用时提供独立操作。 |

用户已确认本轮负面怪癖、压力/折磨和英雄改名的新测试存档通过。阶段 13 的完整组合测试仍推迟到前端集成之后。

---

# 4. 前后端总体架构

```text
Browser
├── HTML / CSS / ES Modules
├── Scene and reusable UI components
├── View Model store + operation history view
└── EditorGateway client
        │ local HTTP + JSON
        ▼
Drogon HTTP Adapter
        │ typed DTO / Application calls
        ▼
Application Service
├── Session / Operation / Undo / Redo
├── Content Query / Validation / Save Preview
└── Safe Save Committer
```

## 4.1 前端职责

- 展示 Application 返回的 DTO、内容定义、能力状态和诊断。
- 将用户意图转换成类型明确的查询或语义操作请求。
- 携带目标实体 ID、Session ID 和当前 revision；例如给英雄装备饰品时必须提交当前英雄 ID。
- 管理临时输入态、弹窗、焦点、拖拽和可视化反馈。
- 展示操作记录和调用后端 Undo/Redo；不自行反向改造存档模型。

## 4.2 后端职责

- 从存档解析 Mod 顺序和启用列表，并构造有效内容环境。
- 提供 Session View Model、内容查询、Capability、Operation、Undo/Redo、Validation、Preview 和 Commit。
- 对目标英雄、饰品职业限制、内容有效性、数据范围和 Session revision 做权威校验。
- 生成候选存档、备份和安全写回；返回结构化结果与诊断。

前端传入的“当前选择英雄”是操作上下文，不是授权。后端仍须核对 Hero ID 存在、内容可解析、操作适用于目标对象。

## 4.3 Drogon 边界

Drogon 同时托管首版静态前端并提供 REST + JSON API。Controller 只负责 HTTP DTO 校验、调用 Application Service、转换 Result/Error 和返回 JSON；不得扫描 Mod、访问 SQLite、拼 DSON 路径或直接实现业务规则。HTTP endpoint 和 DTO 应围绕现有 Application 用例稳定后逐步确定。

服务默认仅绑定 loopback。采用启动期随机令牌或等效本地保护，检查 Origin；写操作携带 Session revision。禁止开放任意路径读文件的接口，也不把内部堆栈和任意本机路径返回给页面。

---

# 5. 启动、初始化与设置流程

## 5.1 启动流程

```text
启动桌面程序
  → 初始化 Application 与 Drogon
  → 分配本地端口及本次启动令牌
  → 确认 HTTP 服务就绪
  → 打开默认浏览器到本地前端
  → 前端查询状态并显示初始化或上次工作区
```

端口、令牌和服务就绪信息通过本机启动流程安全传递，页面不得猜测固定端口或在 URL 中暴露长期秘密。关闭浏览器不应静默提交或丢弃 Session。浏览器重新打开时，若本地服务仍在运行，应重新查询当前 Session；关闭程序时，若存在未保存 Operation，应要求用户继续编辑或明确丢弃。首版不假设 Session 在程序退出后仍可恢复。

## 5.2 初始化界面

初始化页面负责让用户确认/选择：

- Darkest Dungeon 安装目录；
- Steam Workshop Mod 目录；
- 额外本地 Mod 目录（通常为 `modes`，允许玩家配置）；
- 存档根目录；
- UI 语言。

同时初始化独立的存档备份目录。备份目录写入配置文件后由后端创建并检查读写权限，不依赖默认目录推断。
普通备份最大数量默认 20。设置页还提供自动编辑保存开关和间隔，默认开启、默认 30 秒。

自动编辑保存只在 Session 第一次产生编辑后启动。后端将恢复快照写入
`backupRoot/AutoEditSave`，不会直接修改游戏存档；显式保存成功后停止当前 Session 的自动保存并清理恢复点，
下一次编辑时重新启动。第一次编辑建议立即写入一个恢复点，然后按配置间隔继续写入。

应用初始化完成后，如果发现上次未提交的恢复点，页面必须展示来源 Profile、revision、时间和指纹信息，
让用户选择恢复或放弃。恢复前重新打开来源存档并校验指纹；来源存档发生外部变化时进入冲突诊断。

提供路径检查、读取权限检查、游戏/DLC/Mod 资源扫描状态、数据库初始化进度和诊断入口。所有游戏与 Mod 路径只读。长时间扫描显示进度、当前任务、取消状态和可展开诊断。

## 5.3 Mod 顺序

每次打开存档后，使用该存档记录的启用 Mod 和顺序创建 Effective Content Environment。首版不读取、不导入 Mod 管理器导出的 JSON。若存档引用的 Mod 缺失、发生冲突或环境刷新失败，应在继续编辑前显示差异与影响；不能静默改用磁盘扫描顺序。

## 5.4 设置界面

设置项包括路径、语言、窗口/浏览器启动偏好、资产缩放、提示信息密度和诊断日志位置等。路径配置应由用户显式选择；前端不接受任意 HTTP URL 作为本机文件路径。语言改变后刷新展示层文本，不修改存档 ID 或 Mod 排序。

备份目录独立于游戏和存档目录，由后端写入配置并初始化。普通备份最大数量默认 20；自动编辑保存默认开启，
间隔默认 30 秒。自动编辑保存只在 Session 第一次产生编辑后启动，第一次编辑建议立即写入一个恢复点，
之后按配置间隔继续写入。显式保存成功后停止当前 Session 的自动保存并清理恢复点，下一次编辑时重新启动。

## 5.5 初始化与恢复生命周期

```text
PROCESS_STARTING
  → HTTP_READY
  → CONFIGURATION_READY
  → BACKUP_STORAGE_READY
  → RECOVERY_CHECKING
  → PROFILE_DISCOVERING
  → PROFILE_OPENING
  → EFFECTIVE_ENVIRONMENT_RESOLVING
  → SESSION_READY
  → EDITING_DIRTY（启动自动编辑保存）
  → COMMITTING（成功后清理当前恢复点）
  → EDITING_CLEAN
```

配置、备份目录或来源存档检查失败时停留在对应阶段并显示诊断。恢复点恢复的是后端 Session 快照，
不是对游戏存档的直接写回；恢复成功后仍需要经过正常的校验、预览和显式保存流程。

---

# 6. 信息架构与界面

## 6.1 主界面

```text
Application
├── Initialization / Environment Setup
├── Settings
└── Town Editor
    ├── Resource Bar
    ├── Building Hotspots / Building Upgrade View
    ├── Hero Roster / Hero Detail View
    ├── Trinket Chest / Trinket Inventory Controls
    ├── Operation History / Undo / Redo
    ├── Validation / Diagnostics
    └── Save / Preview / Backup Controls
```

首屏以原版小镇 UI 为视觉基线，不做表格式后台管理页。可以增加侧栏、浮层和编辑反馈，以便呈现编辑器特有的操作、来源、验证和撤销状态。

## 6.2 资源栏

显示基础资源的本地化名称、图标和当前值。点击数值直接进入数值编辑态；输入并确认后立即调用后端操作，Esc 取消本次未确认输入。基础资源不使用左右键微调。

编辑前后值、超出范围错误和后端拒绝原因必须就地显示。一次性“基础资源修改”可以作为复合操作，便于一次撤销。

## 6.3 建筑与升级

小镇画面上的建筑热点沿用原版布局。点击建筑直接打开该建筑的升级界面，而不是进入游戏内使用界面。升级节点、当前级别、依赖关系、锁定状态、来源和可编辑状态都由有效内容环境及后端 View Model 提供。

升级控件沿用 v1.0 交互：左键 +1、右键 -1、Shift+左键最大、Shift+右键最小；同时提供键盘和明确的可访问按钮。右键语义只对配置了升级档案的控件生效，不全局套用。具体最大等级来自当前有效升级树，不硬编码原版上限。

“小镇建筑系统开放”“单栋建筑建成/锁定”“建筑升级节点进度”是不同状态，必须分别呈现，不用一个总开关代替。

## 6.4 英雄名单与详情

点击英雄卡片打开详情界面或侧栏，至少展示原版肖像/职业、名单位置、等级、名称、武器/防具、战斗技能、生存技能、怪癖、饰品和已支持的状态。稳定 Hero ID 用于请求和重排后定位；名单位置只用于显示。

英雄等级、装备和技能使用旧设计里的升级交互档案；每类控件应有自己的 InteractionProfile。升级、降级、最大化和最小化都由一个明确的语义命令执行，不在页面里改多个底层节点。

技能 UI 分开表示：

- 战斗技能等级；
- 生存技能是否已学会；
- 生存技能是否装备；
- 生存技能训练锁定（当前暂缓，不开放）。

技能等级上限从有效 Mod 定义读取。不得假定所有英雄/技能都以 5 级为上限。

## 6.5 英雄压力与状态

压力编辑使用 0–200 滑块，同时显示可精确输入的数值框。数值输入和滑块操作共享同一临时值；确认后调用 `SetHeroStress` 类语义操作。压力 0 对应清空压力，不另造互相矛盾的状态。

状态入口仅呈现已经确认的“折磨/非折磨”编辑语义。任务内美德不作为可持久化的营地状态提供编辑。HP、伤害等动态计算值不得显示为可编辑输入框。

## 6.6 饰品库存

饰品箱采用原版库存外观，并允许额外编辑工具：

- 搜索与筛选饰品；
- 按稀有度、职业、来源 Mod 批量添加符合条件的饰品；默认添加所有匹配饰品；
- 提供“仅添加库存中不存在的饰品”复选框；
- 按相同筛选条件清空部分库存，或清空全部库存；
- 清空前显示匹配数量和确认框。全部清空与筛选清空都必须明确区分。

批量添加/清空应作为明确的后端复合 Operation 请求。前端先展示筛选条件与预计影响数量，再提交；后端返回实际添加、跳过、删除数量及无法处理的条目。只添加不存在项的比较规则以稳定 Trinket ID 为准，并由后端定义。

## 6.7 英雄饰品栏

给英雄添加饰品时，操作请求必须包括目标 Hero ID 和 Trinket ID。前端可根据后端提供的职业限制筛除不适用项或标为不可用；后端必须再次验证。已有饰品可以左键拖动调整位置；右键删除/销毁该饰品，不转移至库存。拖动和删除均应有立即的操作结果和撤销入口。

## 6.8 删除英雄

删除英雄是高影响操作，沿用原版卡片入口但必须弹出确认，显示英雄姓名、职业和名单位置。该操作当前尚未完成游戏内测试；在后端能力标记可安全提交之前，前端禁用正式提交入口。UI 完成后与整体验收一起生成隔离测试档并验证。

---

# 7. 本地化、内容选择与工具提示

## 7.1 本地化

内容名称和描述根据用户选择的语言查询有效内容环境。建议回退顺序为：用户语言 → 英文 → 本地化键 → Raw ID。UI 自身文案与游戏内容本地化分别管理。

## 7.2 Hover / Focus 浮层

鼠标移动到可说明的 UI 元素时显示原版风格的提示浮层；键盘焦点也能显示同样信息。饰品、技能、怪癖和升级节点浮层展示：

1. 本地化名称；
2. 按玩家语言显示的效果/说明；
3. 需要时显示职业、稀有度、等级要求或状态说明；
4. 最后一行单独显示来源 Mod 名称。原版内容显示“原版”。

来源 Mod 是独立的最后一行，不能混在效果描述里。缺失资产时显示占位图与 Raw ID，不导致页面组件失败。长文本、贴边位置和遮挡在实现阶段逐步调整。

## 7.3 内容选择器

统一选择器支持名称、Raw ID、本地化键、描述、来源 Mod 和标签搜索，并支持稀有度、职业、Mod 等过滤。列表项展示图标、本地化名称、简短描述和来源。最终请求始终传稳定内容 ID，不传显示名称。

---

# 8. Operation、Undo、Redo 与批量编辑

## 8.1 编辑操作立即可撤销

每次编辑操作成功应用到当前 Session 后，立即可撤销；无需先保存。编辑只改变后端 Session 工作模型，不直接触碰磁盘。Undo/Redo 由后端 Session 执行，前端呈现结果和历史。

Undo 单位按**一次用户意图/一次成功 Operation**定义，不按底层字段数、DSON 节点数或鼠标事件数定义。一个复合或批量操作（例如批量升级多名英雄、批量添加饰品、按筛选清空饰品）作为一个可撤销项；一次拖放重排是一个可撤销项；一次确认的资源/压力输入也是一个可撤销项。

滑块拖动期间使用本地临时值，完成拖动或确认输入后提交一次 Operation，避免每个指针采样点都进入历史。输入取消或后端校验失败不新增历史项。新操作成功后清空 Redo 栈。

## 8.2 批量操作

前端可以把多项编辑组织成单一复合操作，但必须明确展示目标对象、操作范围和总影响。例如批量升级英雄应显示目标英雄列表及目标等级/规则。Application 对批量命令应保证整体成功或整体失败；如业务允许部分成功，必须以逐项结果明确告知，且 Undo 能精确恢复成功部分。

## 8.3 操作历史呈现

操作历史显示人类可读的动作、目标、执行时间/顺序和结果，例如“英雄名称：A → B”“将 4 名英雄武器升至 5 级”“按 Mod X 销毁 12 件饰品”。不要向普通用户显示 DSON 路径。撤销失败时保留现状和记录，提示重新加载/刷新原因，不得假装成功。

## 8.4 保存后的历史

Commit 成功后后端把当前结果设为新 baseline。第一版可清空旧 Undo/Redo 历史或以新历史段继续；不能让普通 Undo 看起来能撤回已经写入磁盘的存档。保存后恢复旧状态应通过备份恢复流程。

---

# 9. Session、View Model 与界面状态

前端仅保留展示状态和未确认的表单状态。权威 Campaign 模型、revision、Operation Log、Undo/Redo 栈及 dirty documents 均属于后端 Session。

所有写请求携带：

```text
sessionId
expectedRevision
operation kind / typed payload
stable target IDs (for example heroId and trinketId)
```

成功返回新的 revision、受影响 View Model、Operation 摘要和能力/校验变化。revision 过期时不自动重放：提示数据已变化，刷新相关视图后由用户重试。失败操作不进入后端历史，不应让前端显示已成功。

前端可乐观展示压力滑块拖动或输入中的临时值；Operation 被接受前必须明确区分临时编辑和 Session 已接受值。破坏性操作、批量操作和错误恢复不得依赖不确定的乐观回滚。

---

# 10. 保存、预览与恢复

## 10.1 保存前流程

```text
Edit in Session
  → Validate Session
  → Show semantic preview and warnings
  → Confirm target profile and destination
  → Backend creates complete backup
  → Safe commit / write / read-back validation
  → Show receipt, backup ID and new baseline
```

资源、英雄、建筑等编辑按钮绝不等同于 Save。保存界面应展示目标 Profile、受影响文档、变更摘要、错误和警告、备份位置/ID以及当前是否存在未解决诊断。

Commit 只能调用 Application 的 Preview/Commit 用例。UI 不拼路径、不写临时文件、不自行创建备份。安全提交失败或部分提交时，展示后端诊断和恢复入口，不隐藏错误，也不将 Session 标为已保存。

## 10.2 离开与关闭

切换 Profile 或关闭程序时检测未保存 Operation。提供取消离开、丢弃 Session 或先打开保存预览的明确选择。浏览器刷新只重新查询服务端 Session；自动关闭和程序退出均不能隐式提交或静默丢弃。

## 10.3 备份恢复

恢复前显示备份时间、Profile 和来源信息并二次确认。恢复调用后端 RestoreBackup；完成后刷新 Session/Profile 信息并显示 read-back 结果。

---

# 11. Application Gateway 与 DTO 草案

此节定义 UI 调用能力的概念边界，不冻结最终 URL。HTTP DTO 由 Application Service 用例和真实 UI 流程共同校准。

## 11.1 查询类用例

```text
GetApplicationStatus
GetConfiguration / SaveConfiguration
InspectInstallation
InitializeOrRefreshEnvironment
ListSaveProfiles
OpenSave
GetTownView
GetHeroView(heroId)
SearchContent(kind, query, filters, page)
GetOperationCapabilities
GetOperationHistory
ValidateSession
PreviewCommit
ListBackups
```

Environment refresh 必须使用当前打开存档中的 Mod 顺序。它不接受 Mod Manager JSON 导入。

## 11.2 命令类用例

```text
ApplyOperation(sessionId, expectedRevision, typedOperation)
ApplyCompositeOperation(sessionId, expectedRevision, typedOperations)
Undo(sessionId, expectedRevision)
Redo(sessionId, expectedRevision)
CommitSave(sessionId, expectedRevision, explicitTarget)
RestoreBackup(backupId, explicitTarget)
CloseSession(sessionId, disposition)
```

示例语义 payload：

```json
{
  "kind": "EquipHeroTrinket",
  "heroId": "stable-hero-id",
  "trinketId": "effective-content-id",
  "expectedRevision": 42
}
```

该示例只说明语义要求，不规定最终 JSON 字段命名。前端不得提交原始 AST 路径、SQL、任意文件路径或直接可执行脚本。

## 11.3 Result / Error / Progress

Result 至少区分成功、业务校验失败、能力未实现/暂缓、revision 冲突、文件/环境错误和安全写回错误。UI 根据稳定错误码与结构化上下文呈现，不解析异常文本猜测流程。

初始化、内容扫描和校验等长任务可返回 Job ID、进度、可取消状态及诊断摘要。页面切换不应丢失 Job 状态。

---

# 12. 视觉资源、布局和可访问性

- 原版游戏资产和 Mod 资产由后端内容库/AssetResolver 提供引用，运行时只读加载；不随编辑器打包游戏文件。
- 使用原版背景、图标、肖像和 UI 元素构造小镇场景；缺失时显示稳定占位图与 Raw ID。
- 采用 CSS 变量/主题层控制缩放、对比度、提示层级和侧栏宽度，避免把屏幕坐标写死在业务组件中。
- 原版视觉优先，但关键数值、禁用原因、撤销、确认和保存反馈必须可读。
- 左右键组合操作同时提供键盘替代和可见提示；可交互热点应有焦点状态、工具提示和明确命中区域。
- 浮层要能避开窗口边缘、鼠标和其他关键数值；长描述可以滚动，不遮挡操作按钮。

---

# 13. 错误、诊断与用户反馈

界面需要区分并解释：

- 游戏/存档路径不可用；
- Mod 缺失、顺序不一致或内容定义无法解析；
- 当前对象/内容部分解析或只读；
- Operation 不受支持、暂缓或超出内容定义范围；
- Session revision 过期；
- 校验失败、候选生成失败、备份失败、写入失败、写后回读失败和部分提交。

每条用户消息应说明对象、原因和可采取的动作。技术诊断可展开查看，但默认不把堆栈、绝对本机路径或原始 AST 暴露给普通玩家。Operation 成功、撤销/重做、校验和提交都要有清楚的状态反馈。

---

# 14. 性能与前端实现建议

- 首版使用 HTML、CSS 和原生 JavaScript ES Modules；按 scene、component、gateway、store 分模块组织。若复杂度证明需要，再引入 UI 框架。
- 大型内容列表使用分页或虚拟列表；搜索 debounce，优先调用后端索引；前端只缓存当前视图所需数据。
- 图片按需加载，限制解码尺寸并清理不再使用的 Blob/Object URL。
- 初始化和扫描由后端 Job 执行；前端显示进度，不在 UI 线程做大规模文件扫描。
- UI 语言、内容语言、来源 Mod 与 Raw ID 分开管理，避免内容更新导致界面状态错乱。

---

# 15. 前端实施阶段

## F0 — Gateway Contract 与 Browser Bootstrap

- 确认 Application 查询/命令 DTO、Result/Error、Capability 和 revision 约定。
- 接入 Drogon 静态服务与最小状态 API；实现本地启动、就绪检查和浏览器打开。
- 建立 ES Module 目录、统一样式变量和本机服务错误页。

## F1 — 初始化、设置与 Profile 选择

- 配置安装目录、Workshop Mod、额外本地 Mod、存档目录和语言。
- 配置并初始化独立备份目录、普通备份数量上限和自动编辑保存策略。
- 启动时检查 `AutoEditSave` 恢复点，并提供恢复/放弃入口。
- 显示只读扫描、数据库初始化、Mod 环境和 Profile 发现进度。
- 打开存档后确认 Mod 顺序来自存档，并能查看环境诊断。

## F2 — Town Shell 与基础导航

- 构建原版小镇视觉、资源栏、建筑热点、英雄名单、饰品箱和保存状态栏。
- 先完成导航、状态、占位内容、工具提示及侧栏布局，再逐项接入编辑命令。
- 资源数值可直接输入；点击建筑打开对应升级界面。

## F3 — Hero / Building / Trinket 编辑纵切片

- 按后端 Capability 接通已验证英雄、技能、装备、怪癖、状态、建筑升级和饰品操作。
- 实现内容搜索、本地化、来源 Mod、英雄饰品适配提示和拖动换位。
- 接通按条件批量加/清饰品、确认框和影响摘要；接通批量编辑命令时保证可撤销。

## F4 — Operation History 与安全保存

- 实现即时 Undo/Redo、批量操作历史、Session dirty 状态、校验、预览、显式 Commit、备份恢复和失败诊断。
- 确认修改操作本身不写磁盘，只有 Commit 写入明确目标。

## F5 — 综合集成与资格测试

- UI 核心流程稳定后开展 Stage 13 综合测试，覆盖原版/DLC/Mod 环境、缺失 Mod、外部修改、长名单、批量操作、Undo/Redo、备份和恢复。
- 删除英雄与清空饰品等高影响操作纳入 UI 确认流程验收；英雄删除须单独用隔离存档完成游戏内测试。
- 生存技能锁定和疾病编辑仍按暂缓策略，不因 UI 完成而自动开放。

Stage 14 的 UI 接洽不再作为前端开发前的独立阻塞阶段；Gateway 合约、Drogon Adapter 和页面联调归入 F0–F5。Stage 13 综合测试安排在这些 UI 集成工作之后。

---

# 16. 前端 Definition of Done

- 程序能启动本地 Drogon 服务并打开浏览器；服务仅暴露本机编辑所需能力。
- 用户能完成路径设置、环境检查、Profile 选择和打开存档；Mod 顺序只由存档提供。
- 小镇主界面保持原版风格，并能渐进接入资源、建筑、英雄和饰品编辑。
- 每次编辑成功后可立即 Undo；Redo、批量操作、拖动和破坏性操作的历史语义明确。
- 英雄特定饰品操作传递正确 Hero ID，后端校验不匹配条件；禁用能力有明确原因。
- Tooltip 显示用户语言效果，来源 Mod 在独立末行；缺失图片不会导致界面崩溃。
- 页面不直接访问 SQLite、DSON、存档路径或安全写回文件。
- 保存前可查看校验与变更摘要，明确确认目标；保存失败能展示恢复/诊断路径。
- 删除英雄、生存技能训练锁定、疾病等暂缓能力不会被前端暗中转换成相似操作。
- 前端集成后完成 Stage 13 综合资格测试并记录真实游戏验收结果。

---

# 17. 讨论中可后续优化的细节

以下内容不阻止第一版结构实现，可在具体页面落地时再确定：

- 侧栏、浮层、建筑升级面板在不同分辨率下的布局和切换方式；
- 批量饰品操作的大结果集分页、预估数量和极端数量保护；
- 操作历史的筛选、分组与批量操作细节展开；
- 编辑时是否支持更改目标存档副本、自动恢复未保存 Session 和多 Profile 并行；
- 资产缩放、字体、主题对比度与输入法体验。

这些优化不得改变“操作进入内存 Session、后端校验、明确保存写回”的核心流程。

---

# 18. 相关文档

- [Backend Technical Design and Implementation Guide](Darkest%20Dungeon%201%20Sandbox%20Save%20Editor%20%E2%80%94%20Backend%20Technical%20Design%20and%20Implementation%20Guide.md)
- [v1.0 Product and Technical Design](Darkest_Dungeon_1_Sandbox_Save_Editor_Development_Design_v1.0.md)
- [Core Editing Game-Test Results and UI Integration Reference](核心编辑功能实测与UI集成参考.md)
- [Stage 11 Advanced Game-Test Save Report](../test_save_profile/stage11_advanced_game_tests/README.md)
- [Stage 12 Operation Save Acceptance Notes](../test_save_profile/stage12_operation_tests/README.md)
- [Stage 12 Follow-up Save Acceptance Notes](../test_save_profile/stage12_followup_tests/README.md)
