#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

#include "Portal.h"

static constexpr size_t METER_MAX_MODEL_ROWS = 10;
static constexpr size_t METER_MAX_CHART_POINTS = 32;

struct MeterModelUsage {
  String model = "";
  double includedRequests = 0;
  double billedRequests = 0;
  double grossAmountUsd = 0;
  double billedAmountUsd = 0;
};

struct MeterChartPoint {
  uint32_t timestampMs = 0;
  double grossUsd = 0;
  double discountUsd = 0;
  double billedUsd = 0;
};

struct MeterUsage {
  bool ok = false;
  String plan = "pro";
  String resetDate = "";
  String resetDateUtc = "";
  String lastError = "";
  double subscriptionUsd = 0;
  double totalUsageUsd = 0;
  double includedAllowanceUsd = 0;
  double includedCoveredUsd = 0;
  double includedUsageConsumedUsd = 0;
  double remainingIncludedUsd = 0;
  double overageUsd = 0;
  double totalMonthlySpendUsd = 0;
  double usedCredits = 0;
  double usageValueCredits = 0;
  double aiCreditsUsed = 0;
  double aiCreditsIncluded = 0;
  double aiCreditsRemaining = 0;
  double aiCreditsOverage = 0;
  double aiCreditsPercent = 0;
  double includedCredits = 0;
  double includedUsageConsumedCredits = 0;
  double overageCredits = 0;
  double remainingIncludedCredits = 0;
  double includedUsagePercent = 0;
  bool overagesEnabled = false;
  int premiumInteractionsLimit = 0;
  int premiumInteractionsRemaining = 0;
  uint16_t httpCode = 0;
  uint32_t deviceUpdatedMs = 0;
  size_t modelCount = 0;
  MeterModelUsage models[METER_MAX_MODEL_ROWS];
  size_t chartCount = 0;
  MeterChartPoint chart[METER_MAX_CHART_POINTS];
};

