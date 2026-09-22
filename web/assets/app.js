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
};

function lines(value) {
  return (value ?? []).join("\n");
}

function parseLines(value) {
  return value.split(/\r?\n/).map((item) => item.trim()).filter(Boolean);
}

function showConfiguration(config) {
  elements.gameRoot.value = config.gameRoot ?? "";
  elements.saveRoots.value = lines(config.saveRoots);
  elements.workshopRoots.value = lines(config.workshopRoots);
  elements.localModRoots.value = lines(config.localModRoots);
  elements.backupRoot.value = config.backupRoot ?? "";
  elements.maxBackupCount.value = config.maxBackupCount ?? 20;
  elements.language.value = config.language ?? "english";
  elements.autoEditSaveEnabled.checked = config.autoEditSaveEnabled ?? true;
  elements.autoEditSaveInterval.value = config.autoEditSaveIntervalSeconds ?? 30;
  elements.configurationState.textContent = "已初始化";
  elements.panel.hidden = false;
}

async function loadProfiles() {
  elements.profilesMessage.textContent = "正在发现 Profile…";
  elements.profileList.replaceChildren();
  try {
    const result = await editorGateway.listProfiles();
    if (!result.profiles.length) {
      elements.profilesMessage.textContent = "没有发现 profile_* 目录。";
      return;
    }
    elements.profilesMessage.textContent = `发现 ${result.profiles.length} 个 Profile。`;
    for (const profile of result.profiles) {
      const item = document.createElement("li");
      const title = document.createElement("strong");
      title.textContent = `${profile.id} · ${profile.status}`;
      const detail = document.createElement("span");
      detail.textContent = `${profile.rootPath} · ${profile.documentCount} 个文档`;
      item.append(title, detail);
      elements.profileList.append(item);
    }
  } catch (error) {
    elements.profilesMessage.textContent = error.message;
  }
}

async function loadConfiguration() {
  const configuration = await editorGateway.getConfiguration();
  showConfiguration(configuration);
  const recovery = await editorGateway.getRecoveryStatus();
  if (!recovery.available) return;
  elements.recoveryMessage.textContent = `档案 ${recovery.profileId || "未知"} 在 ${recovery.savedAt || "最近一次编辑"} 留有恢复点（revision ${recovery.revision}）。`;
  elements.recoveryCard.hidden = false;
  elements.restoreRecovery.onclick = async () => {
    try {
      const restored = await editorGateway.restoreRecovery();
      sessionStorage.setItem("ddse-recovery-snapshot", restored.snapshot);
      elements.recoveryMessage.textContent = "恢复点已载入，等待打开对应存档后恢复编辑。";
      elements.restoreRecovery.disabled = true;
    } catch (error) {
      elements.configurationMessage.textContent = error.message;
    }
  };
  elements.discardRecovery.onclick = async () => {
    try {
      await editorGateway.discardRecovery();
      elements.recoveryCard.hidden = true;
      elements.configurationMessage.textContent = "已放弃上次恢复点。";
    } catch (error) {
      elements.configurationMessage.textContent = error.message;
    }
  };
}

elements.form.addEventListener("submit", async (event) => {
  event.preventDefault();
  elements.configurationMessage.textContent = "正在保存配置…";
  try {
    const configuration = await editorGateway.saveConfiguration({
      backupRoot: elements.backupRoot.value.trim(),
      maxBackupCount: Number(elements.maxBackupCount.value),
      language: elements.language.value.trim(),
      autoEditSaveEnabled: elements.autoEditSaveEnabled.checked,
      autoEditSaveIntervalSeconds: Number(elements.autoEditSaveInterval.value),
      gameRoot: elements.gameRoot.value.trim(),
      saveRoots: parseLines(elements.saveRoots.value),
      workshopRoots: parseLines(elements.workshopRoots.value),
      localModRoots: parseLines(elements.localModRoots.value),
    });
    showConfiguration(configuration);
    elements.configurationMessage.textContent = "配置已保存，备份目录已初始化。";
    await loadProfiles();
  } catch (error) {
    elements.configurationMessage.textContent = error.message;
  }
});

elements.refreshProfiles.addEventListener("click", loadProfiles);

async function connect() {
  const deadline = Date.now() + 6000;
  let lastError;
  while (Date.now() < deadline) {
    try {
      const status = await editorGateway.getApplicationStatus();
      elements.indicator.classList.add("ready");
      elements.title.textContent = "本地服务已连接";
      elements.message.textContent = `${status.application.name} 正在本机运行。`;
      elements.state.textContent = status.state;
      elements.appVersion.textContent = status.application.version;
      elements.apiVersion.textContent = `v${status.apiVersion}`;
      elements.details.hidden = false;
      await loadConfiguration();
      await loadProfiles();
      return;
    } catch (error) {
      lastError = error;
      if (error instanceof GatewayError && error.code !== "LOCAL_SERVICE_UNAVAILABLE") break;
      await new Promise((resolve) => window.setTimeout(resolve, 350));
    }
  }

  elements.indicator.classList.add("failed");
  elements.title.textContent = "无法连接本地服务";
  elements.message.textContent = "请确认 DDSE 本机服务仍在运行，然后重新载入此页面。";
  elements.error.textContent = lastError?.message ?? "本机服务没有在限定时间内响应。";
  elements.error.hidden = false;
}

connect();
