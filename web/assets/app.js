import { editorGateway, GatewayError } from "./gateway.js";
import { mountSpine21 } from "./spine21.js";

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
  townReloadProfile: document.querySelector("#town-reload-profile"),
  townReloadOverlay: document.querySelector("#town-reload-overlay"),
  settingsReturnTown: document.querySelector("#settings-return-town"),
  heroList: document.querySelector("#hero-list"),
  heroAdd: document.querySelector("#hero-add"),
  heroSelector: document.querySelector("#hero-selector"),
  heroSelectorClose: document.querySelector("#hero-selector-close"),
  heroSearch: document.querySelector("#hero-search"),
  heroModFilter: document.querySelector("#hero-mod-filter"),
  heroSelectorMessage: document.querySelector("#hero-selector-message"),
  heroSelectorList: document.querySelector("#hero-selector-list"),
  heroSelectionCount: document.querySelector("#hero-selection-count"),
  heroClearSelection: document.querySelector("#hero-clear-selection"),
  heroBatchConfirm: document.querySelector("#hero-batch-confirm"),
  heroDetail: document.querySelector("#hero-detail"),
  heroDetailClose: document.querySelector("#hero-detail-close"),
  heroDetailName: document.querySelector("#hero-detail-name"),
  heroDetailClass: document.querySelector("#hero-detail-class"),
  heroDetailRename: document.querySelector("#hero-detail-rename"),
  heroDetailLevel: document.querySelector("#hero-detail-level"),
  heroDetailXp: document.querySelector("#hero-detail-xp"),
  heroDetailStress: document.querySelector("#hero-detail-stress"),
  heroDetailStressBar: document.querySelector("#hero-detail-stress-bar"),
  heroDetailIdle: document.querySelector("#hero-detail-idle"),
  heroDetailPositive: document.querySelector("#hero-detail-positive"),
  heroDetailNegative: document.querySelector("#hero-detail-negative"),
  heroDetailUnknownWrap: document.querySelector("#hero-detail-unknown-wrap"),
  heroDetailUnknown: document.querySelector("#hero-detail-unknown"),
  heroDetailWeapon: document.querySelector("#hero-detail-weapon"),
  heroDetailArmour: document.querySelector("#hero-detail-armour"),
  heroDetailTrinkets: document.querySelector("#hero-detail-trinkets"),
  heroDetailCombat: document.querySelector("#hero-detail-combat"),
  heroDetailCamping: document.querySelector("#hero-detail-camping"),
  heroDetailDiseases: document.querySelector("#hero-detail-diseases"),
  trinketGrid: document.querySelector("#trinket-grid"),
  trinketSort: document.querySelector("#trinket-sort"),
  trinketBatchAdd: document.querySelector("#trinket-batch-add"),
  trinketBatchDelete: document.querySelector("#trinket-batch-delete"),
  trinketSelector: document.querySelector("#trinket-selector"),
  trinketSelectorTitle: document.querySelector("#trinket-selector-title"),
  trinketSelectorClose: document.querySelector("#trinket-selector-close"),
  trinketSearch: document.querySelector("#trinket-search"),
  trinketModFilter: document.querySelector("#trinket-mod-filter"),
  trinketClassFilter: document.querySelector("#trinket-class-filter"),
  trinketRarityFilter: document.querySelector("#trinket-rarity-filter"),
  trinketSelectorMessage: document.querySelector("#trinket-selector-message"),
  trinketSelectorList: document.querySelector("#trinket-selector-list"),
  trinketBatchFooter: document.querySelector("#trinket-batch-footer"),
  trinketBatchConfirm: document.querySelector("#trinket-batch-confirm"),
  trinketBatchHint: document.querySelector("#trinket-batch-hint"),
  trinketClearSelection: document.querySelector("#trinket-clear-selection"),
  trinketSelectionCount: document.querySelector("#trinket-selection-count"),
  trinketOnlyNew: document.querySelector("#trinket-only-new"),
  resourceGrid: document.querySelector("#resource-grid"),
  townUndo: document.querySelector("#town-undo"),
  townRedo: document.querySelector("#town-redo"),
  townSave: document.querySelector("#town-save"),
  townSaveState: document.querySelector("#town-save-state"),
  buildingList: document.querySelector("#building-list"),
  buildingMaximizeAll: document.querySelector("#building-maximize-all"),
  buildingEditor: document.querySelector("#building-editor"),
  buildingEditorTitle: document.querySelector("#building-editor-title"),
  buildingEditorKicker: document.querySelector("#building-editor-kicker"),
  buildingEditorActions: document.querySelector("#building-editor-actions"),
  buildingEditorHint: document.querySelector("#building-editor-hint"),
  buildingEditorContent: document.querySelector("#building-editor-content"),
  buildingEditorClose: document.querySelector("#building-editor-close"),
};

let locale = "zh_cn";
let strings = {};
let latestCampaign = null;
let currentConfiguration = null;
let settingsMode = false;
let trinketDefinitions = [];
let trinketSelectorMode = "add";
let selectedTrinketIds = new Set();
let selectedTrinketRawKeys = new Set();
let heroClassDefinitions = [];
let selectedHeroClassIds = new Set();
let activeHeroId = null;
let heroIdleKey = "";
let heroIdleStop = null;
let heroIdleController = null;

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
  document.querySelectorAll("[data-i18n-title]").forEach((node) => { node.setAttribute("title", t(node.dataset.i18nTitle)); });
  document.querySelectorAll("[data-i18n-placeholder]").forEach((node) => { node.setAttribute("placeholder", t(node.dataset.i18nPlaceholder)); });
  for (const option of elements.language.options) option.textContent = t(`language.${option.value}`);
}

function showTownShell() {
  settingsMode = false;
  elements.settingsReturnTown.hidden = true;
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
    .replace(/\[[^\]]*\]|\{[^{}]*\}/g, "")
    // Compiled .loc2 localization stores colour markup as
    // <c>XXXXXXtext</c>; the prefix is a game colour code.
    .replace(/<c>([\s\S]*?)<\/c>/gi, (_, content) => {
      let prefix = 0;
      while (prefix < content.length && prefix < 6 && content.charCodeAt(prefix) < 0x80) prefix += 1;
      return content.slice(prefix);
    })
    .replace(/<\/?c>/gi, "")
    .replace(/\s{2,}/g, " ")
    .trim();
}

function gameMarkup(value) {
  const text = String(value ?? "");
  const fragment = document.createDocumentFragment();
  const token = /<c>([\s\S]*?)<\/c>|\{colour_start\|([^}]+)\}([\s\S]*?)\{colour_end\}/gi;
  let offset = 0;
  for (const match of text.matchAll(token)) {
    fragment.append(document.createTextNode(cleanGameText(text.slice(offset, match.index))));
    const span = document.createElement("span");
    if (match[2]) {
      const color = match[2].toLowerCase();
      span.className = `game-color game-color-${color}`;
      span.textContent = cleanGameText(match[3]);
    } else {
      const content = match[1];
      let prefix = 0;
      while (prefix < content.length && prefix < 6 && content.charCodeAt(prefix) < 0x80) prefix += 1;
      const code = content.slice(0, prefix).toLowerCase();
      span.className = `game-color game-color-${code[0] || "default"}`;
      if (prefix === 6 && /^[0-9a-f]{6}$/i.test(code)) span.style.color = `#${code}`;
      span.textContent = cleanGameText(content.slice(prefix));
    }
    fragment.append(span);
    offset = match.index + match[0].length;
  }
  fragment.append(document.createTextNode(cleanGameText(text.slice(offset))));
  return fragment;
}

