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
  getCampaign: () => request("/api/campaign"),
  listHeroClasses: () => request("/api/campaign/hero-classes"),
  editHeroes: (action, revision, options = {}) => request("/api/campaign/hero", "POST", { action, revision, ...options }),
  listTrinkets: (search = "", mod = "", heroClass = "") => request(`/api/campaign/trinkets?search=${encodeURIComponent(search)}&mod=${encodeURIComponent(mod)}&class=${encodeURIComponent(heroClass)}`),
  editTrinkets: (action, revision, options = {}) => request("/api/campaign/trinket", "POST", { action, revision, ...options }),
  reloadCampaign: () => request("/api/campaign/reload", "POST", {}),
  setCampaignResource: (index, amount, revision) => request("/api/campaign/resource", "POST", { index, amount, revision }),
  setTownBuildingRank: (buildingId, treeId, rank, revision) => request("/api/campaign/building-rank", "POST", { buildingId, treeId, rank, revision }),
  maximizeTownBuildings: (revision, buildingId = undefined) => request("/api/campaign/building-max", "POST", { revision, ...(buildingId === undefined ? {} : { buildingId }) }),
  editDistrict: (action, revision, districtId = undefined, built = undefined) => request("/api/campaign/district", "POST", { action, revision, ...(districtId === undefined ? {} : { districtId }), ...(built === undefined ? {} : { built }) }),
  saveCampaign: () => request("/api/campaign/save", "POST", {}),
  undoCampaign: (revision) => request("/api/campaign/undo", "POST", { revision }),
  redoCampaign: (revision) => request("/api/campaign/redo", "POST", { revision }),
});
