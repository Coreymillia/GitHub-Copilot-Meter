#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

#include "Portal.h"

struct MeterUsage {
  bool ok = false;
  String plan = "pro";
  String resetDate = "";
  String resetDateUtc = "";
  String lastError = "";
  double subscriptionUsd = 0;
  double totalUsageUsd = 0;
  double includedAllowanceUsd = 0;
  double includedUsageConsumedUsd = 0;
  double remainingIncludedUsd = 0;
  double overageUsd = 0;
  double totalMonthlySpendUsd = 0;
  double usedCredits = 0;
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
};

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

  usage.httpCode = code;
  usage.ok = doc["ok"] | false;
  usage.lastError = String(doc["lastError"] | "");

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
  usage.includedUsageConsumedUsd = meter["includedUsageConsumedUsd"] | 0.0;
  usage.remainingIncludedUsd = meter["remainingIncludedUsd"] | 0.0;
  usage.overageUsd = meter["overageUsd"] | 0.0;
  usage.totalMonthlySpendUsd = meter["totalMonthlySpendUsd"] | 0.0;
  usage.usedCredits = meter["usedCredits"] | 0.0;
  usage.includedCredits = meter["includedCredits"] | 0.0;
  usage.includedUsageConsumedCredits = meter["includedUsageConsumedCredits"] | 0.0;
  usage.overageCredits = meter["overageCredits"] | 0.0;
  usage.remainingIncludedCredits = meter["remainingIncludedCredits"] | 0.0;
  usage.includedUsagePercent = meter["includedUsagePercent"] | 0.0;
  usage.overagesEnabled = meter["overagesEnabled"] | false;
  usage.premiumInteractionsLimit = meter["premiumInteractionsLimit"] | 0;
  usage.premiumInteractionsRemaining = meter["premiumInteractionsRemaining"] | 0;
  usage.deviceUpdatedMs = millis();
  return usage.ok;
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

  usage.httpCode = code;
  usage.ok = doc["ok"] | false;
  usage.lastError = String(doc["lastError"] | "");

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
  usage.includedUsageConsumedUsd = meter["includedUsageConsumedUsd"] | 0.0;
  usage.remainingIncludedUsd = meter["remainingIncludedUsd"] | 0.0;
  usage.overageUsd = meter["overageUsd"] | 0.0;
  usage.totalMonthlySpendUsd = meter["totalMonthlySpendUsd"] | 0.0;
  usage.usedCredits = meter["usedCredits"] | 0.0;
  usage.includedCredits = meter["includedCredits"] | 0.0;
  usage.includedUsageConsumedCredits = meter["includedUsageConsumedCredits"] | 0.0;
  usage.overageCredits = meter["overageCredits"] | 0.0;
  usage.remainingIncludedCredits = meter["remainingIncludedCredits"] | 0.0;
  usage.includedUsagePercent = meter["includedUsagePercent"] | 0.0;
  usage.overagesEnabled = meter["overagesEnabled"] | false;
  usage.premiumInteractionsLimit = meter["premiumInteractionsLimit"] | 0;
  usage.premiumInteractionsRemaining = meter["premiumInteractionsRemaining"] | 0;
  usage.deviceUpdatedMs = millis();
  return usage.ok;
}