function trinketTooltip(trinket, linkedTrinketName = "") {
  const tooltip = document.createElement("div");
  tooltip.className = "trinket-tooltip";
  const title = document.createElement("strong");
  title.textContent = cleanGameText(trinket.name || trinket.id);
  tooltip.append(title);
  if (trinket.restriction) {
    const restriction = document.createElement("div");
    restriction.className = "trinket-restriction";
    restriction.textContent = `[${cleanGameText(trinket.restriction)}]`;
    tooltip.append(restriction);
  }
  if (trinket.rarityName || trinket.rarity != null) {
    const rarity = document.createElement("div");
    rarity.className = "trinket-rarity";
    rarity.textContent = cleanGameText(trinket.rarityName || t("trinket.rarity", { rarity: trinket.rarity }));
    tooltip.append(rarity);
  }
  const effectLines = Array.isArray(trinket.effects) && trinket.effects.length
    ? trinket.effects
    : trinket.description ? String(trinket.description).split("\n") : [];
  if (!effectLines.length) {
    const empty = document.createElement("div");
    empty.className = "trinket-empty-description";
    empty.textContent = t("trinket.hiddenEffects");
    tooltip.append(empty);
  }
  if (effectLines.length) {
    const description = document.createElement("div");
    description.className = "trinket-description";
    for (const line of effectLines) {
      const effect = document.createElement("div");
      effect.className = "trinket-effect";
      effect.append(gameMarkup(line));
      description.append(effect);
    }
    tooltip.append(description);
  }
  const source = document.createElement("div");
  source.className = "trinket-source";
  source.textContent = `${t("trinket.source")}: ${trinket.modName || trinket.sourceId || t("trinket.vanilla")} · ${trinket.sourceId || "vanilla"}`;
  tooltip.append(source);
  if (linkedTrinketName) {
    const linked = document.createElement("div");
    linked.className = "trinket-linked";
    const label = document.createElement("span");
    label.className = "trinket-linked-label";
    label.textContent = t("trinket.linkedHeader");
    const name = document.createElement("strong");
    name.className = "trinket-linked-name";
    name.textContent = cleanGameText(linkedTrinketName);
    linked.append(label, name);
    tooltip.append(linked);
  }
  return tooltip;
}

function trinketRarityId(trinket) {
  return trinket?.rarity == null ? "" : String(trinket.rarity);
}

function trinketRarityLabel(trinket) {
  const id = trinketRarityId(trinket);
  if (!id) return "";
  const localizationKey = `trinket.rarity.${id}`;
  const name = trinket.rarityName || (strings[localizationKey] ? t(localizationKey) : cleanGameText(id.replaceAll("_", " ")));
  return t("trinket.rarity", { rarity: cleanGameText(name) });
}

const TRINKET_RARITY_ORDER = Object.freeze([
  "darkest_dungeon", "trophy", "ancestral_shambler", "ancestral", "crimson_court",
  "comet", "mildred", "thing", "crow", "courtier", "collector", "madman",
  "very_rare", "rare", "uncommon", "common", "very_common", "kickstarter",
]);

function compareTrinkets(a, b) {
  const rank = (item) => {
    const id = trinketRarityId(item).toLowerCase();
    const name = String(item.rarityName || "").toLocaleLowerCase();
    let value = TRINKET_RARITY_ORDER.indexOf(id);
    if (value < 0) value = TRINKET_RARITY_ORDER.findIndex((key) =>
      String(strings[`trinket.rarity.${key}`] || "").toLocaleLowerCase() === name);
    return value < 0 ? TRINKET_RARITY_ORDER.length : value;
  };
  const rarityDifference = rank(a) - rank(b);
  if (rarityDifference) return rarityDifference;
  const collator = new Intl.Collator(locale === "zh_cn" ? "zh-CN" : "en-US", { sensitivity: "base" });
  const nameDifference = collator.compare(String(a.name || a.id || ""), String(b.name || b.id || ""));
  if (nameDifference) return nameDifference;
  return String(a.id || "").localeCompare(String(b.id || ""));
}

function pairTrinketInstances(inventory) {
  const groups = new Map();
  for (const trinket of inventory || []) {
    if (!trinket.hasSetBonus || !trinket.setId) continue;
    const set = groups.get(trinket.setId) || new Map();
    const idGroup = set.get(trinket.id) || [];
    idGroup.push(trinket);
    set.set(trinket.id, idGroup);
    groups.set(trinket.setId, set);
  }
  const pairs = [];
  const pairedKeys = new Set();
  for (const idGroups of groups.values()) {
    const buckets = [...idGroups.values()].map((items) => items.sort(compareTrinkets));
    while (buckets.filter((items) => items.length).length >= 2) {
      const available = buckets.filter((items) => items.length).sort((a, b) => compareTrinkets(a[0], b[0]));
      const first = available[0];
      const second = available[1];
      const count = Math.min(first.length, second.length);
      for (let index = 0; index < count; index += 1) {
        const pair = [first.shift(), second.shift()].sort(compareTrinkets);
        pairs.push(pair);
        for (const item of pair) pairedKeys.add(item.rawKey || String(item.index));
      }
    }
  }
  pairs.sort((a, b) => compareTrinkets(a[0], b[0]) || compareTrinkets(a[1], b[1]));
  const links = new Map();
  for (const [first, second] of pairs) {
    links.set(first.rawKey || String(first.index), second);
    links.set(second.rawKey || String(second.index), first);
  }
  return { pairs, links, pairedKeys };
}

function sortedTrinketInventory(inventory) {
  const { pairs, pairedKeys } = pairTrinketInstances(inventory);
  const paired = pairs.flat();
  const remaining = (inventory || []).filter((item) => !pairedKeys.has(item.rawKey || String(item.index))).sort(compareTrinkets);
  return [...paired, ...remaining];
}

function possibleLinkedTrinketName(trinket, inventory, links) {
  if (trinket.rawKey != null) {
    const linked = links.get(trinket.rawKey);
    return linked?.name || linked?.id || "";
  }
  if (!trinket.hasSetBonus || !trinket.setId) return "";
  const candidates = [...(inventory || []), ...trinketDefinitions]
    .filter((item) => item.hasSetBonus && item.setId === trinket.setId && item.id !== trinket.id)
    .sort(compareTrinkets);
  return candidates[0]?.name || candidates[0]?.id || "";
}

function attachTrinketTooltip(owner, tooltip) {
  tooltip.classList.add("trinket-tooltip-portal");
  tooltip.dataset.scope = owner.closest("#trinket-selector") ? "selector" : "inventory";
  const usePopover = typeof tooltip.showPopover === "function";
  if (usePopover) {
    // Native popovers enter the browser top layer above dialogs and scrolling
    // containers, so the selector can never paint over its own detail card.
    tooltip.setAttribute("popover", "manual");
    document.body.append(tooltip);
  } else {
    tooltip.hidden = true;
    (owner.closest("dialog") || document.body).append(tooltip);
  }
  const show = () => {
    if (usePopover) {
      if (!tooltip.matches(":popover-open")) tooltip.showPopover();
    } else tooltip.hidden = false;
    tooltip.classList.add("visible");
    const rect = owner.getBoundingClientRect();
    const width = Math.min(380, window.innerWidth * .45);
    let left = rect.right + 10;
    if (left + width > window.innerWidth - 8) left = rect.left - width - 10;
    tooltip.style.left = `${Math.max(8, left)}px`;
    tooltip.style.top = `${Math.max(8, Math.min(rect.top, window.innerHeight - 220))}px`;
    requestAnimationFrame(() => {
      if (tooltip.getBoundingClientRect().bottom > window.innerHeight - 8)
        tooltip.style.top = `${Math.max(8, window.innerHeight - tooltip.getBoundingClientRect().height - 8)}px`;
    });
  };
  const hide = () => {
    tooltip.classList.remove("visible");
    if (usePopover) {
      if (tooltip.matches(":popover-open")) tooltip.hidePopover();
    } else tooltip.hidden = true;
  };
  owner.addEventListener("pointerenter", show);
  owner.addEventListener("pointerleave", hide);
  owner.addEventListener("focus", show);
  owner.addEventListener("blur", hide);
  owner.addEventListener("wheel", (event) => {
    if (!event.shiftKey || !tooltip.classList.contains("visible")) return;
    event.preventDefault();
    tooltip.scrollTop += event.deltaY || event.deltaX;
  }, { passive: false });
  owner._hideTrinketTooltip = hide;
}

