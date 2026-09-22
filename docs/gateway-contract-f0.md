# F0 Gateway 合约

当前合约覆盖本地服务就绪检查、F1 配置初始化与自动编辑恢复点查询。存档、环境与编辑用例将在后续 F 阶段按 Application Service 逐步加入。

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

请求体为同一组字段的 JSON 对象。后端负责范围校验、写入配置文件并创建备份目录；页面不得自行创建目录。

### `GET /api/recovery`

返回 `available`、来源 Profile、来源指纹、Session revision 和最后保存时间。恢复点位于
`backupRoot/AutoEditSave/recovery.json`，不会覆盖游戏存档。

### `POST /api/recovery/restore`

读取有效恢复点并返回恢复元数据与 Session 快照，供后续 OpenSave/Session 流程载入。恢复前必须校验来源存档指纹。

### `POST /api/recovery`

放弃当前恢复点并写入作废标记。显式 Commit 成功后由 Application Service 执行同样的清理动作。

### `GET /api/profiles`

按配置中的 `saveRoots` 发现 `profile_*` 目录，返回 Profile 标识、路径、读取状态、文档数量、指纹和诊断。
前端只显示这些 DTO，不自行遍历存档目录。

## 浏览器 Gateway

`web/assets/gateway.js` 是唯一页面 API 入口。调用方接收状态 DTO 或 `GatewayError`，不直接使用 `fetch`，也不访问服务端实现细节。
