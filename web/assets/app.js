import { editorGateway, GatewayError } from "./gateway.js";

const elements = {
  indicator: document.querySelector("#connection-indicator"),
  title: document.querySelector("#connection-title"),
  message: document.querySelector("#connection-message"),
  details: document.querySelector("#service-details"),
  state: document.querySelector("#service-state"),
  appVersion: document.querySelector("#app-version"),
  apiVersion: document.querySelector("#api-version"),
  error: document.querySelector("#connection-error"),
  panel: document.querySelector("#f1-panel"),
  configurationState: document.querySelector("#configuration-state"),
  form: document.querySelector("#configuration-form"),
  configurationSubmit: document.querySelector("#configuration-submit"),
  gameRoot: document.querySelector("#game-root"),
  saveRoots: document.querySelector("#save-roots"),
  workshopRoots: document.querySelector("#workshop-roots"),
  localModRoots: document.querySelector("#local-mod-roots"),
  backupRoot: document.querySelector("#backup-root"),
  maxBackupCount: document.querySelector("#max-backup-count"),
  language: document.querySelector("#language"),
  autoEditSaveEnabled: document.querySelector("#auto-edit-save-enabled"),
  autoEditSaveInterval: document.querySelector("#auto-edit-save-interval"),
  configurationMessage: document.querySelector("#configuration-message"),
  recoveryCard: document.querySelector("#recovery-card"),
  recoveryMessage: document.querySelector("#recovery-message"),
  restoreRecovery: document.querySelector("#restore-recovery"),
  discardRecovery: document.querySelector("#discard-recovery"),
  refreshProfiles: document.querySelector("#refresh-profiles"),
  profilesMessage: document.querySelector("#profiles-message"),
  profileList: document.querySelector("#profile-list"),
  initializationCard: document.querySelector("#initialization-card"),
  initializationWork: document.querySelector("#initialization-work"),
  initializationProgress: document.querySelector("#initialization-progress"),
  initializationMessage: document.querySelector("#initialization-message"),
  initializationBaseTime: document.querySelector("#initialization-base-time"),
  initializationModTime: document.querySelector("#initialization-mod-time"),
  initializationTotalTime: document.querySelector("#initialization-total-time"),
  reinitializeMods: document.querySelector("#reinitialize-mods"),
  databaseModsCard: document.querySelector("#database-mods-card"),
  townShell: document.querySelector("#town-shell"),
  townStage: document.querySelector("#town-stage"),
  townBackground: document.querySelector("#town-background"),
  townBackgroundFallback: document.querySelector(".town-background-fallback"),
  townSettingsButton: document.querySelector("#town-settings-button"),
  heroList: document.querySelector("#hero-list"),
  trinketGrid: document.querySelector("#trinket-grid"),
  resourceGrid: document.querySelector("#resource-grid"),
  townUndo: document.querySelector("#town-undo"),
  townRedo: document.querySelector("#town-redo"),
  townSave: document.querySelector("#town-save"),
  townSaveState: document.querySelector("#town-save-state"),
};

let locale = "zh_cn";
let strings = {};
let latestCampaign = null;

function t(key, variables = {}) {
  let value = strings[key] ?? key;
  for (const [name, replacement] of Object.entries(variables)) value = value.replace(`{${name}}`, replacement);
  return value;
}

function displayError(error) {
  const key = `errors.${error?.code ?? ""}`;
  return strings[key] ? t(key) : (error?.message ?? "");
}

async function setLocale(nextLocale) {
  const requested = nextLocale === "en_us" ? "en_us" : "zh_cn";
  try {
    const response = await fetch(`/assets/locales/${requested}.json`, { cache: "no-store" });
    if (!response.ok) throw new Error("locale unavailable");
    strings = await response.json();
    locale = requested;
  } catch {
    if (!Object.keys(strings).length) strings = { "app.title": "Darkest Dungeon Save Editor" };
  }
  document.querySelectorAll("[data-i18n]").forEach((node) => { node.textContent = t(node.dataset.i18n); });
  document.querySelectorAll("[data-i18n-aria]").forEach((node) => { node.setAttribute("aria-label", t(node.dataset.i18nAria)); });
  for (const option of elements.language.options) option.textContent = t(`language.${option.value}`);
}

