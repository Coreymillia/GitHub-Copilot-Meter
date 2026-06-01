const http = require("http");
const fs = require("fs");
const path = require("path");
const { URL } = require("url");

const APP_DIR = __dirname;
const ROOT_DIR = path.resolve(APP_DIR, "..");
const DATA_DIR = path.join(ROOT_DIR, "data");
const SETTINGS_PATH = path.join(DATA_DIR, "settings.json");

const DEFAULT_SETTINGS = {
  bindHost: "0.0.0.0",
  bindPort: 8787,
  pollIntervalSec: 60,
  planOverride: "auto",
  githubCookie: "",
  extraHeadersText: "",
  usageChartUrl: "https://github.com/settings/billing/usage_chart?group=0&period=3&product=&query=",
  entitlementUrl: "https://github.com/github-copilot/chat/entitlement",
  premiumUsageTableUrl: "",
};

const PLAN_CREDITS = {
  pro: 1500,
  "pro+": 7000,
  max: 20000,
};

const PLAN_SUBSCRIPTION_USD = {
  pro: 10,
  "pro+": 39,
  max: 100,
};

let settings = loadSettings();
let refreshTimer = null;
let usageState = {
  ok: false,
  lastUpdated: 0,
  lastError: "Configure GitHub auth to begin polling.",
  meter: null,
  sources: {
    usageChart: null,
    entitlement: null,
    premiumUsageTable: null,
  },
};

function ensureDataDir() {
  fs.mkdirSync(DATA_DIR, { recursive: true });
}

function readJson(filePath, fallback) {
  try {
    return JSON.parse(fs.readFileSync(filePath, "utf8"));
  } catch {
    return fallback;
  }
}

function writeJson(filePath, value) {
  ensureDataDir();
  fs.writeFileSync(filePath, JSON.stringify(value, null, 2));
}

function clampInt(value, fallback, minimum, maximum) {
  const parsed = Number.parseInt(String(value ?? ""), 10);
  if (!Number.isFinite(parsed)) {
    return fallback;
  }
  return Math.min(maximum, Math.max(minimum, parsed));
}

function normalizePlanOverride(value) {
  const normalized = String(value || "").trim().toLowerCase();
  if (["auto", "pro", "pro+", "max"].includes(normalized)) {
    return normalized;
  }
  return "auto";
}

function normalizeDetectedPlan(value) {
  const normalized = String(value || "")
    .trim()
    .toLowerCase()
    .replace(/[\s_-]+/g, "");
  if (!normalized) {
    return "";
  }
  if (normalized === "pro") {
    return "pro";
  }
  if (normalized === "pro+" || normalized === "proplus" || normalized === "copilotpro+") {
    return "pro+";
  }
  if (normalized === "max" || normalized === "copilotmax") {
    return "max";
  }
  return "";
}

