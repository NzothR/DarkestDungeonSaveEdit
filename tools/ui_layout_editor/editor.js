const CANVAS_WIDTH = 1920;
const CANVAS_HEIGHT = 1080;
const ID_PATTERN = /^[A-Za-z_][A-Za-z0-9_]*$/;

const PRESETS = [
  { id: "town_background", name: "小镇背景", placement: "background" },
  { id: "bottom_bar", name: "底栏背景", placement: "bottom_bar" },
  { id: "estate_name", name: "存档名字", placement: "free" },
  { id: "building_survivalist", name: "生存大师", placement: "free" },
  { id: "building_stage_coach", name: "马车", placement: "free" },
  { id: "building_tavern", name: "酒馆", placement: "free" },
  { id: "building_sanitarium", name: "疗养院", placement: "free" },
  { id: "building_abbey", name: "教堂", placement: "free" },
  { id: "building_graveyard", name: "墓园", placement: "free" },
  { id: "building_nomad_wagon", name: "游牧民货车", placement: "free" },
  { id: "building_guild", name: "工会", placement: "free" },
  { id: "building_blacksmith", name: "铁匠铺", placement: "free" },
  { id: "hero_roster", name: "英雄名单", placement: "free" },
  { id: "icon_settings", name: "设置图标", placement: "free" },
  { id: "icon_trinket_chest", name: "饰品箱图标", placement: "free" },
  { id: "icon_tutorial", name: "教学图标", placement: "free" },
  { id: "icon_log", name: "日志图标", placement: "free" },
  { id: "icon_quest", name: "任务图标", placement: "free" },
  { id: "resource_gold", name: "金币图标", placement: "free" },
  { id: "resource_heirlooms", name: "雕像图标", placement: "free" },
  { id: "resource_portraits", name: "画像图标", placement: "free" },
  { id: "resource_deeds", name: "地契图标", placement: "free" },
  { id: "resource_crests", name: "纹章图标", placement: "free" },
  { id: "resource_crystalline", name: "星晶图标", placement: "free" },
];

const elements = {
  canvas: document.querySelector("#canvas"),
  assetSummary: document.querySelector("#asset-summary"),
  componentEmpty: document.querySelector("#component-empty"),
  componentList: document.querySelector("#component-list"),
  status: document.querySelector("#editor-status"),
  addComponent: document.querySelector("#add-component"),
  refreshAssets: document.querySelector("#refresh-assets"),
  finishEditing: document.querySelector("#finish-editing"),
  saveLayout: document.querySelector("#save-layout"),
  saveDialog: document.querySelector("#save-dialog"),
  saveForm: document.querySelector("#save-form"),
  saveFilename: document.querySelector("#save-filename"),
  saveError: document.querySelector("#save-error"),
  cancelSave: document.querySelector("#cancel-save"),
  cancelSaveBottom: document.querySelector("#cancel-save-bottom"),
  assetDialog: document.querySelector("#asset-dialog"),
  assetSearch: document.querySelector("#asset-search"),
  assetList: document.querySelector("#asset-list"),
  assetEmpty: document.querySelector("#asset-empty"),
  associationDialog: document.querySelector("#association-dialog"),
  associationForm: document.querySelector("#association-form"),
  associationSelect: document.querySelector("#association-select"),
  newAssociation: document.querySelector("#new-association"),
  newAssociationFields: document.querySelector("#new-association-fields"),
  associationName: document.querySelector("#association-name"),
  associationId: document.querySelector("#association-id"),
  associationError: document.querySelector("#association-error"),
  selectedAssetLabel: document.querySelector("#selected-asset-label"),
  selectedAssetPreview: document.querySelector("#selected-asset-preview"),
  cancelAssociation: document.querySelector("#cancel-association"),
  cancelAssociationBottom: document.querySelector("#cancel-association-bottom"),
};

const state = {
  assets: [],
  presets: PRESETS,
  associations: new Map(),
  components: [],
  selectedId: null,
  editing: false,
  pendingAsset: null,
  nextZ: 1,
};

function setStatus(message, kind = "") {
  elements.status.textContent = message;
  elements.status.className = `status ${kind}`.trim();
}