static bool meterApiApplyUsageDocument(MeterUsage &usage, JsonDocument &doc, uint16_t code) {
  usage.httpCode = code;
  usage.ok = doc["ok"] | false;
  usage.lastError = String(doc["lastError"] | "");
  usage.modelCount = 0;
  usage.chartCount = 0;
  usage.includedCoveredUsd = 0.0;

  JsonObject meter = doc["meter"].as<JsonObject>();
  if (meter.isNull()) {
    if (usage.lastError.isEmpty()) {
      usage.lastError = "no meter";
    }
    return usage.ok;
  }

  usage.plan = String(meter["plan"] | "pro");
  usage.resetDate = String(meter["resetDate"] | "");
  usage.resetDateUtc = String(meter["resetDateUtc"] | "");
  usage.subscriptionUsd = meter["subscriptionUsd"] | 0.0;
  usage.totalUsageUsd = meter["totalUsageUsd"] | 0.0;
  usage.includedAllowanceUsd = meter["includedAllowanceUsd"] | 0.0;
  usage.includedCoveredUsd = meter["includedCoveredUsd"] | usage.includedAllowanceUsd;
  usage.includedUsageConsumedUsd = meter["includedUsageConsumedUsd"] | usage.includedCoveredUsd;
  usage.remainingIncludedUsd = meter["remainingIncludedUsd"] | 0.0;
  usage.overageUsd = meter["overageUsd"] | 0.0;
  usage.totalMonthlySpendUsd = meter["totalMonthlySpendUsd"] | 0.0;
  usage.usedCredits = meter["usedCredits"] | 0.0;
  usage.includedCredits = meter["includedCredits"] | 0.0;
  usage.includedUsageConsumedCredits = meter["includedUsageConsumedCredits"] | 0.0;
  usage.overageCredits = meter["overageCredits"] | 0.0;
  usage.remainingIncludedCredits = meter["remainingIncludedCredits"] | 0.0;
  usage.includedUsagePercent = meter["includedUsagePercent"] | 0.0;
  usage.usageValueCredits = meter["usageValueCredits"] | usage.usedCredits;
  usage.aiCreditsUsed = meter["aiCreditsUsed"] | usage.includedUsageConsumedCredits;
  usage.aiCreditsIncluded = meter["aiCreditsIncluded"] | usage.includedCredits;
  usage.aiCreditsRemaining = meter["aiCreditsRemaining"] | usage.remainingIncludedCredits;
  usage.aiCreditsOverage = meter["aiCreditsOverage"] | usage.overageCredits;
  usage.aiCreditsPercent = meter["aiCreditsPercent"] | usage.includedUsagePercent;
  usage.overagesEnabled = meter["overagesEnabled"] | false;
  usage.premiumInteractionsLimit = meter["premiumInteractionsLimit"] | 0;
  usage.premiumInteractionsRemaining = meter["premiumInteractionsRemaining"] | 0;

  JsonArray modelArray = meter["premiumRequests"]["models"].as<JsonArray>();
  if (!modelArray.isNull()) {
    for (JsonObject model : modelArray) {
      if (usage.modelCount >= METER_MAX_MODEL_ROWS) {
        break;
      }
      MeterModelUsage &row = usage.models[usage.modelCount++];
      row.model = String(model["model"] | "");
      row.includedRequests = model["includedRequests"] | 0.0;
      row.billedRequests = model["billedRequests"] | 0.0;
      row.grossAmountUsd = model["grossAmountUsd"] | 0.0;
      row.billedAmountUsd = model["billedAmountUsd"] | 0.0;
    }
  }

  JsonArray chartArray = meter["chart"].as<JsonArray>();
  if (!chartArray.isNull()) {
    for (JsonObject point : chartArray) {
      if (usage.chartCount >= METER_MAX_CHART_POINTS) {
        break;
      }
      MeterChartPoint &entry = usage.chart[usage.chartCount++];
      entry.timestampMs = point["timestampMs"] | 0;
      entry.grossUsd = point["grossUsd"] | 0.0;
      entry.discountUsd = point["discountUsd"] | 0.0;
      entry.billedUsd = point["billedUsd"] | 0.0;
    }
  }

  usage.deviceUpdatedMs = millis();
  return usage.ok;
}

static bool meterApiGet(const char *path, String &bodyOut, uint16_t &httpCodeOut) {
  char url[192];
  if (!meterBuildUrl(url, sizeof(url), path)) {
    return false;
  }

  HTTPClient http;
  http.begin(url);
  http.setTimeout(8000);
  int code = http.GET();
  bodyOut = http.getString();
  http.end();

  if (code <= 0) {
    return false;
  }

  httpCodeOut = static_cast<uint16_t>(code);
  return true;
}

static bool meterApiPost(const char *path, const char *body, String &bodyOut, uint16_t &httpCodeOut) {
  char url[192];
  if (!meterBuildUrl(url, sizeof(url), path)) {
    return false;
  }

  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(8000);
  int code = http.POST(body ? body : "{}");
  bodyOut = http.getString();
  http.end();

  if (code <= 0) {
    return false;
  }

  httpCodeOut = static_cast<uint16_t>(code);
  return true;
}

static bool meterApiFetchUsage(MeterUsage &usage) {
  String body;
  uint16_t code = 0;
  if (!meterApiGet("/api/usage", body, code)) {
    usage.ok = false;
    usage.httpCode = 0;
    usage.lastError = "helper offline";
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    usage.ok = false;
    usage.httpCode = code;
    usage.lastError = "bad json";
    return false;
  }
  return meterApiApplyUsageDocument(usage, doc, code);
}

static bool meterApiTriggerRefresh(MeterUsage &usage) {
  String body;
  uint16_t code = 0;
  if (!meterApiPost("/api/refresh", "{}", body, code)) {
    usage.ok = false;
    usage.httpCode = 0;
    usage.lastError = "refresh fail";
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    usage.ok = false;
    usage.httpCode = code;
    usage.lastError = "bad refresh json";
    return false;
  }
  return meterApiApplyUsageDocument(usage, doc, code);
}