function showTownShell() {
  elements.panel.hidden = true;
  elements.townShell.hidden = false;
  const candidates = [
    "fx/town_ground/town_ground.sprite.png",
    "__database__",
    "panels/town/town.png",
    "panels/town/estate.png",
    "campaign/town/town.png",
  ];
  let index = 0;
  elements.townStage.classList.remove("art-missing");
  elements.townBackground.onload = () => {
    elements.townStage.classList.remove("art-missing");
    elements.townBackgroundFallback.hidden = true;
  };
  const tryNext = () => {
    if (index >= candidates.length) {
      elements.townBackground.hidden = true;
      elements.townBackgroundFallback.hidden = false;
      elements.townStage.classList.add("art-missing");
      return;
    }
    elements.townBackground.hidden = false;
    elements.townBackgroundFallback.hidden = true;
    const candidate = candidates[index++];
    elements.townBackground.src = candidate === "__database__"
      ? "/api/town-asset?role=background"
      : `/api/game-asset?path=${encodeURIComponent(candidate)}`;
  };
  elements.townBackground.onerror = tryNext;
  tryNext();
  for (const building of document.querySelectorAll(".town-building")) {
    const id = building.dataset.building;
    const image = building.querySelector("img");
    const candidates = [
      `panels/town/buildings/${id}.png`,
      `campaign/town/buildings/${id}.png`,
    ];
    let index = 0;
    const tryNext = () => {
      if (index >= candidates.length) {
        image.hidden = true;
        return;
      }
      image.hidden = false;
      image.src = `/api/game-asset?path=${encodeURIComponent(candidates[index++])}`;
    };
    image.onerror = tryNext;
    tryNext();
  }
  loadCampaign().catch((error) => {
    const message = displayError(error) || t("town.dataUnavailable");
    elements.heroList.textContent = message;
    elements.trinketGrid.textContent = message;
    elements.resourceGrid.textContent = message;
  });
}

function localizedResourceName(resource) {
  const keys = {
    gold: "town.gold", bust: "town.heirlooms", portraits: "town.portraits",
    deeds: "town.deeds", deed: "town.deeds", crests: "town.crests", crest: "town.crests",
    shards: "town.crystalline", shard: "town.crystalline",
    heirlooms: "town.heirlooms", statue: "town.heirlooms", portrait: "town.portraits",
    memory: "town.memory", blueprint: "town.blueprint",
  };
  const key = keys[String(resource.id || "").toLowerCase()];
  return key && strings[key] ? t(key) : (resource.name || resource.id || t("town.resource"));
}

function chooseAsset(assets = [], preferred = []) {
  const ordered = [...assets].sort((left, right) => {
    const a = preferred.findIndex((role) => String(left.role || "").toLowerCase().includes(role));
    const b = preferred.findIndex((role) => String(right.role || "").toLowerCase().includes(role));
    return (a < 0 ? 999 : a) - (b < 0 ? 999 : b);
  });
  return ordered.find((asset) => asset.resolved && asset.path) || ordered.find((asset) => asset.path);
}

function assetUrl(asset) {
  return asset ? `/api/content-asset?path=${encodeURIComponent(asset.path)}` : "";
}

function cleanGameText(value) {
  return String(value ?? "")
    .replace(/\[[^\]]*\]/g, "")
    .replace(/\s{2,}/g, " ")
    .trim();
}