function associationFor(id) {
  return state.associations.get(id) || state.presets.find((item) => item.id === id);
}

function assetUrl(path) {
  return `/asset/${path.split("/").map(encodeURIComponent).join("/")}`;
}

async function requestJson(path, options = {}) {
  const response = await fetch(path, options);
  const payload = await response.json();
  if (!response.ok) throw new Error(payload.error || `请求失败 (${response.status})`);
  return payload;
}

async function loadAssets() {
  state.assets = await requestJson("/api/assets");
  elements.assetSummary.textContent = `已扫描 ${state.assets.length} 个图片资源；保存文件只记录资源路径和布局关系。`;
  renderAssetList();
}

async function loadPresets() {
  const presets = await requestJson("/api/presets");
  if (Array.isArray(presets) && presets.length) state.presets = presets;
}

async function loadLayout() {
  const layout = await requestJson("/api/layout");
  state.associations.clear();
  for (const association of layout.associations || []) state.associations.set(association.id, association);
  state.components = (layout.components || []).map((component) => ({ ...component }));
  state.nextZ = state.components.reduce((max, item) => Math.max(max, Number(item.zIndex) || 0), 0) + 1;
}

function renderAssetList() {
  const query = elements.assetSearch.value.trim().toLowerCase();
  const assets = state.assets.filter((asset) => `${asset.name} ${asset.path}`.toLowerCase().includes(query));
  elements.assetList.replaceChildren();
  elements.assetEmpty.hidden = assets.length !== 0;
  for (const asset of assets) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "asset-card";
    button.innerHTML = `<img src="${assetUrl(asset.path)}" alt=""><strong></strong><small></small>`;
    button.querySelector("strong").textContent = asset.name;
    button.querySelector("small").textContent = asset.path;
    button.addEventListener("click", () => openAssociation(asset));
    elements.assetList.append(button);
  }
}

function renderAssociationOptions() {
  const options = [...state.presets];
  for (const association of state.associations.values()) {
    if (!options.some((item) => item.id === association.id)) options.push(association);
  }
  elements.associationSelect.replaceChildren(new Option("选择一个预设关联", ""));
  for (const association of options) {
    elements.associationSelect.append(new Option(`${association.name} (${association.id})`, association.id));
  }
}

function openAssetPicker() {
  elements.assetSearch.value = "";
  renderAssetList();
  elements.assetDialog.showModal();
}

function openAssociation(asset) {
  state.pendingAsset = asset;
  elements.selectedAssetLabel.textContent = `${asset.name} · ${asset.path}`;
  elements.selectedAssetPreview.src = assetUrl(asset.path);
  elements.associationError.hidden = true;
  elements.newAssociationFields.hidden = true;
  elements.newAssociation.textContent = "新增关联";
  elements.associationSelect.value = "";
  renderAssociationOptions();
  elements.assetDialog.close();
  elements.associationDialog.showModal();
}

function defaultGeometry(placement, asset) {
  if (placement === "background") return { x: 0, y: 0, width: CANVAS_WIDTH, height: CANVAS_HEIGHT };
  if (placement === "bottom_bar") return { x: 0, y: 940, width: CANVAS_WIDTH, height: 140 };
  const preview = elements.selectedAssetPreview;
  const ratio = preview.naturalWidth && preview.naturalHeight ? preview.naturalWidth / preview.naturalHeight : 1.5;
  const width = Math.min(640, CANVAS_WIDTH * .38);
  return { x: (CANVAS_WIDTH - width) / 2, y: (CANVAS_HEIGHT - width / ratio) / 2, width, height: width / ratio };
}

function relationFromDialog() {
  const isCustom = !elements.newAssociationFields.hidden;
  if (!isCustom) {
    const selected = elements.associationSelect.value;
    if (!selected) throw new Error("请选择预设关联，或点击新增关联输入名称和 ID。\n");
    return associationFor(selected);
  }
  const name = elements.associationName.value.trim();
  const id = elements.associationId.value.trim();
  if (!name) throw new Error("关联名称不能为空。\n");
  if (!ID_PATTERN.test(id)) throw new Error("关联 ID 必须符合变量名规则。\n");
  if (state.presets.some((item) => item.id === id) || state.associations.has(id)) throw new Error("这个关联 ID 已经存在。\n");
  return { id, name, placement: "free" };
}

