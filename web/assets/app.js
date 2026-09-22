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
  initializationComplete: document.querySelector("#initialization-complete"),
  databaseModsCard: document.querySelector("#database-mods-card"),
};

let locale = "zh_cn";
let strings = {};

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
  while (state.status === "running" || state.status === "queued") {
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
    elements.initializationComplete.hidden = false;
    elements.databaseModsCard.hidden = false;
    await loadProfiles();
  } else if (state.status === "failed") {
    elements.initializationMessage.textContent = [...(state.diagnostics ?? []), t("initialization.failed")].join(" ");
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
    elements.configurationMessage.textContent = displayError(error);
  }
});

elements.refreshProfiles.addEventListener("click", loadProfiles);
elements.initializationComplete.addEventListener("click", () => {
  elements.initializationCard.hidden = true;
  elements.databaseModsCard.hidden = false;
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