function heroDetailImage(path, assets, roles, fallback) {
  const asset = path ? { path, resolved: true } : chooseAsset((assets || []).filter((entry) =>
    String(entry.path || "").toLowerCase().endsWith(".png")), roles);
  if (!asset || !String(asset.path).toLowerCase().endsWith(".png")) return null;
  const image = document.createElement("img");
  image.alt = "";
  image.src = assetUrl(asset);
  image.onerror = () => { if (fallback) image.replaceWith(fallback()); else image.remove(); };
  return image;
}

function renderHeroDetailEntries(container, entries, kind, emptyKey) {
  container.replaceChildren();
  if (!entries.length) {
    container.append(Object.assign(document.createElement("p"), { className: "hero-detail-empty", textContent: t(emptyKey) }));
    return;
  }
  for (const entry of entries) {
    const card = document.createElement("div");
    card.className = `hero-detail-item hero-detail-item-${kind}`;
    if (entry.polarity === "negative") card.classList.add("hero-detail-item-negative");
    card.title = cleanGameText(entry.name || entry.id);
    if (kind === "camping" && entry.learned === false) {
      card.classList.add("hero-detail-skill-unlearned");
      card.title += ` · ${t("hero.skillUnlearned")}`;
    }
    if (kind !== "quirk") {
      const image = heroDetailImage(entry.iconPath, entry.assets, [kind, "icon", "skill", "trinket"], null);
      const art = kind === "trinket" ? document.createElement("span") : card;
      if (kind === "trinket") art.className = "hero-detail-trinket-art";
      if (image) art.append(image);
      else art.append(Object.assign(document.createElement("span"), { className: "hero-detail-item-symbol", textContent: kind === "disease" ? "✚" : "✦" }));
      if (kind === "trinket") card.append(art);
    }
    const name = document.createElement("span");
    name.textContent = cleanGameText(entry.name || entry.id);
    card.append(name);
    if (entry.isLocked) card.classList.add("locked");
    container.append(card);
  }
}

function renderHeroEquipment(container, equipment, kind) {
  container.replaceChildren();
  const label = t(kind === "weapon" ? "hero.weapon" : "hero.armour");
  const art = document.createElement("span");
  art.className = "hero-detail-gear-art";
  const image = heroDetailImage(equipment?.iconPath, null, [], null);
  if (image) art.append(image);
  else art.append(Object.assign(document.createElement("span"), { className: "hero-detail-item-symbol", textContent: kind === "weapon" ? "⚔" : "◆" }));
  const copy = document.createElement("span");
  copy.className = "hero-detail-gear-copy";
  copy.append(Object.assign(document.createElement("small"), { textContent: label }));
  const name = cleanGameText(equipment?.name || label);
  copy.append(Object.assign(document.createElement("strong"), { textContent: name }));
  copy.append(Object.assign(document.createElement("span"), {
    textContent: t("hero.equipmentRank", { rank: equipment?.rank == null ? "—" : equipment.rank }),
  }));
  container.title = `${name} · ${copy.lastChild.textContent}`;
  container.append(art, copy);
}

function stopHeroIdle() {
  heroIdleController?.abort();
  heroIdleController = null;
  heroIdleStop?.();
  heroIdleStop = null;
  heroIdleKey = "";
}

function renderIdleFallback(hero) {
  elements.heroDetailIdle.replaceChildren();
  const portrait = heroDetailImage(hero.portraitPath, hero.assets, ["portrait_roster", "portrait"],
    () => Object.assign(document.createElement("span"), { className: "hero-detail-idle-fallback", textContent: t("hero.previewUnavailable") }));
  if (portrait) elements.heroDetailIdle.append(portrait);
  else elements.heroDetailIdle.append(Object.assign(document.createElement("span"), { className: "hero-detail-idle-fallback", textContent: t("hero.previewUnavailable") }));
  elements.heroDetailIdle.append(Object.assign(document.createElement("small"), { textContent: t("hero.previewUnavailable") }));
}

function renderHeroIdle(hero) {
  const paths = { sprite: hero.idleSpritePath, atlas: hero.idleAtlasPath, skeleton: hero.idleSkeletonPath };
  const key = `${hero.id}|${paths.sprite}|${paths.atlas}|${paths.skeleton}`;
  if (key === heroIdleKey) return;
  stopHeroIdle();
  heroIdleKey = key;
  elements.heroDetailIdle.dataset.idleSpritePath = paths.sprite || "";
  elements.heroDetailIdle.dataset.idleAtlasPath = paths.atlas || "";
  elements.heroDetailIdle.dataset.idleSkeletonPath = paths.skeleton || "";
  if (!paths.sprite || !paths.atlas || !paths.skeleton) { renderIdleFallback(hero); return; }
  const canvas = document.createElement("canvas");
  canvas.setAttribute("aria-label", t("hero.idlePreview"));
  elements.heroDetailIdle.replaceChildren(canvas);
  const controller = new AbortController();
  heroIdleController = controller;
  mountSpine21(canvas, paths, controller.signal).then((stop) => {
    if (controller.signal.aborted || heroIdleKey !== key) stop();
    else heroIdleStop = stop;
  }).catch(() => {
    if (!controller.signal.aborted && heroIdleKey === key) renderIdleFallback(hero);
  });
}

function renderHeroDetail() {
  if (!activeHeroId) return;
  const hero = (latestCampaign?.heroes || []).find((entry) => entry.id === activeHeroId);
  if (!hero) { elements.heroDetail.close(); activeHeroId = null; return; }
  elements.heroDetailName.textContent = cleanGameText(hero.name || t("hero.unnamed"));
  elements.heroDetailClass.textContent = cleanGameText(hero.className || hero.classId || t("town.unknownClass"));
  elements.heroDetailRename.disabled = hero.nameEditable === false;
  elements.heroDetailLevel.textContent = hero.level == null ? "—" : String(hero.level);
  elements.heroDetailXp.textContent = hero.level == null && hero.resolveXp != null
    ? t("hero.xpUnresolved", { xp: hero.resolveXp }) : "";
  elements.heroDetailStress.textContent = hero.stress == null ? "—" : String(Math.round(hero.stress));
  elements.heroDetailStressBar.value = Math.max(0, Math.min(200, Number(hero.stress) || 0));
  const quirks = (hero.quirks || []).filter((entry) => !entry.isDisease);
  renderHeroDetailEntries(elements.heroDetailPositive, quirks.filter((entry) => entry.polarity === "positive"), "quirk", "hero.noQuirks");
  renderHeroDetailEntries(elements.heroDetailNegative, quirks.filter((entry) => entry.polarity === "negative"), "quirk", "hero.noQuirks");
  const unknownQuirks = quirks.filter((entry) => entry.polarity !== "positive" && entry.polarity !== "negative");
  elements.heroDetailUnknownWrap.hidden = !unknownQuirks.length;
  if (unknownQuirks.length) renderHeroDetailEntries(elements.heroDetailUnknown, unknownQuirks, "quirk", "hero.noQuirks");
  renderHeroEquipment(elements.heroDetailWeapon, hero.equipment?.weapon, "weapon");
  renderHeroEquipment(elements.heroDetailArmour, hero.equipment?.armour, "armour");
  renderHeroDetailEntries(elements.heroDetailCombat, hero.combatSkills || [], "combat", "hero.noSkills");
  renderHeroDetailEntries(elements.heroDetailCamping, hero.campingSkills || [], "camping", "hero.noSkills");
  renderHeroDetailEntries(elements.heroDetailDiseases, (hero.quirks || []).filter((entry) => entry.isDisease), "disease", "hero.noDiseases");
  renderHeroDetailEntries(elements.heroDetailTrinkets, hero.trinkets || [], "trinket", "hero.noTrinkets");
  renderHeroIdle(hero);
}

