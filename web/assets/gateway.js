const API_REQUEST_HEADER = "X-DDSE-Request";

export class GatewayError extends Error {
  constructor(code, message, status) {
    super(message);
    this.name = "GatewayError";
    this.code = code;
    this.status = status;
  }
}

async function request(path, method = "GET", body = undefined) {
  let response;
  try {
    response = await fetch(path, {
      method,
      credentials: "same-origin",
      cache: "no-store",
      headers: {
        [API_REQUEST_HEADER]: "1",
        ...(body === undefined ? {} : { "Content-Type": "application/json" }),
      },
      ...(body === undefined ? {} : { body: JSON.stringify(body) }),
    });
  } catch {
    throw new GatewayError("LOCAL_SERVICE_UNAVAILABLE", "Local service unavailable.", 0);
  }

  let payload;
  try {
    payload = await response.json();
  } catch {
    throw new GatewayError("INVALID_GATEWAY_RESPONSE", "Invalid response from local service.", response.status);
  }

  if (!response.ok || payload.ok !== true) {
    const error = payload.error ?? {};
    throw new GatewayError(
      error.code ?? "GATEWAY_REQUEST_FAILED",
      error.message ?? "Local service request failed.",
      response.status,
    );
  }
  return payload.value;
}

export const editorGateway = Object.freeze({
  getApplicationStatus: () => request("/api/status"),
  getConfiguration: () => request("/api/configuration"),
  saveConfiguration: (configuration) => request("/api/configuration", "PUT", configuration),
  getRecoveryStatus: () => request("/api/recovery"),
  listProfiles: () => request("/api/profiles"),
  getInitialization: () => request("/api/initialization"),
  startInitialization: (force = false) => request("/api/initialization", "POST", { force }),
  listDatabaseMods: () => request("/api/database/mods"),
  selectDirectory: (kind) => request("/api/select-directory", "POST", { kind }),
  restoreRecovery: () => request("/api/recovery/restore", "POST", {}),
  discardRecovery: () => request("/api/recovery", "POST", {}),
});
