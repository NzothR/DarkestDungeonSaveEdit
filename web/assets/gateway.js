const API_REQUEST_HEADER = "X-DDSE-Request";

export class GatewayError extends Error {
  constructor(code, message, status) {
    super(message);
    this.name = "GatewayError";
    this.code = code;
    this.status = status;
  }
}

async function request(path) {
  let response;
  try {
    response = await fetch(path, {
      method: "GET",
      credentials: "same-origin",
      cache: "no-store",
      headers: { [API_REQUEST_HEADER]: "1" },
    });
  } catch {
    throw new GatewayError("LOCAL_SERVICE_UNAVAILABLE", "无法连接本机编辑服务。", 0);
  }

  let payload;
  try {
    payload = await response.json();
  } catch {
    throw new GatewayError("INVALID_GATEWAY_RESPONSE", "本机服务返回了无法识别的响应。", response.status);
  }

  if (!response.ok || payload.ok !== true) {
    const error = payload.error ?? {};
    throw new GatewayError(
      error.code ?? "GATEWAY_REQUEST_FAILED",
      error.message ?? "本机服务请求失败。",
      response.status,
    );
  }
  return payload.value;
}

export const editorGateway = Object.freeze({
  getApplicationStatus: () => request("/api/status"),
});