async function renameHero(hero) {
  const updatedName = window.prompt(t("hero.namePrompt"), hero.name || "");
  if (updatedName == null || updatedName === hero.name) return;
  try { renderCampaign(await editorGateway.editHeroes("rename", latestCampaign.revision,
    { heroId: hero.id, name: updatedName })); }
  catch (error) { elements.townSaveState.textContent = displayError(error); }
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

  renderBuildings(campaign);

  elements.heroList.replaceChildren();
  if (!campaign.heroes?.length) {
    elements.heroList.append(Object.assign(document.createElement("p"), { className: "town-data-message", textContent: t("town.noHeroes") }));
  }
  for (const hero of campaign.heroes || []) {
    const row = document.createElement("div");
    row.className = "hero-entry";
    row.dataset.heroId = hero.id;
    row.draggable = true;
    row.title = t("hero.rowHint");
    const asset = hero.portraitPath
      ? { path: hero.portraitPath, resolved: true }
      : chooseAsset(hero.assets, ["portrait_roster", "portrait", "hero", "roster"]);
    if (asset) {
      const image = document.createElement("img");
      image.alt = "";
      image.src = assetUrl(asset);
      image.draggable = false;
      image.onerror = () => { image.replaceWith(Object.assign(document.createElement("span"), { className: "missing-art portrait-fallback" })); };
      row.append(image);
    } else row.append(Object.assign(document.createElement("span"), { className: "missing-art portrait-fallback" }));
    const text = document.createElement("div");
    text.className = "hero-entry-text";
    const name = document.createElement("strong");
    name.textContent = cleanGameText(hero.name || t("hero.unnamed"));
    const className = document.createElement("small");
    className.textContent = cleanGameText(hero.className || hero.classId || t("town.unknownClass"));
    text.append(name, className);
    row.append(text);
    row.addEventListener("click", () => {
      activeHeroId = hero.id;
      renderHeroDetail();
      if (!elements.heroDetail.open) elements.heroDetail.showModal();
    });
    row.addEventListener("contextmenu", async (event) => {
      event.preventDefault();
      if (!event.shiftKey && !window.confirm(t("hero.deleteConfirm", { name: cleanGameText(hero.name || hero.className || hero.id) }))) return;
      try { renderCampaign(await editorGateway.editHeroes("delete", latestCampaign.revision, { heroId: hero.id })); }
      catch (error) { elements.townSaveState.textContent = displayError(error); }
    });
    row.addEventListener("dragstart", (event) => {
      row.classList.add("dragging");
      event.dataTransfer.effectAllowed = "move";
      event.dataTransfer.setData("application/x-ddse-hero", hero.id);
    });
    row.addEventListener("dragend", () => row.classList.remove("dragging"));
    row.addEventListener("dragover", (event) => {
      if (!event.dataTransfer.types.includes("application/x-ddse-hero")) return;
      event.preventDefault();
      event.dataTransfer.dropEffect = "move";
    });
    row.addEventListener("drop", async (event) => {
      const from = event.dataTransfer.getData("application/x-ddse-hero");
      if (!from) return;
      event.preventDefault();
      const order = [...elements.heroList.querySelectorAll(".hero-entry")].map((item) => item.dataset.heroId);
      const fromIndex = order.indexOf(from);
      const toIndex = order.indexOf(hero.id);
      if (fromIndex < 0 || toIndex < 0 || fromIndex === toIndex) return;
      order.splice(toIndex, 0, ...order.splice(fromIndex, 1));
      try { renderCampaign(await editorGateway.editHeroes("reorder", latestCampaign.revision, { heroIds: order })); }
      catch (error) { elements.townSaveState.textContent = displayError(error); }
    });
    elements.heroList.append(row);
  }
  if (elements.heroDetail.open) renderHeroDetail();

  document.querySelectorAll('.trinket-tooltip-portal[data-scope="inventory"]').forEach((tooltip) => tooltip.remove());
  elements.trinketGrid.replaceChildren();
  const inventoryPairs = pairTrinketInstances(campaign.trinkets || []);
  if (!campaign.trinkets?.length) {
    elements.trinketGrid.append(Object.assign(document.createElement("p"), { className: "town-data-message", textContent: t("town.noTrinkets") }));
  }
  for (const trinket of campaign.trinkets || []) {
    const item = document.createElement("div");
    item.className = "trinket-entry";
    item.dataset.trinketId = trinket.id || "";
    item.dataset.rawKey = trinket.rawKey || String(trinket.index);
    item.draggable = true;
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
    attachTrinketTooltip(item, trinketTooltip(trinket,
      possibleLinkedTrinketName(trinket, campaign.trinkets, inventoryPairs.links)));
    item.addEventListener("contextmenu", async (event) => {
      event.preventDefault();
      if (!event.shiftKey && !window.confirm(t("trinket.deleteOne", { name: cleanGameText(trinket.name || trinket.id) }))) return;
      try {
        renderCampaign(await editorGateway.editTrinkets("delete", latestCampaign.revision, { rawKey: item.dataset.rawKey }));
      } catch (error) { elements.townSaveState.textContent = displayError(error); }
    });
    item.addEventListener("dragstart", (event) => {
      item.classList.add("dragging");
      event.dataTransfer.effectAllowed = "move";
      event.dataTransfer.setData("text/plain", item.dataset.rawKey);
    });
    item.addEventListener("dragend", () => item.classList.remove("dragging"));
    item.addEventListener("dragover", (event) => { event.preventDefault(); event.dataTransfer.dropEffect = "move"; });
    item.addEventListener("drop", async (event) => {
      event.preventDefault();
      const from = event.dataTransfer.getData("text/plain");
      const current = [...elements.trinketGrid.querySelectorAll(".trinket-entry")].map((node) => node.dataset.rawKey);
      const fromIndex = current.indexOf(from);
      const toIndex = current.indexOf(item.dataset.rawKey);
      if (fromIndex < 0 || toIndex < 0 || fromIndex === toIndex) return;
      current.splice(toIndex, 0, ...current.splice(fromIndex, 1));
      try { renderCampaign(await editorGateway.editTrinkets("reorder", latestCampaign.revision, { rawKeys: current })); }
      catch (error) { elements.townSaveState.textContent = displayError(error); }
    });
    elements.trinketGrid.append(item);
  }
  elements.townUndo.disabled = !campaign.canUndo;
  elements.townRedo.disabled = !campaign.canRedo;
  elements.townSave.disabled = !campaign.dirty;
  elements.townSaveState.textContent = campaign.dirty ? t("town.unsaved") : t("town.clean");
  elements.townSaveState.title = "";
}

let townReloadDisabledStates = null;

function setTownReloading(reloading) {
  elements.townReloadOverlay.hidden = !reloading;
  elements.townStage.setAttribute("aria-busy", String(reloading));
  if (reloading) {
    townReloadDisabledStates = new Map();
    elements.townStage.querySelectorAll("button, input, select").forEach((control) => {
      townReloadDisabledStates.set(control, control.disabled);
      control.disabled = true;
    });
  } else {
    for (const [control, disabled] of townReloadDisabledStates || []) {
      if (control.isConnected) control.disabled = disabled;
    }
    townReloadDisabledStates = null;
    elements.townUndo.disabled = !latestCampaign?.canUndo;
    elements.townRedo.disabled = !latestCampaign?.canRedo;
    elements.townSave.disabled = !latestCampaign?.dirty;
  }
}

function trinketSearchRank(definition, query) {
  if (!query) return 0;
  const normalize = (value) => String(value ?? "").normalize("NFKC").toLocaleLowerCase().replace(/\s+/g, " ").trim();
  const input = query.trim();
  let matcher = null;
  const regexSyntax = input.match(/^\/(.*)\/([imsu]*)$/s);
  if (regexSyntax) {
    try { matcher = new RegExp(regexSyntax[1], regexSyntax[2]); }
    catch { return Number.MAX_SAFE_INTEGER; }
  }
  const needle = normalize(input);
  const fields = [definition.id, definition.localizationKey, definition.localizedName,
    definition.englishName, `${definition.description || ""} ${definition.englishDescription || ""}`,
    definition.effectSearchText || "",
    `${definition.modName || ""} ${definition.sourceId || ""}`,
    `${(definition.tags || []).join(" ")} ${(definition.heroClasses || []).join(" ")} ${Object.values(definition.heroClassNames || {}).join(" ")}`];
  const weights = [0, 1, 2, 3, 4, 4, 5, 6];
  const index = fields.findIndex((field) => matcher
    ? matcher.test(String(field ?? ""))
    : normalize(field).includes(needle));
  return index < 0 ? Number.MAX_SAFE_INTEGER : weights[index];
}