function normalizeCookieValue(value) {
  let normalized = String(value || "").trim();
  if (!normalized) {
    return "";
  }
  normalized = normalized.replace(/^cookie\s*:\s*/i, "");
  normalized = normalized.replace(/^-b\s*/i, "");
  normalized = normalized.replace(/^['"]|['"]$/g, "");
  normalized = normalized
    .split(/\r?\n/)
    .map((part) => part.trim())
    .filter(Boolean)
    .join(" ");
  return normalized.replace(/\s{2,}/g, " ").trim();
}

function normalizeSettings(input) {
  return {
    bindHost: String(input?.bindHost || DEFAULT_SETTINGS.bindHost).trim() || DEFAULT_SETTINGS.bindHost,
    bindPort: clampInt(input?.bindPort, DEFAULT_SETTINGS.bindPort, 1, 65535),
    pollIntervalSec: clampInt(input?.pollIntervalSec, DEFAULT_SETTINGS.pollIntervalSec, 15, 3600),
    planOverride: normalizePlanOverride(input?.planOverride),
    githubCookie: normalizeCookieValue(input?.githubCookie),
    extraHeadersText: String(input?.extraHeadersText || "").trim(),
    usageChartUrl: String(input?.usageChartUrl || DEFAULT_SETTINGS.usageChartUrl).trim() || DEFAULT_SETTINGS.usageChartUrl,
    entitlementUrl: String(input?.entitlementUrl || DEFAULT_SETTINGS.entitlementUrl).trim() || DEFAULT_SETTINGS.entitlementUrl,
    premiumUsageTableUrl: String(input?.premiumUsageTableUrl || DEFAULT_SETTINGS.premiumUsageTableUrl).trim(),
  };
}

function loadSettings() {
  return normalizeSettings(readJson(SETTINGS_PATH, DEFAULT_SETTINGS));
}

function saveSettings(nextSettings) {
  settings = normalizeSettings(nextSettings);
  writeJson(SETTINGS_PATH, settings);
  scheduleRefresh();
}

function getPublicSettings() {
  return {
    bindHost: settings.bindHost,
    bindPort: settings.bindPort,
    pollIntervalSec: settings.pollIntervalSec,
    planOverride: settings.planOverride,
    usageChartUrl: settings.usageChartUrl,
    entitlementUrl: settings.entitlementUrl,
    premiumUsageTableUrl: settings.premiumUsageTableUrl,
    githubCookieConfigured: Boolean(settings.githubCookie),
    extraHeadersConfigured: Boolean(settings.extraHeadersText),
  };
}

function parseExtraHeaders(extraHeadersText) {
  const headers = {};
  for (const line of String(extraHeadersText || "").split(/\r?\n/)) {
    const trimmed = line.trim();
    if (!trimmed) {
      continue;
    }
    const separatorIndex = trimmed.indexOf(":");
    if (separatorIndex <= 0) {
      continue;
    }
    const name = trimmed.slice(0, separatorIndex).trim();
    const value = trimmed.slice(separatorIndex + 1).trim();
    if (!name || !value) {
      continue;
    }
    headers[name] = value;
  }
  return headers;
}

function buildGitHubHeaders() {
  const headers = {
    accept: "application/json",
    "x-requested-with": "XMLHttpRequest",
    "user-agent": "GHCPMeter/0.1",
    ...parseExtraHeaders(settings.extraHeadersText),
  };
  if (settings.githubCookie) {
    headers.cookie = settings.githubCookie;
  }
  return headers;
}

function parseMoneyValue(value) {
  if (typeof value === "number") {
    return value;
  }
  const normalized = String(value || "").replace(/[^0-9.-]/g, "");
  const parsed = Number.parseFloat(normalized);
  return Number.isFinite(parsed) ? parsed : 0;
}

function roundTo(value, places = 2) {
  const factor = Math.pow(10, places);
  return Math.round((Number(value) || 0) * factor) / factor;
}

function parseNumberValue(value) {
  if (typeof value === "number") {
    return value;
  }
  const normalized = String(value || "").replace(/[^0-9.-]/g, "");
  const parsed = Number.parseFloat(normalized);
  return Number.isFinite(parsed) ? parsed : 0;
}

function normalizeColumnName(value) {
  return String(value || "")
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "");
}

function getPremiumUsageTableRoot(premiumUsagePayload) {
  if (premiumUsagePayload?.table && typeof premiumUsagePayload.table === "object") {
    return premiumUsagePayload.table;
  }
  return premiumUsagePayload;
}

function buildPremiumAnalytics(premiumUsagePayload) {
  const tablePayload = getPremiumUsageTableRoot(premiumUsagePayload);
  const columns = Array.isArray(tablePayload?.columns) ? tablePayload.columns : [];
  const rows = Array.isArray(tablePayload?.rows) ? tablePayload.rows : [];
  if (!columns.length || !rows.length) {
    return {
      columns: [],
      models: [],
      totals: {
        modelCount: 0,
        includedRequests: 0,
        billedRequests: 0,
        grossAmountUsd: 0,
        billedAmountUsd: 0,
      },
      page: premiumUsagePayload?.page ?? null,
      numPages: premiumUsagePayload?.numPages ?? null,
      totalResources: premiumUsagePayload?.totalResources ?? null,
    };
  }

  const columnNames = columns.map((column) => String(column?.name || "").trim());
  const columnIndexByName = new Map(columnNames.map((name, index) => [normalizeColumnName(name), index]));

  const models = rows.map((row) => {
    const cells = Array.isArray(row?.cells) ? row.cells : [];
    const getCellValue = (columnName) => {
      const index = columnIndexByName.get(columnName);
      if (index === undefined) {
        return "";
      }
      return String(cells[index]?.value || "").trim();
    };

    return {
      model: getCellValue("model") || String(row?.id || "").trim() || "Unknown",
      includedRequests: roundTo(parseNumberValue(getCellValue("included-requests")), 2),
      billedRequests: roundTo(parseNumberValue(getCellValue("billed-requests")), 2),
      grossAmountUsd: roundTo(parseMoneyValue(getCellValue("gross-amount"))),
      billedAmountUsd: roundTo(parseMoneyValue(getCellValue("billed-amount"))),
    };
  });

  const totals = models.reduce(
    (summary, model) => {
      summary.includedRequests += model.includedRequests;
      summary.billedRequests += model.billedRequests;
      summary.grossAmountUsd += model.grossAmountUsd;
      summary.billedAmountUsd += model.billedAmountUsd;
      return summary;
    },
    {
      modelCount: models.length,
      includedRequests: 0,
      billedRequests: 0,
      grossAmountUsd: 0,
      billedAmountUsd: 0,
    },
  );

  totals.includedRequests = roundTo(totals.includedRequests, 2);
  totals.billedRequests = roundTo(totals.billedRequests, 2);
  totals.grossAmountUsd = roundTo(totals.grossAmountUsd);
  totals.billedAmountUsd = roundTo(totals.billedAmountUsd);

  return {
    columns: columnNames,
    models,
    totals,
    page: premiumUsagePayload?.page ?? null,
    numPages: premiumUsagePayload?.numPages ?? null,
    totalResources: premiumUsagePayload?.totalResources ?? null,
  };
}

async function fetchGitHubJson(requestUrl) {
  const response = await fetch(requestUrl, {
    headers: buildGitHubHeaders(),
  });
  const bodyText = await response.text();
  let payload = null;
  try {
    payload = JSON.parse(bodyText);
  } catch {
    payload = null;
  }
  if (!response.ok) {
    const message =
      (payload && (payload.error || payload.message)) ||
      bodyText.slice(0, 400) ||
      `GitHub request failed with HTTP ${response.status}`;
    throw new Error(message);
  }
  if (!payload || typeof payload !== "object") {
    throw new Error("GitHub did not return JSON.");
  }
  return payload;
}

function resolvePlan(entitlementPlan, premiumInteractionsLimit) {
  const override = normalizePlanOverride(settings.planOverride);
  if (override !== "auto") {
    return override;
  }

  const detectedPlan = normalizeDetectedPlan(entitlementPlan);
  if (detectedPlan) {
    return detectedPlan;
  }

  const numericLimit = parseNumberValue(premiumInteractionsLimit);
  if (numericLimit >= PLAN_CREDITS.max) {
    return "max";
  }
  if (numericLimit >= PLAN_CREDITS["pro+"]) {
    return "pro+";
  }
  if (numericLimit >= PLAN_CREDITS.pro) {
    return "pro";
  }
  return "pro";
}

function buildMeter(usageChartPayload, entitlementPayload) {
  const usageSeries = Array.isArray(usageChartPayload?.usage) ? usageChartPayload.usage : [];
  const usagePoints = Array.isArray(usageSeries[0]?.data) ? usageSeries[0].data : [];
  let usedUsd = 0;
  let includedCoveredUsd = 0;
  let totalAmountUsd = 0;

  const chart = usagePoints.map((point) => {
    const grossUsd = parseMoneyValue(point?.custom?.grossAmount ?? point?.y);
    const discountUsd = parseMoneyValue(point?.custom?.discountAmount);
    const billedUsd = parseMoneyValue(point?.custom?.totalAmount);
    usedUsd += grossUsd;
    includedCoveredUsd += discountUsd;
    totalAmountUsd += billedUsd;
    return {
      timestampMs: Number.parseInt(String(point?.x || "0"), 10) || 0,
      grossUsd: roundTo(grossUsd),
      discountUsd: roundTo(discountUsd),
      billedUsd: roundTo(billedUsd),
    };
  });

  const entitlementPlan = String(entitlementPayload?.plan || "").trim().toLowerCase();
  const premiumInteractionsLimit = roundTo(Math.max(0, parseNumberValue(entitlementPayload?.quotas?.limits?.premiumInteractions)), 1);
  const premiumInteractionsRemaining = roundTo(
    Math.max(0, parseNumberValue(entitlementPayload?.quotas?.remaining?.premiumInteractions)),
    1,
  );
  const resolvedPlan = resolvePlan(entitlementPlan, premiumInteractionsLimit);
  const includedCredits = premiumInteractionsLimit || PLAN_CREDITS[resolvedPlan] || PLAN_CREDITS.pro;
  const includedAllowanceUsd = roundTo(includedCredits / 100);
  const subscriptionUsd = roundTo(PLAN_SUBSCRIPTION_USD[resolvedPlan] || 0);
  const totalUsageUsd = roundTo(usedUsd);
  const premiumInteractionsUsed =
    premiumInteractionsLimit > 0 ? roundTo(Math.max(0, premiumInteractionsLimit - premiumInteractionsRemaining), 1) : null;
  const includedUsageConsumedCredits = premiumInteractionsUsed ?? roundTo(Math.min(includedAllowanceUsd, includedCoveredUsd || 0) * 100, 1);
  const includedUsageConsumedUsd = roundTo(includedUsageConsumedCredits / 100);
  const billedOverageUsd = roundTo(Math.max(0, totalAmountUsd));
  const overageUsd = roundTo(
    billedOverageUsd > 0 ? billedOverageUsd : Math.max(0, totalUsageUsd - includedUsageConsumedUsd),
  );
  const usedCredits = roundTo(totalUsageUsd * 100, 1);
  const overageCredits = roundTo(overageUsd * 100, 1);
  const remainingIncludedCredits =
    premiumInteractionsLimit > 0
      ? premiumInteractionsRemaining
      : Math.max(0, roundTo(includedCredits - includedUsageConsumedCredits, 1));
  const remainingIncludedUsd = roundTo(remainingIncludedCredits / 100);
  const includedUsagePercent =
    includedCredits > 0
      ? Math.min(100, roundTo((includedUsageConsumedCredits / includedCredits) * 100, 1))
      : 0;
  const effectivePercentUsed =
    includedCredits > 0 ? Math.min(100, roundTo((usedCredits / includedCredits) * 100, 1)) : 0;
  const aiCreditsUsed = includedUsageConsumedCredits;
  const aiCreditsRemaining = remainingIncludedCredits;
  const aiCreditsPercent =
    includedCredits > 0 ? Math.min(100, roundTo((aiCreditsUsed / includedCredits) * 100, 1)) : includedUsagePercent;

  return {
    plan: resolvedPlan,
    entitlementPlan: entitlementPlan || null,
    licenseType: entitlementPayload?.licenseType || null,
    subscriptionUsd,
    basePlanCostUsd: subscriptionUsd,
    totalUsageUsd,
    currentMeteredUsageUsd: totalUsageUsd,
    usedUsd: totalUsageUsd,
    includedAllowanceUsd,
    includedUsd: includedAllowanceUsd,
    includedUsageConsumedUsd,
    currentIncludedUsageUsd: includedUsageConsumedUsd,
    remainingIncludedUsd,
    remainingUsd: remainingIncludedUsd,
    overageUsd,
    billedOverageUsd,
    currentBilledOverageUsd: billedOverageUsd,
    includedCoveredUsd: roundTo(includedCoveredUsd),
    billedUsdFromChart: billedOverageUsd,
    usedCredits,
    usageValueCredits: usedCredits,
    aiCreditsUsed,
    includedCredits,
    aiCreditsIncluded: includedCredits,
    includedUsageConsumedCredits,
    aiCreditsIncludedUsed: includedUsageConsumedCredits,
    overageCredits,
    aiCreditsOverage: overageCredits,
    remainingCredits: remainingIncludedCredits,
    remainingIncludedCredits,
    aiCreditsRemaining,
    includedUsagePercent: aiCreditsPercent,
    aiCreditsPercent,
    percentUsed: effectivePercentUsed,
    totalMonthlySpendUsd: roundTo(subscriptionUsd + overageUsd),
    resetDate: entitlementPayload?.quotas?.resetDate || null,
    resetDateUtc: entitlementPayload?.quotas?.resetDateUtc || null,
    overagesEnabled: Boolean(entitlementPayload?.quotas?.overagesEnabled),
    premiumInteractionsUsed,
    premiumInteractionsLimit: premiumInteractionsLimit || null,
    premiumInteractionsRemaining: premiumInteractionsLimit > 0 ? premiumInteractionsRemaining : null,
    chart,
  };
}

async function refreshUsage() {
  if (!settings.githubCookie) {
    usageState = {
      ...usageState,
      ok: false,
      lastError: "Add a GitHub session cookie in settings.",
      lastUpdated: 0,
      meter: null,
      sources: {
        ...usageState.sources,
        premiumUsageTable: settings.premiumUsageTableUrl
          ? {
              configured: true,
              ok: false,
              error: "Add a GitHub session cookie in settings.",
            }
          : {
              configured: false,
              ok: false,
              error: "",
            },
      },
    };
    return usageState;
  }

  try {
    const [usageChartPayload, entitlementPayload] = await Promise.all([
      fetchGitHubJson(settings.usageChartUrl),
      fetchGitHubJson(settings.entitlementUrl),
    ]);
    let premiumAnalytics = null;
    let premiumUsageSource = settings.premiumUsageTableUrl
      ? {
          configured: true,
          ok: false,
          error: "Premium usage table has not been fetched yet.",
        }
      : {
          configured: false,
          ok: false,
          error: "",
        };

    if (settings.premiumUsageTableUrl) {
      try {
        const premiumUsagePayload = await fetchGitHubJson(settings.premiumUsageTableUrl);
        premiumAnalytics = buildPremiumAnalytics(premiumUsagePayload);
        premiumUsageSource = {
          configured: true,
          ok: true,
          rowCount: premiumAnalytics.models.length,
          page: premiumAnalytics.page,
          numPages: premiumAnalytics.numPages,
          totalResources: premiumAnalytics.totalResources,
          error: "",
        };
      } catch (error) {
        premiumUsageSource = {
          configured: true,
          ok: false,
          error: error instanceof Error ? error.message : String(error),
        };
      }
    }

    const meter = buildMeter(usageChartPayload, entitlementPayload);
    meter.premiumRequests = premiumAnalytics;

    usageState = {
      ok: true,
      lastUpdated: Date.now(),
      lastError: "",
      meter,
      sources: {
        usageChart: {
          seriesCount: Array.isArray(usageChartPayload.usage) ? usageChartPayload.usage.length : 0,
          pointCount: Array.isArray(usageChartPayload?.usage?.[0]?.data) ? usageChartPayload.usage[0].data.length : 0,
        },
        entitlement: {
          plan: entitlementPayload.plan || null,
          licenseType: entitlementPayload.licenseType || null,
        },
        premiumUsageTable: premiumUsageSource,
      },
    };
    return usageState;
  } catch (error) {
    usageState = {
      ...usageState,
      ok: false,
      lastError: error instanceof Error ? error.message : String(error),
    };
    return usageState;
  }
}

function scheduleRefresh() {
  if (refreshTimer) {
    clearInterval(refreshTimer);
    refreshTimer = null;
  }
  refreshTimer = setInterval(() => {
    refreshUsage().catch(() => {});
  }, settings.pollIntervalSec * 1000);
}

function sendJson(response, statusCode, body) {
  response.writeHead(statusCode, {
    "content-type": "application/json; charset=utf-8",
    "cache-control": "no-store",
  });
  response.end(JSON.stringify(body, null, 2));
}

function readBody(request) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    request.on("data", (chunk) => {
      chunks.push(chunk);
      const size = chunks.reduce((sum, entry) => sum + entry.length, 0);
      if (size > 1024 * 1024) {
        reject(new Error("Request body too large."));
      }
    });
    request.on("end", () => resolve(Buffer.concat(chunks).toString("utf8")));
    request.on("error", reject);
  });
}

