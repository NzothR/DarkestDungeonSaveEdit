# F0 Gateway 合约

当前合约覆盖本地服务就绪检查和浏览器启动。存档、环境与编辑用例将在后续 F 阶段按 Application Service 逐步加入；当前不提供写入接口。

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
    "state": "ready"
  },
  "diagnostics": []
}
```

失败响应使用 `{ "ok": false, "error": { "code", "message", "context" }, "diagnostics": [] }`；HTTP 状态码表达传输结果，稳定 `code` 表达应用错误。

## 浏览器 Gateway

`web/assets/gateway.js` 是唯一页面 API 入口。调用方接收状态 DTO 或 `GatewayError`，不直接使用 `fetch`，也不访问服务端实现细节。