function renderTrinketSelector() {
  const query = elements.trinketSearch.value;
  const mod = elements.trinketModFilter.value;
  const heroClass = elements.trinketClassFilter.value;
  const rarity = elements.trinketRarityFilter.value;
  const inventory = latestCampaign?.trinkets || [];
  const isBatch = trinketSelectorMode === "batchAdd" || trinketSelectorMode === "batchDelete";
  const matchesFilters = (item, definition = item) => {
    if (mod && definition.sourceId !== mod) return false;
    if (heroClass && !(definition.heroClasses || []).includes(heroClass)) return false;
    if (rarity && trinketRarityId(definition) !== rarity) return false;
    return trinketSearchRank(item, query) !== Number.MAX_SAFE_INTEGER ||
      (definition !== item && trinketSearchRank(definition, query) !== Number.MAX_SAFE_INTEGER);
  };
  const candidates = trinketSelectorMode === "batchDelete"
    ? inventory.map((item) => ({ item, definition: trinketDefinitions.find((entry) => entry.id === item.id) }))
      .filter(({ item, definition }) => matchesFilters(item, definition || item))
    : trinketDefinitions.filter((definition) => matchesFilters(definition, definition))
      .sort((a, b) => trinketSearchRank(a, query) - trinketSearchRank(b, query) || a.name.localeCompare(b.name));
  document.querySelectorAll('.trinket-tooltip-portal[data-scope="selector"]').forEach((tooltip) => tooltip.remove());
  elements.trinketSelectorList.replaceChildren();
  const regexSyntax = query.trim().match(/^\/(.*)\/([imsu]*)$/s);
  if (regexSyntax) {
    try { new RegExp(regexSyntax[1], regexSyntax[2]); }
    catch {
      elements.trinketSelectorMessage.textContent = t("trinket.invalidRegex");
      return;
    }
  }
  elements.trinketSelectorMessage.textContent = t("trinket.results", { count: candidates.length });
  if (isBatch) {
    elements.trinketBatchHint.textContent = t(trinketSelectorMode === "batchAdd"
      ? "trinket.batchAddHint" : "trinket.batchDeleteHint");
    const selectedCount = trinketSelectorMode === "batchAdd" ? selectedTrinketIds.size : selectedTrinketRawKeys.size;
    elements.trinketSelectionCount.textContent = t("trinket.selectedCount", { count: selectedCount });
    const hasSelection = selectedCount > 0;
    const hasScopeFilters = Boolean(mod || heroClass || rarity);
    const actionLabel = trinketSelectorMode === "batchAdd"
      ? hasSelection ? "trinket.batchAddSelected" : "trinket.batchAddVisible"
      : hasSelection ? "trinket.batchDeleteSelected"
        : hasScopeFilters ? "trinket.batchDeleteVisible" : "trinket.batchDeleteAll";
    elements.trinketBatchConfirm.textContent = t(actionLabel);
  }
  const inventoryPairs = pairTrinketInstances(inventory);
  for (const candidate of candidates) {
    const item = trinketSelectorMode === "batchDelete" ? candidate.item : candidate;
    const definition = trinketSelectorMode === "batchDelete" ? candidate.definition || item : candidate;
    const selectionKey = trinketSelectorMode === "batchDelete" ? item.rawKey : definition.id;
    const selected = trinketSelectorMode === "batchDelete"
      ? selectedTrinketRawKeys.has(selectionKey) : selectedTrinketIds.has(selectionKey);
    const row = document.createElement("button");
    row.type = "button";
    row.className = `trinket-selector-item${selected ? " selected" : ""}`;
    row.setAttribute("aria-pressed", String(selected));
    const asset = chooseAsset(item.assets, ["trinket", "icon"]);
    if (asset) {
      const image = document.createElement("img");
      image.src = assetUrl(asset);
      image.alt = "";
      image.onerror = () => image.replaceWith(Object.assign(document.createElement("span"), {
        className: "missing-art item-fallback", textContent: t("trinket.missingArt"),
      }));
      row.append(image);
    } else row.append(Object.assign(document.createElement("span"), {
      className: "missing-art item-fallback", textContent: t("trinket.missingArt"),
    }));
    const info = document.createElement("span");
    info.className = "trinket-selector-info";
    const name = document.createElement("strong");
    name.textContent = item.name || item.id;
    const rarityLabel = trinketRarityLabel(item);
    if (rarityLabel) {
      const rarity = document.createElement("small");
      rarity.className = "trinket-selector-rarity";
      rarity.textContent = rarityLabel;
      info.append(name, rarity);
    } else info.append(name);
    const source = document.createElement("small");
    source.textContent = `${item.modName || definition.modName || t("trinket.vanilla")} · ${item.sourceId || definition.sourceId || "vanilla"}`;
    info.append(source);
    row.append(info);
    attachTrinketTooltip(row, trinketTooltip(item,
      possibleLinkedTrinketName(item, inventory, inventoryPairs.links)));
    row.addEventListener("click", async () => {
      if (isBatch) {
        const selectedSet = trinketSelectorMode === "batchDelete" ? selectedTrinketRawKeys : selectedTrinketIds;
        if (selectedSet.has(selectionKey)) selectedSet.delete(selectionKey);
        else selectedSet.add(selectionKey);
        renderTrinketSelector();
        return;
      }
      try {
        renderCampaign(await editorGateway.editTrinkets(trinketSelectorMode === "add" ? "add" : "delete", latestCampaign.revision,
          trinketSelectorMode === "add" ? { trinketId: definition.id } : { trinketId: definition.id }));
        if (trinketSelectorMode === "add") elements.trinketSelector.close();
      } catch (error) { elements.trinketSelectorMessage.textContent = displayError(error); }
    });
    if (trinketSelectorMode === "batchDelete") row.addEventListener("contextmenu", (event) => {
      event.preventDefault();
      selectedTrinketRawKeys.delete(selectionKey);
      renderTrinketSelector();
    });
    elements.trinketSelectorList.append(row);
  }
}

function renderHeroSelector() {
  const query = elements.heroSearch.value.trim().toLocaleLowerCase();
  const sourceId = elements.heroModFilter.value;
  const visible = heroClassDefinitions.filter((item) => {
    const searchable = `${item.id} ${item.name} ${item.modName} ${item.sourceId}`.toLocaleLowerCase();
    return (!sourceId || item.sourceId === sourceId) && (!query || searchable.includes(query));
  });
  elements.heroSelectorList.replaceChildren();
  for (const item of visible) {
    const row = document.createElement("button");
    row.type = "button";
    row.className = `trinket-selector-item${selectedHeroClassIds.has(item.id) ? " selected" : ""}`;
    if (item.portraitPath) {
      const image = document.createElement("img");
      image.alt = "";
      image.src = `/api/content-asset?path=${encodeURIComponent(item.portraitPath)}`;
      image.onerror = () => image.replaceWith(Object.assign(document.createElement("span"),
        { className: "missing-art item-fallback" }));
      row.append(image);
    } else row.append(Object.assign(document.createElement("span"), { className: "missing-art item-fallback" }));
    const info = document.createElement("span");
    info.className = "trinket-selector-info";
    info.append(Object.assign(document.createElement("strong"), { textContent: item.name || item.id }));
    info.append(Object.assign(document.createElement("small"), { textContent: `${item.modName || item.sourceId} · ${item.id}` }));
    row.append(info);
    row.addEventListener("click", () => {
      if (selectedHeroClassIds.has(item.id)) selectedHeroClassIds.delete(item.id);
      else selectedHeroClassIds.add(item.id);
      renderHeroSelector();
    });
    elements.heroSelectorList.append(row);
  }
  elements.heroSelectorMessage.textContent = heroClassDefinitions.length
    ? t("hero.results", { count: visible.length })
    : t("hero.noSafeTemplate");
  elements.heroSelectionCount.textContent = t("hero.selectedCount", { count: selectedHeroClassIds.size });
}