function buildPageHtml() {
  return `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>GHCPMeter</title>
  <style>
    body{font-family:Arial,sans-serif;background:#0b1018;color:#e8f3ff;margin:0;padding:24px;}
    h1,h2{margin:0 0 12px;}
    .wrap{max-width:1100px;margin:0 auto;display:grid;gap:18px;}
    .card{background:#111a24;border:1px solid #2b4258;border-radius:12px;padding:18px;}
    .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;}
    .metric{background:#0d141d;border-radius:10px;padding:12px;border:1px solid #223444;}
    .label{font-size:12px;color:#9fb7cc;text-transform:uppercase;letter-spacing:.08em;}
    .value{font-size:28px;font-weight:bold;margin-top:4px;}
    .bar{height:18px;background:#1a2633;border-radius:999px;overflow:hidden;border:1px solid #314b63;}
    .bar > span{display:block;height:100%;background:linear-gradient(90deg,#3bcf8e,#ffd166,#ff6b6b);}
    label{display:block;margin:12px 0 6px;font-weight:bold;color:#cbe6ff;}
    input,select,textarea,button{width:100%;box-sizing:border-box;border-radius:8px;border:1px solid #37516b;background:#0b1018;color:#eef7ff;padding:10px;}
    textarea{min-height:120px;resize:vertical;font-family:monospace;}
    button{background:#1b77ff;border:none;font-weight:bold;cursor:pointer;}
    button.secondary{background:#233243;}
    .row{display:grid;grid-template-columns:1fr 1fr;gap:12px;}
    .hint,.error,.ok{font-size:14px;}
    .hint{color:#9fb7cc;}
    .error{color:#ff9a9a;}
    .ok{color:#8ff1b2;}
    pre{white-space:pre-wrap;word-break:break-word;background:#0b1018;border:1px solid #2b4258;border-radius:10px;padding:12px;}
    .tableWrap{overflow:auto;border:1px solid #2b4258;border-radius:10px;}
    table{width:100%;border-collapse:collapse;background:#0b1018;}
    th,td{padding:10px 12px;border-bottom:1px solid #223444;text-align:left;white-space:nowrap;}
    th{font-size:12px;color:#9fb7cc;text-transform:uppercase;letter-spacing:.08em;background:#101924;}
    td.num{text-align:right;}
    @media (max-width:700px){.row{grid-template-columns:1fr;}}
  </style>
</head>
<body>
  <div class="wrap">
    <div class="card">
      <h1>GHCPMeter</h1>
      <p class="hint">Local GitHub Copilot meter helper for Pi/browser dashboards and future CYD clients.</p>
    </div>
    <div class="card">
      <h2>Meter</h2>
      <div id="status" class="hint">Loading…</div>
      <div class="grid" id="metrics"></div>
      <div class="label" style="margin-top:14px;">Usage</div>
      <div class="bar"><span id="usageBar" style="width:0%"></span></div>
      <pre id="rawMeter">Waiting for data…</pre>
    </div>
    <div class="card">
      <h2>Premium request analytics</h2>
      <div id="premiumStatus" class="hint">Loading…</div>
      <div class="grid" id="premiumMetrics"></div>
      <div id="premiumTable"></div>
    </div>
    <div class="card">
      <h2>Settings</h2>
      <p class="hint">Paste auth locally here. Leave secret fields blank to keep the current saved value. The same GitHub cookie works for the premium analytics table too.</p>
      <form id="settingsForm">
        <div class="row">
          <div>
            <label for="planOverride">Plan</label>
            <select id="planOverride" name="planOverride">
              <option value="auto">Auto-detect</option>
              <option value="pro">Pro</option>
              <option value="pro+">Pro+</option>
              <option value="max">Max</option>
            </select>
          </div>
          <div>
            <label for="pollIntervalSec">Poll interval (seconds)</label>
            <input id="pollIntervalSec" name="pollIntervalSec" type="number" min="15" max="3600">
          </div>
        </div>
        <label for="githubCookie">GitHub cookie header value</label>
        <textarea id="githubCookie" name="githubCookie" placeholder="user_session=...; __Host-user_session_same_site=...; _gh_sess=..."></textarea>
        <label for="extraHeadersText">Optional extra headers</label>
        <textarea id="extraHeadersText" name="extraHeadersText" placeholder="x-fetch-nonce: ...&#10;x-github-client-version: ..."></textarea>
        <div class="row">
          <div>
            <label for="usageChartUrl">Usage chart URL</label>
            <input id="usageChartUrl" name="usageChartUrl" type="text">
          </div>
          <div>
            <label for="entitlementUrl">Entitlement URL</label>
            <input id="entitlementUrl" name="entitlementUrl" type="text">
          </div>
        </div>
        <label for="premiumUsageTableUrl">Premium usage table URL</label>
        <input id="premiumUsageTableUrl" name="premiumUsageTableUrl" type="text" placeholder="https://github.com/settings/billing/copilot_usage_table?...">
        <div class="row" style="margin-top:14px;">
          <button type="submit">Save settings</button>
          <button class="secondary" type="button" id="refreshBtn">Refresh now</button>
        </div>
      </form>
      <div id="saveStatus" class="hint" style="margin-top:10px;"></div>
    </div>
  </div>
  <script>
    function formatUsd(value) {
      return '$' + Number(value || 0).toFixed(2);
    }

    function formatCount(value) {
      const numeric = Number(value || 0);
      if (!Number.isFinite(numeric)) {
        return '0';
      }
      if (Math.abs(numeric % 1) < 0.001) {
        return String(Math.round(numeric));
      }
      return numeric.toFixed(2).replace(/\\.00$/, '').replace(/(\\.\\d)0$/, '$1');
    }

    function formatPlanLabel(value) {
      const normalized = String(value || '').trim().toLowerCase();
      if (normalized === 'pro+') {
        return 'Pro+';
      }
      if (normalized === 'pro') {
        return 'Pro';
      }
      if (normalized === 'max') {
        return 'Max';
      }
      return value || '—';
    }

    function escapeHtml(value) {
      return String(value || '')
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#39;');
    }

    async function loadState() {
      const response = await fetch('/api/state', { cache: 'no-store' });
      const state = await response.json();
      renderState(state);
    }

    function renderState(state) {
      const status = document.getElementById('status');
      const metrics = document.getElementById('metrics');
      const rawMeter = document.getElementById('rawMeter');
      const usageBar = document.getElementById('usageBar');
      const premiumStatus = document.getElementById('premiumStatus');
      const premiumMetrics = document.getElementById('premiumMetrics');
      const premiumTable = document.getElementById('premiumTable');

      const meter = state.usage?.meter;
      if (!state.usage?.ok || !meter) {
        status.className = 'error';
        status.textContent = state.usage?.lastError || 'No data yet.';
        metrics.innerHTML = '';
        rawMeter.textContent = JSON.stringify(state.usage || {}, null, 2);
        usageBar.style.width = '0%';
        premiumStatus.className = 'hint';
        premiumStatus.textContent = 'Premium analytics will appear here after the helper loads successfully.';
        premiumMetrics.innerHTML = '';
        premiumTable.innerHTML = '';
      } else {
        status.className = 'ok';
        status.textContent = 'Updated ' + new Date(state.usage.lastUpdated).toLocaleString();
        const quotaPercent = Math.min(100, Number((meter.aiCreditsPercent ?? meter.includedUsagePercent) || 0));
        usageBar.style.width = quotaPercent + '%';
        metrics.innerHTML = [
              ['Plan', formatPlanLabel(meter.plan)],
              ['Base plan cost', formatUsd(meter.basePlanCostUsd ?? meter.subscriptionUsd)],
              ['Overage', formatUsd(meter.overageUsd)],
              ['Usage value', formatUsd(meter.totalUsageUsd)],
              ['AI credits used', formatCount(meter.aiCreditsUsed ?? meter.includedUsageConsumedCredits)],
              ['AI credits left', formatCount(meter.aiCreditsRemaining ?? meter.remainingIncludedCredits)],
              ['AI credit cap', formatCount(meter.aiCreditsIncluded ?? meter.includedCredits)],
              ['Usage %', String(quotaPercent.toFixed(1)) + '%'],
              ['Reset', meter.resetDateUtc || meter.resetDate || '—']
            ]
          .map(([label, value]) => '<div class="metric"><div class="label">' + label + '</div><div class="value">' + value + '</div></div>').join('');
        rawMeter.textContent = JSON.stringify(meter, null, 2);

        const premiumRequests = meter.premiumRequests;
        const premiumSource = state.usage?.sources?.premiumUsageTable;
        if (!premiumSource?.configured) {
          premiumStatus.className = 'hint';
          premiumStatus.textContent = 'Add a Premium usage table URL in settings to enable per-model premium request analytics.';
          premiumMetrics.innerHTML = '';
          premiumTable.innerHTML = '';
        } else if (!premiumSource.ok || !premiumRequests) {
          premiumStatus.className = 'error';
          premiumStatus.textContent = premiumSource.error || 'Premium usage table request failed.';
          premiumMetrics.innerHTML = '';
          premiumTable.innerHTML = '';
        } else {
          premiumStatus.className = premiumRequests.models.length ? 'ok' : 'hint';
          premiumStatus.textContent = premiumRequests.models.length
            ? 'Loaded ' + premiumRequests.models.length + ' model rows.'
            : 'Premium usage table returned no model rows.';
          premiumMetrics.innerHTML = [
            ['Models', formatCount(premiumRequests.totals.modelCount)],
            ['Included requests', formatCount(premiumRequests.totals.includedRequests)],
            ['Billed requests', formatCount(premiumRequests.totals.billedRequests)],
            ['Gross amount', formatUsd(premiumRequests.totals.grossAmountUsd)],
            ['Billed amount', formatUsd(premiumRequests.totals.billedAmountUsd)],
            ['Premium remaining', meter.premiumInteractionsRemaining == null ? '—' : formatCount(meter.premiumInteractionsRemaining)],
            ['Premium limit', meter.premiumInteractionsLimit == null ? '—' : formatCount(meter.premiumInteractionsLimit)]
          ].map(([label, value]) => '<div class="metric"><div class="label">' + label + '</div><div class="value">' + value + '</div></div>').join('');
          premiumTable.innerHTML = premiumRequests.models.length
            ? '<div class="tableWrap"><table><thead><tr><th>Model</th><th>Included</th><th>Billed</th><th>Gross</th><th>Billed amount</th></tr></thead><tbody>' +
              premiumRequests.models.map((model) =>
                '<tr>' +
                '<td>' + escapeHtml(model.model) + '</td>' +
                '<td class="num">' + formatCount(model.includedRequests) + '</td>' +
                '<td class="num">' + formatCount(model.billedRequests) + '</td>' +
                '<td class="num">' + formatUsd(model.grossAmountUsd) + '</td>' +
                '<td class="num">' + formatUsd(model.billedAmountUsd) + '</td>' +
                '</tr>'
              ).join('') +
              '</tbody></table></div>'
            : '';
        }
      }

      document.getElementById('planOverride').value = state.settings.planOverride;
      document.getElementById('pollIntervalSec').value = state.settings.pollIntervalSec;
      document.getElementById('usageChartUrl').value = state.settings.usageChartUrl;
      document.getElementById('entitlementUrl').value = state.settings.entitlementUrl;
      document.getElementById('premiumUsageTableUrl').value = state.settings.premiumUsageTableUrl || '';
      document.getElementById('githubCookie').value = '';
      document.getElementById('extraHeadersText').value = '';
      document.getElementById('saveStatus').textContent =
        'Cookie saved: ' + (state.settings.githubCookieConfigured ? 'yes' : 'no') +
        ' | Extra headers saved: ' + (state.settings.extraHeadersConfigured ? 'yes' : 'no');
    }

    async function saveSettings(event) {
      event.preventDefault();
      const body = {
        planOverride: document.getElementById('planOverride').value,
        pollIntervalSec: document.getElementById('pollIntervalSec').value,
        usageChartUrl: document.getElementById('usageChartUrl').value,
        entitlementUrl: document.getElementById('entitlementUrl').value,
        premiumUsageTableUrl: document.getElementById('premiumUsageTableUrl').value
      };
      const githubCookie = document.getElementById('githubCookie').value.trim();
      const extraHeadersText = document.getElementById('extraHeadersText').value.trim();
      if (githubCookie) body.githubCookie = githubCookie;
      if (extraHeadersText) body.extraHeadersText = extraHeadersText;
      const response = await fetch('/api/settings', {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify(body)
      });
      const payload = await response.json();
      document.getElementById('saveStatus').textContent = payload.ok ? 'Saved.' : (payload.error || 'Save failed.');
      await loadState();
    }

    async function refreshNow() {
      await fetch('/api/refresh', { method: 'POST' });
      await loadState();
    }

    document.getElementById('settingsForm').addEventListener('submit', saveSettings);
    document.getElementById('refreshBtn').addEventListener('click', refreshNow);
    loadState().catch((error) => {
      document.getElementById('status').className = 'error';
      document.getElementById('status').textContent = error.message;
    });
    setInterval(() => loadState().catch(() => {}), 15000);
  </script>
</body>
</html>`;
}