function renderCampaign(campaign) {
  latestCampaign = campaign;
  const resources = campaign.resources || [];
  elements.resourceGrid.replaceChildren();
  if (!resources.length) {
    elements.resourceGrid.append(Object.assign(document.createElement("p"), { className: "town-data-message", textContent: t("town.noResources") }));
  }
  for (const resource of resources) {
    const item = document.createElement("label");
    item.className = "resource-editor";
    const name = document.createElement("b");
    name.textContent = cleanGameText(localizedResourceName(resource));
    const input = document.createElement("input");
    input.type = "number";
    input.min = "0";
    input.step = "1";
    input.value = resource.amount == null ? "" : resource.amount;
    input.disabled = !resource.editable;
    input.title = resource.id || "";
    input.addEventListener("change", async () => {
      if (input.disabled || input.value === "") return;
      input.disabled = true;
      try {
        const updated = await editorGateway.setCampaignResource(resource.index, Number(input.value), campaign.revision);
        renderCampaign(updated);
      } catch (error) {
        input.disabled = false;
        elements.townSaveState.textContent = displayError(error);
      }
    });
    item.append(name, input);
    elements.resourceGrid.append(item);
  }

  elements.heroList.replaceChildren();
  if (!campaign.heroes?.length) {
    elements.heroList.append(Object.assign(document.createElement("p"), { className: "town-data-message", textContent: t("town.noHeroes") }));
  }
  for (const hero of campaign.heroes || []) {
    const row = document.createElement("div");
    row.className = "hero-entry";
    const asset = hero.portraitPath
      ? { path: hero.portraitPath, resolved: true }
      : chooseAsset(hero.assets, ["portrait_roster", "portrait", "hero", "roster"]);
    if (asset) {
      const image = document.createElement("img");
      image.alt = "";
      image.src = assetUrl(asset);
      image.onerror = () => { image.replaceWith(Object.assign(document.createElement("span"), { className: "missing-art portrait-fallback" })); };
      row.append(image);
    } else row.append(Object.assign(document.createElement("span"), { className: "missing-art portrait-fallback" }));
    const text = document.createElement("div");
    text.className = "hero-entry-text";
    const name = document.createElement("strong");
    name.textContent = cleanGameText(hero.name || hero.id);
    const className = document.createElement("small");
    className.textContent = cleanGameText(hero.className || hero.classId || t("town.unknownClass"));
    text.append(name, className);
    row.append(text);
    elements.heroList.append(row);
  }

  elements.trinketGrid.replaceChildren();
  if (!campaign.trinkets?.length) {
    elements.trinketGrid.append(Object.assign(document.createElement("p"), { className: "town-data-message", textContent: t("town.noTrinkets") }));
  }
  for (const trinket of campaign.trinkets || []) {
    const item = document.createElement("div");
    item.className = "trinket-entry";
    const asset = chooseAsset(trinket.assets, ["trinket", "item", "icon"]);
    if (asset) {
      const image = document.createElement("img");
      image.alt = cleanGameText(trinket.name || trinket.id || "");
      image.src = assetUrl(asset);
      image.onerror = () => { image.replaceWith(Object.assign(document.createElement("span"), { className: "missing-art item-fallback" })); };
      item.append(image);
    } else item.append(Object.assign(document.createElement("span"), { className: "missing-art item-fallback" }));
    if (trinket.amount > 1) {
      const count = document.createElement("small");
      count.textContent = `×${trinket.amount}`;
      item.append(count);
    }
    item.title = cleanGameText(trinket.name || trinket.id || "");
    elements.trinketGrid.append(item);
  }
  elements.townUndo.disabled = !campaign.canUndo;
  elements.townRedo.disabled = !campaign.canRedo;
  elements.townSave.disabled = !campaign.dirty;
  elements.townSaveState.textContent = campaign.dirty ? t("town.unsaved") : t("town.clean");
}

async function loadCampaign() {
  const campaign = await editorGateway.getCampaign();
  renderCampaign(campaign);
}

function showConfiguration(config) {
  elements.gameRoot.value = config.gameRoot ?? "";
  elements.saveRoots.value = config.saveRoots?.[0] ?? "";
  elements.workshopRoots.value = config.workshopRoots?.[0] ?? "";
  elements.localModRoots.value = config.localModRoots?.[0] ?? "";
  elements.backupRoot.value = config.backupRoot ?? "";
  elements.maxBackupCount.value = config.maxBackupCount ?? 20;
  elements.language.value = config.language ?? "zh_cn";
  elements.autoEditSaveEnabled.checked = config.autoEditSaveEnabled ?? true;
  elements.autoEditSaveInterval.value = config.autoEditSaveIntervalSeconds ?? 30;
  elements.configurationState.textContent = t("configuration.ready");
  elements.configurationSubmit.disabled = false;
  elements.configurationSubmit.dataset.mode = "save";
  elements.configurationSubmit.textContent = t("settings.save");
  elements.panel.hidden = false;
}

async function loadProfiles() {
  elements.profilesMessage.textContent = t("profiles.loading");
  elements.profileList.replaceChildren();
  try {
    const result = await editorGateway.listDatabaseMods();
    if (!result.mods.length) {
      elements.profilesMessage.textContent = t("profiles.empty");
      return;
    }
    elements.profilesMessage.textContent = t("profiles.found", { count: result.mods.length });
    for (const mod of result.mods) {
      const item = document.createElement("li");
      if (mod.coverUrl) {
        const cover = document.createElement("img");
        cover.className = "mod-cover";
        cover.alt = "";
        cover.src = mod.coverUrl;
        item.append(cover);
      }
      const text = document.createElement("span");
      const title = document.createElement("strong");
      title.textContent = t("profiles.detail", { order: mod.order + 1, name: mod.displayName, key: mod.key });
      const detail = document.createElement("span");
      detail.textContent = mod.provider ? `${mod.provider}${mod.externalId ? ` · ${mod.externalId}` : ""}` : mod.key;
      text.append(title, detail);
      item.append(text);
      elements.profileList.append(item);
    }
  } catch (error) {
    elements.profilesMessage.textContent = displayError(error);
  }
}