async function openHeroSelector() {
  selectedHeroClassIds.clear();
  elements.heroSearch.value = "";
  elements.heroModFilter.replaceChildren(new Option(t("hero.allMods"), ""));
  elements.heroSelectorList.replaceChildren();
  elements.heroSelectorMessage.textContent = t("hero.loading");
  if (!elements.heroSelector.open) elements.heroSelector.showModal();
  try {
    heroClassDefinitions = await editorGateway.listHeroClasses();
    const mods = new Map();
    for (const item of heroClassDefinitions) if (item.sourceId)
      mods.set(item.sourceId, item.modName || item.sourceId);
    for (const [id, name] of mods) elements.heroModFilter.add(new Option(name, id));
    renderHeroSelector();
  } catch (error) { elements.heroSelectorMessage.textContent = displayError(error); }
}

async function openTrinketSelector(mode) {
  trinketSelectorMode = mode;
  selectedTrinketIds.clear();
  selectedTrinketRawKeys.clear();
  elements.trinketSelectorTitle.textContent = t(`trinket.title.${mode}`);
  elements.trinketBatchFooter.hidden = mode !== "batchAdd" && mode !== "batchDelete";
  elements.trinketOnlyNew.parentElement.hidden = mode !== "batchAdd";
  elements.trinketClearSelection.hidden = mode !== "batchAdd" && mode !== "batchDelete";
  elements.trinketBatchConfirm.textContent = t(mode === "batchDelete" ? "trinket.batchDelete" : "trinket.batchAdd");
  elements.trinketSearch.value = "";
  elements.trinketModFilter.value = "";
  elements.trinketClassFilter.value = "";
  elements.trinketRarityFilter.value = "";
  elements.trinketSelectorList.replaceChildren();
  if (!elements.trinketSelector.open) elements.trinketSelector.showModal();
  try {
    if (!trinketDefinitions.length) {
      elements.trinketSelectorMessage.textContent = t("trinket.loading");
      trinketDefinitions = await editorGateway.listTrinkets();
    }
    const mods = new Map();
    const classes = new Map();
    const rarities = new Map();
    for (const item of trinketDefinitions) {
      if (item.sourceId) mods.set(item.sourceId, item.modName || item.sourceId);
      for (const id of item.heroClasses || []) classes.set(id, item.heroClassNames?.[id] || id);
      const rarityId = trinketRarityId(item);
      if (rarityId) rarities.set(rarityId, trinketRarityLabel(item));
    }
    elements.trinketModFilter.replaceChildren(new Option(t("trinket.allMods"), ""));
    for (const [id, name] of mods) elements.trinketModFilter.add(new Option(name, id));
    elements.trinketClassFilter.replaceChildren(new Option(t("trinket.allClasses"), ""));
    for (const [id, name] of classes) elements.trinketClassFilter.add(new Option(name, id));
    elements.trinketRarityFilter.replaceChildren(new Option(t("trinket.allRarities"), ""));
    for (const [id, name] of rarities) elements.trinketRarityFilter.add(new Option(name, id));
    renderTrinketSelector();
  } catch (error) { elements.trinketSelectorMessage.textContent = displayError(error); }
}

const BUILDING_LOCALE_KEYS = Object.freeze({
  camping_trainer: "town.survivalist", stage_coach: "town.stageCoach", tavern: "town.tavern",
  sanitarium: "town.sanitarium", abbey: "town.abbey", graveyard: "town.graveyard",
  nomad_wagon: "town.nomadWagon", guild: "town.guild", blacksmith: "town.blacksmith",
});

const UPGRADE_TITLE_KEYS = Object.freeze({
  "abbey.meditation": "town.upgrade.abbey.meditation", "abbey.prayer": "town.upgrade.abbey.prayer",
  "abbey.flagellation": "town.upgrade.abbey.flagellation", "blacksmith.weapon": "town.upgrade.blacksmith.weapon",
  "blacksmith.armour": "town.upgrade.blacksmith.armour", "blacksmith.cost": "town.upgrade.blacksmith.cost",
  "camping_trainer.cost": "town.upgrade.camping_trainer.cost", "guild.skill_levels": "town.upgrade.guild.skill_levels",
  "guild.cost": "town.upgrade.guild.cost", "nomad_wagon.numitems": "town.upgrade.nomad_wagon.numitems",
  "nomad_wagon.cost": "town.upgrade.nomad_wagon.cost", "sanitarium.cost": "town.upgrade.sanitarium.cost",
  "sanitarium.disease_quirk_cost": "town.upgrade.sanitarium.disease_quirk_cost",
  "sanitarium.slots": "town.upgrade.sanitarium.slots", "stage_coach.numrecruits": "town.upgrade.stage_coach.numrecruits",
  "stage_coach.rostersize": "town.upgrade.stage_coach.rostersize",
  "stage_coach.upgraded_recruits": "town.upgrade.stage_coach.upgraded_recruits",
  "tavern.bar": "town.upgrade.tavern.bar", "tavern.gambling": "town.upgrade.tavern.gambling",
  "tavern.brothel": "town.upgrade.tavern.brothel",
});

function renderBuildings(campaign) {
  elements.buildingList.replaceChildren();
  for (const building of campaign.buildings || []) {
    const row = document.createElement("button");
    row.type = "button";
    row.className = "building-row building-row-button";
    const label = document.createElement("span");
    label.textContent = t(BUILDING_LOCALE_KEYS[building.id] || "town.building");
    row.append(label);
    row.addEventListener("click", () => openBuildingEditor(building.id));
    elements.buildingList.append(row);
  }
  const districts = document.createElement("button");
  districts.type = "button";
  districts.className = "building-row building-row-button building-section-label";
  districts.append(Object.assign(document.createElement("span"), { textContent: t("town.areaBuildings") }));
  districts.addEventListener("click", openDistrictEditor);
  elements.buildingList.append(districts);
  elements.buildingMaximizeAll.disabled = !(campaign.buildings || []).some((building) =>
    (building.upgradeTrees || []).some((tree) => tree.rank < tree.maxRank));
}

function createTownAction(key, handler, className = "") {
  const button = document.createElement("button");
  button.type = "button";
  button.className = className;
  button.textContent = t(key);
  button.addEventListener("click", handler);
  return button;
}

function presentBuildingEditor(title, kicker, actions, hint, content) {
  elements.buildingEditorTitle.textContent = title;
  elements.buildingEditorKicker.textContent = kicker;
  elements.buildingEditorActions.replaceChildren(...actions);
  elements.buildingEditorHint.textContent = hint;
  elements.buildingEditorContent.replaceChildren(...content);
  if (!elements.buildingEditor.open) elements.buildingEditor.showModal();
}

function campaignBuilding(buildingId) {
  return latestCampaign?.buildings?.find((item) => item.id === buildingId);
}

function openBuildingEditor(buildingId) {
  const building = latestCampaign?.buildings?.find((item) => item.id === buildingId);
  if (!building) return;
  const title = t(BUILDING_LOCALE_KEYS[building.id] || "town.building");
  const actions = (building.upgradeTrees || []).length
    ? [createTownAction("town.maximizeThisBuilding", () => changeBuildingRank(buildingId, null, null, true))]
    : [];
  const content = [];
  for (const tree of building.upgradeTrees || []) {
    const card = document.createElement("section");
    card.className = "upgrade-tree";
    const heading = document.createElement("h3");
    heading.textContent = t(UPGRADE_TITLE_KEYS[tree.id] || "town.upgradeTree");
    const nodes = document.createElement("div");
    nodes.className = "upgrade-nodes";
    for (let index = 0; index < (tree.nodes || []).length; index += 1) {
      const node = tree.nodes[index];
      const button = document.createElement("button");
      button.type = "button";
      button.className = `upgrade-node${node.purchased ? " purchased" : ""}`;
      button.textContent = node.code.toUpperCase();
      const nodeInstructions = t("town.nodeControlsTip");
      button.title = [cleanGameText(node.description || ""), nodeInstructions].filter(Boolean).join("\n");
      button.setAttribute("aria-label", `${heading.textContent} ${node.code.toUpperCase()}`);
      button.addEventListener("click", () => changeBuildingRank(buildingId, tree.id, index + 1, false));
      button.addEventListener("contextmenu", (event) => {
        event.preventDefault();
        changeBuildingRank(buildingId, tree.id, index, false);
      });
      nodes.append(button);
    }
    card.append(heading, nodes);
    content.push(card);
  }
  if (!content.length) content.push(Object.assign(document.createElement("p"), { className: "town-data-message", textContent: t("town.noUpgradeTrees") }));
  presentBuildingEditor(title, t("town.buildingUpgrade"), actions, t("town.upgradeControls"), content);
}