async function handleRequest(request, response) {
  const requestUrl = new URL(request.url, `http://${request.headers.host || "localhost"}`);

  if (request.method === "GET" && requestUrl.pathname === "/") {
    response.writeHead(200, { "content-type": "text/html; charset=utf-8" });
    response.end(buildPageHtml());
    return;
  }

  if (request.method === "GET" && requestUrl.pathname === "/api/state") {
    sendJson(response, 200, {
      ok: true,
      settings: getPublicSettings(),
      usage: usageState,
    });
    return;
  }

  if (request.method === "GET" && requestUrl.pathname === "/api/usage") {
    sendJson(response, usageState.ok ? 200 : 503, usageState);
    return;
  }

  if (request.method === "POST" && requestUrl.pathname === "/api/settings") {
    try {
      const rawBody = await readBody(request);
      const body = rawBody ? JSON.parse(rawBody) : {};
      const nextSettings = {
        ...settings,
        ...body,
        githubCookie: body.githubCookie !== undefined ? String(body.githubCookie).trim() : settings.githubCookie,
        extraHeadersText: body.extraHeadersText !== undefined ? String(body.extraHeadersText).trim() : settings.extraHeadersText,
      };
      saveSettings(nextSettings);
      await refreshUsage();
      sendJson(response, 200, { ok: true });
    } catch (error) {
      sendJson(response, 400, {
        ok: false,
        error: error instanceof Error ? error.message : "Failed to save settings.",
      });
    }
    return;
  }

  if (request.method === "POST" && requestUrl.pathname === "/api/refresh") {
    const nextState = await refreshUsage();
    sendJson(response, nextState.ok ? 200 : 503, nextState);
    return;
  }

  sendJson(response, 404, { ok: false, error: "Not found." });
}

async function main() {
  ensureDataDir();
  scheduleRefresh();
  await refreshUsage();
  const server = http.createServer((request, response) => {
    handleRequest(request, response).catch((error) => {
      sendJson(response, 500, {
        ok: false,
        error: error instanceof Error ? error.message : "Internal server error.",
      });
    });
  });
  server.listen(settings.bindPort, settings.bindHost, () => {
    console.log(`GHCPMeter listening on http://${settings.bindHost}:${settings.bindPort}`);
  });
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