function addComponent(event) {
  event.preventDefault();
  if (!state.pendingAsset) return;
  try {
    const association = relationFromDialog();
    const geometry = defaultGeometry(association.placement, state.pendingAsset);
    const existing = state.components.findIndex((component) => component.associationId === association.id);
    if (existing >= 0) state.components.splice(existing, 1);
    state.associations.set(association.id, association);
    const component = {
      associationId: association.id,
      assetPath: state.pendingAsset.path,
      ...geometry,
      zIndex: state.nextZ++,
    };
    state.components.push(component);
    state.selectedId = component.associationId;
    state.editing = association.placement === "free";
    elements.associationDialog.close();
    render();
    setStatus(state.editing ? "组件已放置，正在编辑" : "特殊组件已固定放置", "success");
  } catch (error) {
    elements.associationError.textContent = error.message;
    elements.associationError.hidden = false;
  }
}

function applyGeometry(element, component) {
  element.style.left = `${component.x / CANVAS_WIDTH * 100}%`;
  element.style.top = `${component.y / CANVAS_HEIGHT * 100}%`;
  element.style.width = `${component.width / CANVAS_WIDTH * 100}%`;
  element.style.height = `${component.height / CANVAS_HEIGHT * 100}%`;
  element.style.zIndex = component.zIndex;
}

function renderCanvas() {
  elements.canvas.replaceChildren();
  for (const component of [...state.components].sort((a, b) => a.zIndex - b.zIndex)) {
    const association = associationFor(component.associationId) || { id: component.associationId, name: component.associationId, placement: "free" };
    const layer = document.createElement("div");
    layer.className = `layout-component ${association.placement === "free" ? "editable" : "fixed"}`;
    layer.dataset.id = component.associationId;
    layer.title = `${association.name} (${association.id})`;
    applyGeometry(layer, component);
    const image = document.createElement("img");
    image.src = assetUrl(component.assetPath);
    image.alt = association.name;
    layer.append(image);
    if (association.placement === "free" && state.editing && state.selectedId === component.associationId) {
      layer.classList.add("selected");
      const handle = document.createElement("span");
      handle.className = "resize-handle";
      layer.append(handle);
      attachResize(layer, handle, component);
    }
    if (association.placement === "free") attachDrag(layer, component);
    elements.canvas.append(layer);
  }
}

function attachDrag(layer, component) {
  layer.addEventListener("pointerdown", (event) => {
    if (!state.editing || event.button !== 0 || event.target.closest(".resize-handle")) return;
    event.preventDefault();
    const rect = elements.canvas.getBoundingClientRect();
    const start = { x: event.clientX, y: event.clientY, componentX: component.x, componentY: component.y };
    layer.setPointerCapture(event.pointerId);
    const move = (moveEvent) => {
      component.x = Math.max(0, Math.min(CANVAS_WIDTH - component.width, start.componentX + (moveEvent.clientX - start.x) * CANVAS_WIDTH / rect.width));
      component.y = Math.max(0, Math.min(CANVAS_HEIGHT - component.height, start.componentY + (moveEvent.clientY - start.y) * CANVAS_HEIGHT / rect.height));
      applyGeometry(layer, component);
    };
    const stop = () => {
      layer.removeEventListener("pointermove", move);
      layer.removeEventListener("pointerup", stop);
      updateComponentList();
    };
    layer.addEventListener("pointermove", move);
    layer.addEventListener("pointerup", stop, { once: true });
  });
  layer.addEventListener("contextmenu", (event) => {
    event.preventDefault();
    state.selectedId = component.associationId;
    state.editing = true;
    render();
    setStatus("已进入组件编辑模式");
  });
  layer.addEventListener("click", (event) => {
    if (event.target.closest(".resize-handle")) return;
    state.selectedId = component.associationId;
    render();
  });
}

