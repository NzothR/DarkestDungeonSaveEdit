# F0 Gateway 合约

当前合约覆盖本地服务就绪检查、F1 配置初始化、数据库初始化进度、数据库 Mod 查询与自动编辑恢复点查询。

## 本地服务

- 服务只绑定 `127.0.0.1`，默认由系统分配端口。
- HTTP 服务就绪后才打开浏览器；也可通过 `--no-browser` 关闭自动打开。
- 页面由固定的 `web` 目录提供，不提供任意文件读取路由。
- 首页设置本次进程随机生成的 HttpOnly、SameSite=Strict 会话 Cookie。API 同时校验 Host、可用时的 Origin、自定义请求头和 Cookie。
- 页面不得把本机启动令牌或服务端口写入 URL。服务地址只在启动输出中显示。

## `GET /api/status`

请求使用同源 Cookie，并带 `X-DDSE-Request: 1`。成功响应采用统一 Result 包装：

```json
{
  "ok": true,
  "value": {
    "apiVersion": 1,
    "application": {
      "name": "Darkest Dungeon Save Editor",
      "version": "0.1.0"
    },
    "state": "ready",
    "configuration": {
      "state": "ready",
      "backupRoot": "D:/.../ddse-data/backups",
      "autoEditSaveEnabled": true,
      "autoEditSaveIntervalSeconds": 30,
      "recoveryAvailable": false
    }
  },
  "diagnostics": []
}
```

失败响应使用 `{ "ok": false, "error": { "code", "message", "context" }, "diagnostics": [] }`；HTTP 状态码表达传输结果，稳定 `code` 表达应用错误。

## F1 配置与备份

应用首次启动时在 `ddse-data/config.json` 写入配置，并初始化配置中的 `backupRoot` 与
`backupRoot/AutoEditSave`。普通备份默认保留 20 份；自动编辑保存默认开启，间隔为 30 秒。
配置写入使用原子替换，目录不可写时返回稳定错误码。

### `GET /api/configuration`

返回 `gameRoot`、`workshopRoots`、`localModRoots`、`saveRoots`、`backupRoot`、`dataRoot`、
`language`、`maxBackupCount`、`autoEditSaveEnabled` 和 `autoEditSaveIntervalSeconds`。

### `PUT /api/configuration`

请求体为同一组字段的 JSON 对象。`gameRoot` 和 `backupRoot` 必须填写；存档 Profile、Workshop Mod 和额外本地 Mod 路径可以留空。
后端负责范围校验、写入配置文件并创建备份目录；页面不得自行创建目录。所有目录配置各只有一个路径。

### `POST /api/select-directory`

请求体为 `{ "kind": "game|save|workshop|localMod|backup" }`。Windows 下打开原生目录选择器，
返回用户选择的绝对路径；`save` 选择器接收单个 `profile_*` 目录并立即验证存档结构，验证失败不会返回可写路径。
取消选择返回 `DIRECTORY_PICKER_CANCELLED`。前端不依赖浏览器的文件系统路径模拟能力。

### `GET /api/recovery`

返回 `available`、来源 Profile、来源指纹、Session revision 和最后保存时间。恢复点位于
`backupRoot/AutoEditSave/recovery.json`，不会覆盖游戏存档。

### `POST /api/recovery/restore`

读取有效恢复点并返回恢复元数据与 Session 快照，供后续 OpenSave/Session 流程载入。恢复前必须校验来源存档指纹。

### `POST /api/recovery`

放弃当前恢复点并写入作废标记。显式 Commit 成功后由 Application Service 执行同样的清理动作。

### `GET /api/initialization` / `POST /api/initialization`

读取或启动数据库初始化任务。返回 `status`、`phase`、`progressPercent`、`currentWork`、
`baseElapsedMs`、`modElapsedMs`、`totalElapsedMs`、启用 Mod 数量和诊断列表。完整配置启动服务后会自动开始，
保存配置也会触发任务；前端使用轮询，不需要 WebSocket。

原版数据库存在且结构有效时复用，不重复扫描；Mod 环境数据库每次初始化都会按当前配置更新。
缺失 Mod 或单个 Mod 内容错误会写入诊断，其他 Mod 仍会提交到数据库。

### `GET /api/database/mods`

从 `mod_environment.db` 查询存档中启用的 Mod，按存档顺序返回。名称来自数据库的 `display_name`，
无法匹配时使用存档保存的 key/name。初始化完成前返回 `DATABASE_NOT_READY`。

### `GET /api/database/mod-cover?modId=...`

根据数据库记录的 Mod 根目录读取固定文件 `preview_icon.png`（同时兼容已有的 `preview.*`、`cover.*` 和
`mod_preview.*` 文件名），图标路径不写入数据库，仅用于列表展示；不存在封面时返回 404。

### `GET /api/game-asset?path=...`

从已配置游戏目录读取只读美术资源。路径必须是游戏目录内的相对路径，禁止绝对路径和 `..` 穿越；首版小镇界面使用该接口读取背景和建筑图片，资源不存在时由前端显示占位图。

小镇背景优先请求 `fx/town_ground/town_ground.sprite.png`。`GET /api/town-asset?role=background` 会在原版资源数据库中按索引查找同一背景，作为数据库存在但文件布局需要兼容时的回退接口。

## 浏览器 Gateway

`web/assets/gateway.js` 是唯一页面 API 入口。调用方接收状态 DTO 或 `GatewayError`，不直接使用 `fetch`，也不访问服务端实现细节。