async function changeBuildingRank(buildingId, treeId, rank, maximize) {
  if (!latestCampaign) return;
  if (!maximize) {
    const tree = campaignBuilding(buildingId)?.upgradeTrees?.find((item) => item.id === treeId);
    if (!tree || tree.rank === rank) return;
  }
  try {
    const updated = maximize
      ? await editorGateway.maximizeTownBuildings(latestCampaign.revision, buildingId)
      : await editorGateway.setTownBuildingRank(buildingId, treeId, rank, latestCampaign.revision);
    renderCampaign(updated);
    openBuildingEditor(buildingId);
  } catch (error) {
    const message = displayError(error);
    elements.townSaveState.textContent = message;
    elements.buildingEditorHint.textContent = message;
  }
}

function openDistrictEditor() {
  if (!latestCampaign) return;
  const systemOpen = latestCampaign.districtSystemOpen;
  const actions = [
    createTownAction("town.unlockAllDistricts", () => changeDistrict("unlock_all")),
    createTownAction("town.lockAllAndSystem", () => changeDistrict("system_lock"), "secondary"),
    createTownAction(systemOpen ? "town.lockDistrictSystem" : "town.unlockDistrictSystem",
      () => changeDistrict(systemOpen ? "system_lock" : "system_open"), "secondary"),
  ];
  actions[0].disabled = !systemOpen;
  actions[1].disabled = !systemOpen;
  const content = [];
  for (const district of latestCampaign.districts || []) {
    const row = document.createElement("button");
    row.type = "button";
    row.className = `district-entry${district.built ? " built" : ""}`;
    row.disabled = !district.editable;
    row.textContent = `${district.name} · ${district.built ? t("town.districtUnlocked") : t("town.districtLocked")}`;
    row.title = t("town.districtClickTip");
    row.addEventListener("click", () => changeDistrict("set", district.id, !district.built));
    content.push(row);
  }
  if (!content.length) content.push(Object.assign(document.createElement("p"), { className: "town-data-message", textContent: t("town.noDistricts") }));
  presentBuildingEditor(t("town.areaBuildings"), t("town.districtSystem"), actions,
    systemOpen ? t("town.districtControls") : t("town.districtSystemLocked"), content);
}

async function changeDistrict(action, districtId = undefined, built = undefined) {
  if (!latestCampaign) return;
  try {
    renderCampaign(await editorGateway.editDistrict(action, latestCampaign.revision, districtId, built));
    openDistrictEditor();
  } catch (error) { elements.townSaveState.textContent = displayError(error); }
}

async function loadCampaign() {
  const campaign = await editorGateway.getCampaign();
  renderCampaign(campaign);
}

function showConfiguration(config) {
  currentConfiguration = config;
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

async function loadInitialization({ preserveSettingsButton = settingsMode, showTownOnReuse = !settingsMode } = {}) {
  let state = await editorGateway.getInitialization();
  elements.initializationCard.hidden = false;
  elements.reinitializeMods.hidden = true;
  while (state.status === "running" || state.status === "queued") {
    if (!preserveSettingsButton) {
      elements.configurationSubmit.disabled = true;
      elements.configurationSubmit.dataset.mode = "running";
      elements.configurationSubmit.textContent = t("initialization.runningButton");
    }
    elements.reinitializeMods.disabled = true;
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
    if (preserveSettingsButton) {
      elements.configurationSubmit.disabled = false;
      elements.configurationSubmit.dataset.mode = "save";
      elements.configurationSubmit.textContent = t("settings.save");
    } else {
      elements.configurationSubmit.disabled = false;
      elements.configurationSubmit.dataset.mode = "complete";
      elements.configurationSubmit.textContent = t("initialization.continue");
    }
    elements.reinitializeMods.hidden = false;
    elements.reinitializeMods.disabled = false;
    if (state.reusedExisting && showTownOnReuse) showTownShell();
  } else if (state.status === "failed") {
    elements.initializationMessage.textContent = [...(state.diagnostics ?? []), t("initialization.failed")].join(" ");
    elements.configurationSubmit.disabled = false;
    elements.configurationSubmit.dataset.mode = "save";
    elements.configurationSubmit.textContent = t("settings.save");
    elements.reinitializeMods.hidden = true;
  }
  return state;
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
  const previousSaveRoot = currentConfiguration?.saveRoots?.[0] ?? "";
  const nextSaveRoot = elements.saveRoots.value.trim();
  const saveRootChanged = previousSaveRoot !== nextSaveRoot;
  if (settingsMode && saveRootChanged && latestCampaign?.dirty &&
      !window.confirm(t("settings.profileChangeDiscardDraft"))) return;
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
    trinketDefinitions = [];
    showConfiguration(configuration);
    await setLocale(configuration.language);
    elements.configurationMessage.textContent = t("settings.saved");
    if (settingsMode && saveRootChanged) {
      const state = await loadInitialization({ preserveSettingsButton: true, showTownOnReuse: false });
      if (state.status === "completed") showTownShell();
    } else if (!settingsMode) {
      await loadInitialization();
    }
  } catch (error) {
    elements.configurationSubmit.disabled = false;
    elements.configurationSubmit.dataset.mode = "save";
    elements.configurationSubmit.textContent = t("settings.save");
    elements.configurationMessage.textContent = displayError(error);
  }
});