function formatMilliseconds(value) {
  return value == null ? "—" : t("initialization.milliseconds", { value });
}

async function loadInitialization() {
  let state = await editorGateway.getInitialization();
  elements.initializationCard.hidden = false;
  elements.reinitializeMods.hidden = true;
  while (state.status === "running" || state.status === "queued") {
    elements.configurationSubmit.disabled = true;
    elements.reinitializeMods.disabled = true;
    elements.configurationSubmit.dataset.mode = "running";
    elements.configurationSubmit.textContent = t("initialization.runningButton");
    elements.initializationWork.textContent = state.currentWork || t("initialization.working");
    elements.initializationProgress.style.width = `${state.progressPercent ?? 0}%`;
    elements.initializationMessage.textContent = t("initialization.progress", { percent: state.progressPercent ?? 0 });
    await new Promise((resolve) => window.setTimeout(resolve, 500));
    state = await editorGateway.getInitialization();
  }
  elements.initializationWork.textContent = state.currentWork || "";
  elements.initializationProgress.style.width = `${state.progressPercent ?? 0}%`;
  elements.initializationBaseTime.textContent = formatMilliseconds(state.baseElapsedMs);
  elements.initializationModTime.textContent = formatMilliseconds(state.modElapsedMs);
  elements.initializationTotalTime.textContent = formatMilliseconds(state.totalElapsedMs);
  if (state.status === "completed") {
    elements.initializationMessage.textContent = t("initialization.completed", { count: state.enabledMods ?? 0 });
    elements.configurationSubmit.disabled = false;
    elements.configurationSubmit.dataset.mode = "complete";
    elements.configurationSubmit.textContent = t("initialization.continue");
    elements.reinitializeMods.hidden = false;
    elements.reinitializeMods.disabled = false;
    if (state.reusedExisting) showTownShell();
  } else if (state.status === "failed") {
    elements.initializationMessage.textContent = [...(state.diagnostics ?? []), t("initialization.failed")].join(" ");
    elements.configurationSubmit.disabled = false;
    elements.configurationSubmit.dataset.mode = "save";
    elements.configurationSubmit.textContent = t("settings.save");
    elements.reinitializeMods.hidden = true;
  }
}

async function loadConfiguration() {
  const configuration = await editorGateway.getConfiguration();
  await setLocale(configuration.language);
  showConfiguration(configuration);
  const recovery = await editorGateway.getRecoveryStatus();
  if (recovery.available) {
    elements.recoveryMessage.textContent = `${recovery.profileId || "Profile"} · ${recovery.savedAt || ""} · revision ${recovery.revision}`;
    elements.recoveryCard.hidden = false;
    elements.restoreRecovery.onclick = async () => {
      try {
        const restored = await editorGateway.restoreRecovery();
        sessionStorage.setItem("ddse-recovery-snapshot", restored.snapshot);
        elements.recoveryMessage.textContent = t("recovery.loaded");
        elements.restoreRecovery.disabled = true;
      } catch (error) {
        elements.configurationMessage.textContent = displayError(error);
      }
    };
  }
  const initialization = await editorGateway.getInitialization();
  if (initialization.status === "idle" && configuration.gameRoot && configuration.backupRoot && configuration.dataRoot) {
    await editorGateway.startInitialization();
  }
  loadInitialization().catch((error) => { elements.initializationMessage.textContent = displayError(error); });
  elements.discardRecovery.onclick = async () => {
    try {
      await editorGateway.discardRecovery();
      elements.recoveryCard.hidden = true;
      elements.configurationMessage.textContent = t("recovery.discarded");
    } catch (error) {
      elements.configurationMessage.textContent = displayError(error);
    }
  };
}

