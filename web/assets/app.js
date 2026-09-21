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
};

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