elements.refreshProfiles.addEventListener("click", loadProfiles);
elements.buildingEditorClose.addEventListener("click", () => elements.buildingEditor.close());
elements.buildingMaximizeAll.addEventListener("click", async () => {
  if (!latestCampaign) return;
  elements.buildingMaximizeAll.disabled = true;
  try { renderCampaign(await editorGateway.maximizeTownBuildings(latestCampaign.revision)); }
  catch (error) { elements.townSaveState.textContent = displayError(error); }
});
elements.reinitializeMods.addEventListener("click", async () => {
  elements.reinitializeMods.disabled = true;
  trinketDefinitions = [];
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
  settingsMode = true;
  elements.townShell.hidden = true;
  elements.panel.hidden = false;
  elements.settingsReturnTown.hidden = false;
  elements.configurationSubmit.disabled = false;
  elements.configurationSubmit.dataset.mode = "save";
  elements.configurationSubmit.textContent = t("settings.save");
});
elements.settingsReturnTown.addEventListener("click", () => {
  if (!settingsMode) return;
  elements.settingsReturnTown.hidden = true;
  showTownShell();
});
elements.townReloadProfile.addEventListener("click", async () => {
  if (latestCampaign?.dirty && !window.confirm(t("town.reloadDiscardDraft"))) return;
  trinketDefinitions = [];
  if (elements.trinketSelector.open) elements.trinketSelector.close();
  if (elements.buildingEditor.open) elements.buildingEditor.close();
  setTownReloading(true);
  elements.townSaveState.textContent = t("town.reloadWorking");
  try {
    renderCampaign(await editorGateway.reloadCampaign());
    elements.townSaveState.textContent = t("town.reloadComplete");
  } catch (error) {
    elements.townSaveState.textContent = displayError(error);
  } finally {
    setTownReloading(false);
  }
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
elements.townSave.addEventListener("click", async () => {
  if (!latestCampaign?.dirty) return;
  elements.townSave.disabled = true;
  elements.townSaveState.textContent = t("town.saveWorking");
  try {
    const result = await editorGateway.saveCampaign();
    renderCampaign(result.campaign);
    elements.townSaveState.textContent = t("town.saveComplete");
    elements.townSaveState.title = result.backupDirectory || "";
  } catch (error) {
    elements.townSave.disabled = !latestCampaign?.dirty;
    elements.townSaveState.textContent = displayError(error);
  }
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

elements.heroAdd.addEventListener("click", openHeroSelector);
elements.heroDetailClose.addEventListener("click", () => elements.heroDetail.close());
elements.heroDetail.addEventListener("close", () => { activeHeroId = null; stopHeroIdle(); });
elements.heroDetailRename.addEventListener("click", () => {
  const hero = (latestCampaign?.heroes || []).find((entry) => entry.id === activeHeroId);
  if (hero && hero.nameEditable !== false) renameHero(hero);
});
elements.heroDetailTrinkets.addEventListener("wheel", (event) => {
  if (elements.heroDetailTrinkets.scrollWidth <= elements.heroDetailTrinkets.clientWidth || !event.deltaY) return;
  event.preventDefault();
  elements.heroDetailTrinkets.scrollLeft += event.deltaY;
}, { passive: false });
elements.heroSelectorClose.addEventListener("click", () => elements.heroSelector.close());
elements.heroSelector.addEventListener("close", () => { selectedHeroClassIds.clear(); });
elements.heroSearch.addEventListener("input", renderHeroSelector);
elements.heroModFilter.addEventListener("change", () => { selectedHeroClassIds.clear(); renderHeroSelector(); });
elements.heroClearSelection.addEventListener("click", () => { selectedHeroClassIds.clear(); renderHeroSelector(); });
elements.heroBatchConfirm.addEventListener("click", async () => {
  const query = elements.heroSearch.value.trim().toLocaleLowerCase();
  const sourceId = elements.heroModFilter.value;
  const visible = heroClassDefinitions.filter((item) => {
    const text = `${item.id} ${item.name} ${item.modName} ${item.sourceId}`.toLocaleLowerCase();
    return (!sourceId || item.sourceId === sourceId) && (!query || text.includes(query));
  });
  const selected = selectedHeroClassIds.size ? [...selectedHeroClassIds] : visible.map((item) => item.id);
  if (!selected.length) { elements.heroSelectorMessage.textContent = t("hero.noChanges"); return; }
  if (selected.length > 1 && !window.confirm(t("hero.confirmBatch", { count: selected.length }))) return;
  elements.heroBatchConfirm.disabled = true;
  elements.heroSelectorMessage.textContent = t("hero.working");
  try {
    renderCampaign(await editorGateway.editHeroes("add", latestCampaign.revision, { classIds: selected }));
    elements.heroSelector.close();
  } catch (error) { elements.heroSelectorMessage.textContent = displayError(error); }
  finally { elements.heroBatchConfirm.disabled = false; }
});
elements.trinketSort.addEventListener("click", async () => {
  if (!latestCampaign?.trinkets?.length) return;
  const desired = sortedTrinketInventory(latestCampaign.trinkets);
  const rawKeys = desired.map((item) => item.rawKey || String(item.index));
  const current = latestCampaign.trinkets.map((item) => item.rawKey || String(item.index));
  if (rawKeys.every((key, index) => key === current[index])) {
    elements.townSaveState.textContent = t("trinket.alreadySorted");
    return;
  }
  elements.trinketSort.disabled = true;
  try {
    renderCampaign(await editorGateway.editTrinkets("reorder", latestCampaign.revision, { rawKeys }));
  } catch (error) {
    elements.townSaveState.textContent = displayError(error);
  } finally {
    elements.trinketSort.disabled = false;
  }
});
elements.trinketBatchAdd.addEventListener("click", () => openTrinketSelector("batchAdd"));
elements.trinketBatchDelete.addEventListener("click", () => openTrinketSelector("batchDelete"));
elements.trinketSelectorClose.addEventListener("click", () => elements.trinketSelector.close());
elements.trinketSelector.addEventListener("close", () => {
  document.querySelectorAll('.trinket-tooltip-portal[data-scope="selector"]').forEach((tooltip) => tooltip.remove());
});
elements.trinketSearch.addEventListener("input", renderTrinketSelector);
for (const control of [elements.trinketModFilter, elements.trinketClassFilter, elements.trinketRarityFilter]) {
  control.addEventListener("change", () => {
    selectedTrinketIds.clear();
    selectedTrinketRawKeys.clear();
    renderTrinketSelector();
  });
}
elements.trinketClearSelection.addEventListener("click", () => {
  selectedTrinketIds.clear();
  selectedTrinketRawKeys.clear();
  renderTrinketSelector();
});
elements.trinketBatchConfirm.addEventListener("click", async () => {
  const action = trinketSelectorMode === "batchAdd" ? "batch_add" : "batch_delete";
  const onlyNew = elements.trinketOnlyNew.checked;
  const query = elements.trinketSearch.value;
  const modId = elements.trinketModFilter.value;
  const heroClass = elements.trinketClassFilter.value;
  const rarityId = elements.trinketRarityFilter.value;
  const visibleDefinitions = trinketDefinitions.filter((item) =>
    (!modId || item.sourceId === modId) && (!heroClass || item.heroClasses?.includes(heroClass)) &&
    (!rarityId || trinketRarityId(item) === rarityId) &&
    trinketSearchRank(item, query) !== Number.MAX_SAFE_INTEGER);
  const visibleInventory = (latestCampaign?.trinkets || []).filter((entry) => {
    const def = trinketDefinitions.find((item) => item.id === entry.id) || entry;
    return (!modId || def.sourceId === modId) && (!heroClass || def.heroClasses?.includes(heroClass)) &&
      (!rarityId || trinketRarityId(def) === rarityId) &&
      (trinketSearchRank(entry, query) !== Number.MAX_SAFE_INTEGER ||
        (def !== entry && trinketSearchRank(def, query) !== Number.MAX_SAFE_INTEGER));
  });
  const trinketIds = trinketSelectorMode === "batchAdd"
    ? (selectedTrinketIds.size ? [...selectedTrinketIds] : visibleDefinitions.map((item) => item.id)) : null;
  const filteredInventoryMode = Boolean(modId || heroClass || rarityId);
  const rawKeys = trinketSelectorMode === "batchDelete"
    ? (selectedTrinketRawKeys.size ? [...selectedTrinketRawKeys]
      : filteredInventoryMode ? visibleInventory.map((item) => item.rawKey) : null) : null;
  const estimatedAddCount = trinketSelectorMode === "batchAdd"
    ? trinketIds.filter((id) => !onlyNew || !(latestCampaign?.trinkets || []).some((entry) => entry.id === id)).length : 0;
  const estimate = trinketSelectorMode === "batchAdd" ? estimatedAddCount
    : rawKeys ? rawKeys.length : (latestCampaign?.trinkets || []).length;
  if (!estimate) { elements.trinketSelectorMessage.textContent = t("trinket.noChanges"); return; }
  if (estimate > 1 && !window.confirm(t("trinket.confirmBatch", { count: estimate }))) return;
  elements.trinketBatchConfirm.disabled = true;
  elements.trinketSelectorMessage.textContent = t("trinket.working");
  try {
    const payload = { onlyNew };
    if (trinketSelectorMode === "batchAdd") payload.trinketIds = trinketIds;
    else if (rawKeys) payload.rawKeys = rawKeys;
    const campaign = await editorGateway.editTrinkets(action, latestCampaign.revision, payload);
    renderCampaign(campaign);
    const resultCount = trinketSelectorMode === "batchAdd" ? campaign.trinketAdded : campaign.trinketDeleted;
    elements.townSaveState.textContent = t("trinket.batchResult", { count: resultCount ?? estimate });
    elements.trinketSelector.close();
  } catch (error) { elements.trinketSelectorMessage.textContent = displayError(error); }
  finally { elements.trinketBatchConfirm.disabled = false; }
});

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