elements.form.addEventListener("submit", async (event) => {
  event.preventDefault();
  if (elements.configurationSubmit.dataset.mode === "complete") {
    showTownShell();
    return;
  }
  elements.configurationSubmit.disabled = true;
  elements.configurationMessage.textContent = t("settings.saving");
  try {
    const configuration = await editorGateway.saveConfiguration({
      backupRoot: elements.backupRoot.value.trim(),
      maxBackupCount: Number(elements.maxBackupCount.value),
      language: elements.language.value.trim(),
      autoEditSaveEnabled: elements.autoEditSaveEnabled.checked,
      autoEditSaveIntervalSeconds: Number(elements.autoEditSaveInterval.value),
      gameRoot: elements.gameRoot.value.trim(),
      saveRoots: elements.saveRoots.value.trim() ? [elements.saveRoots.value.trim()] : [],
      workshopRoots: elements.workshopRoots.value.trim() ? [elements.workshopRoots.value.trim()] : [],
      localModRoots: elements.localModRoots.value.trim() ? [elements.localModRoots.value.trim()] : [],
    });
    showConfiguration(configuration);
    await setLocale(configuration.language);
    elements.configurationMessage.textContent = t("settings.saved");
    await loadInitialization();
  } catch (error) {
    elements.configurationSubmit.disabled = false;
    elements.configurationSubmit.dataset.mode = "save";
    elements.configurationSubmit.textContent = t("settings.save");
    elements.configurationMessage.textContent = displayError(error);
  }
});

elements.refreshProfiles.addEventListener("click", loadProfiles);
elements.reinitializeMods.addEventListener("click", async () => {
  elements.reinitializeMods.disabled = true;
  elements.configurationMessage.textContent = t("initialization.reinitializing");
  try {
    await editorGateway.startInitialization(true);
    await loadInitialization();
  } catch (error) {
    elements.reinitializeMods.disabled = false;
    elements.configurationMessage.textContent = displayError(error);
  }
});
elements.townSettingsButton.addEventListener("click", () => {
  elements.townShell.hidden = true;
  elements.panel.hidden = false;
});
elements.townUndo.addEventListener("click", async () => {
  if (!latestCampaign?.canUndo) return;
  elements.townUndo.disabled = true;
  try { renderCampaign(await editorGateway.undoCampaign(latestCampaign.revision)); }
  catch (error) { elements.townSaveState.textContent = displayError(error); }
});
elements.townRedo.addEventListener("click", async () => {
  if (!latestCampaign?.canRedo) return;
  elements.townRedo.disabled = true;
  try { renderCampaign(await editorGateway.redoCampaign(latestCampaign.revision)); }
  catch (error) { elements.townSaveState.textContent = displayError(error); }
});
elements.townSave.addEventListener("click", () => {
  elements.townSaveState.textContent = t("town.saveDeferred");
});
elements.language.addEventListener("change", async () => {
  await setLocale(elements.language.value);
  if (!elements.databaseModsCard.hidden) await loadProfiles();
});

for (const button of document.querySelectorAll("[data-directory-kind]")) {
  button.addEventListener("click", async () => {
    button.disabled = true;
    try {
      const result = await editorGateway.selectDirectory(button.dataset.directoryKind);
      const input = document.querySelector(`#${{
        game: "game-root",
        save: "save-roots",
        workshop: "workshop-roots",
        localMod: "local-mod-roots",
        backup: "backup-root",
      }[button.dataset.directoryKind]}`);
      if (input) input.value = result.path;
    } catch (error) {
      if (error.code !== "DIRECTORY_PICKER_CANCELLED") elements.configurationMessage.textContent = displayError(error);
    } finally {
      button.disabled = false;
    }
  });
}

async function connect() {
  const deadline = Date.now() + 6000;
  let lastError;
  while (Date.now() < deadline) {
    try {
      const status = await editorGateway.getApplicationStatus();
      elements.indicator.classList.add("ready");
      elements.title.textContent = t("connection.ready");
      elements.message.textContent = t("connection.readyMessage", { name: status.application.name });
      elements.state.textContent = status.state;
      elements.appVersion.textContent = status.application.version;
      elements.apiVersion.textContent = `v${status.apiVersion}`;
      elements.details.hidden = false;
      await loadConfiguration();
      return;
    } catch (error) {
      lastError = error;
      if (error instanceof GatewayError && error.code !== "LOCAL_SERVICE_UNAVAILABLE") break;
      await new Promise((resolve) => window.setTimeout(resolve, 350));
    }
  }

  elements.indicator.classList.add("failed");
  elements.title.textContent = t("connection.failed");
  elements.message.textContent = t("connection.failedMessage");
  elements.error.textContent = displayError(lastError) || t("connection.failedMessage");
  elements.error.hidden = false;
}

connect();