function attachResize(layer, handle, component) {
  handle.addEventListener("pointerdown", (event) => {
    event.preventDefault();
    event.stopPropagation();
    const rect = elements.canvas.getBoundingClientRect();
    const start = { x: event.clientX, y: event.clientY, width: component.width, height: component.height };
    const ratio = component.width / component.height;
    handle.setPointerCapture(event.pointerId);
    const move = (moveEvent) => {
      const width = Math.max(40, start.width + (moveEvent.clientX - start.x) * CANVAS_WIDTH / rect.width);
      component.width = Math.min(CANVAS_WIDTH - component.x, width);
      component.height = Math.min(CANVAS_HEIGHT - component.y, component.width / ratio);
      applyGeometry(layer, component);
    };
    const stop = () => {
      handle.removeEventListener("pointermove", move);
      handle.removeEventListener("pointerup", stop);
      updateComponentList();
    };
    handle.addEventListener("pointermove", move);
    handle.addEventListener("pointerup", stop, { once: true });
  });
}

function updateComponentList() {
  elements.componentList.replaceChildren();
  elements.componentEmpty.hidden = state.components.length !== 0;
  for (const component of [...state.components].sort((a, b) => a.zIndex - b.zIndex)) {
    const association = associationFor(component.associationId);
    const asset = state.assets.find((item) => item.path === component.assetPath);
    const button = document.createElement("button");
    button.type = "button";
    button.innerHTML = `<img alt=""><span><strong></strong><small></small></span>`;
    button.querySelector("img").src = assetUrl(component.assetPath);
    button.querySelector("strong").textContent = association?.name || component.associationId;
    button.querySelector("small").textContent = `${component.associationId} · ${asset?.path || component.assetPath}`;
    button.addEventListener("click", () => {
      state.selectedId = component.associationId;
      render();
    });
    elements.componentList.append(button);
  }
}

function render() {
  renderCanvas();
  updateComponentList();
  elements.finishEditing.hidden = !state.editing;
  elements.saveLayout.hidden = state.editing;
}

function openSaveDialog() {
  if (state.editing) {
    setStatus("请先点击“编辑完成”再保存。", "error");
    return;
  }
  elements.saveError.hidden = true;
  elements.saveDialog.showModal();
  elements.saveFilename.focus();
}

async function saveLayout(event) {
  event.preventDefault();
  try {
    const payload = {
      version: 1,
      canvas: { width: CANVAS_WIDTH, height: CANVAS_HEIGHT },
      associations: [...state.associations.values()],
      components: state.components,
    };
    const filename = elements.saveFilename.value.trim();
    if (!filename) throw new Error("请输入保存文件名。");
    const result = await requestJson("/api/layout", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ ...payload, filename }) });
    elements.saveDialog.close();
    setStatus(`已保存布局：${result.path}`, "success");
  } catch (error) {
    elements.saveError.textContent = error.message;
    elements.saveError.hidden = false;
  }
}

elements.addComponent.addEventListener("click", openAssetPicker);
elements.refreshAssets.addEventListener("click", async () => {
  try { await loadAssets(); render(); setStatus("资源列表已刷新", "success"); }
  catch (error) { setStatus(error.message, "error"); }
});
elements.assetSearch.addEventListener("input", renderAssetList);
elements.associationForm.addEventListener("submit", addComponent);
elements.newAssociation.addEventListener("click", () => {
  const opening = elements.newAssociationFields.hidden;
  elements.newAssociationFields.hidden = !opening;
  elements.newAssociation.textContent = opening ? "使用预设关联" : "新增关联";
  elements.associationSelect.disabled = opening;
  if (opening) elements.associationName.focus();
});
elements.cancelAssociation.addEventListener("click", () => elements.associationDialog.close());
elements.cancelAssociationBottom.addEventListener("click", () => elements.associationDialog.close());
elements.finishEditing.addEventListener("click", () => { state.editing = false; render(); setStatus("编辑完成，可以保存布局。", "success"); });
elements.saveForm.addEventListener("submit", saveLayout);
elements.saveLayout.addEventListener("click", openSaveDialog);
elements.cancelSave.addEventListener("click", () => elements.saveDialog.close());
elements.cancelSaveBottom.addEventListener("click", () => elements.saveDialog.close());

Promise.all([loadAssets(), loadPresets(), loadLayout()]).then(() => { render(); setStatus("布局已加载"); }).catch((error) => setStatus(error.message, "error"));
