#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

extern Arduino_GFX *gfx;

static const uint16_t METER_DEFAULT_PORT = 8787;
static const uint16_t METER_DEFAULT_POLL_SEC = 60;

static char meter_wifi_ssid[64] = "";
static char meter_wifi_pass[64] = "";
static char meter_host[64] = "";
static uint16_t meter_port = METER_DEFAULT_PORT;
static uint16_t meter_poll_sec = METER_DEFAULT_POLL_SEC;
static uint8_t meter_brightness = 220;
static bool meter_has_settings = false;
static unsigned long meter_last_wifi_retry_ms = 0;
static const unsigned long METER_WIFI_RETRY_INTERVAL_MS = 10000;

static WebServer *meter_portal_server = nullptr;
static DNSServer *meter_portal_dns = nullptr;

static void meterLoadSettings() {
  Preferences prefs;
  prefs.begin("ghcpmeter", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  String host = prefs.getString("host", "");
  meter_port = static_cast<uint16_t>(prefs.getUInt("port", METER_DEFAULT_PORT));
  meter_poll_sec = static_cast<uint16_t>(prefs.getUInt("poll", METER_DEFAULT_POLL_SEC));
  meter_brightness = prefs.getUChar("bright", 220);
  prefs.end();

  ssid.toCharArray(meter_wifi_ssid, sizeof(meter_wifi_ssid));
  pass.toCharArray(meter_wifi_pass, sizeof(meter_wifi_pass));
  host.toCharArray(meter_host, sizeof(meter_host));

  if (meter_port == 0) {
    meter_port = METER_DEFAULT_PORT;
  }
  if (meter_poll_sec < 15 || meter_poll_sec > 3600) {
    meter_poll_sec = METER_DEFAULT_POLL_SEC;
  }
  if (meter_brightness < 10) {
    meter_brightness = 10;
  }
  meter_has_settings = meter_wifi_ssid[0] != '\0' && meter_host[0] != '\0';
}

static void meterSaveSettings(
  const char *ssid,
  const char *pass,
  const char *host,
  uint16_t port,
  uint16_t pollSec,
  uint8_t brightness
) {
  Preferences prefs;
  prefs.begin("ghcpmeter", false);
  prefs.putString("ssid", ssid ? ssid : "");
  prefs.putString("pass", pass ? pass : "");
  prefs.putString("host", host ? host : "");
  prefs.putUInt("port", port);
  prefs.putUInt("poll", pollSec);
  prefs.putUChar("bright", brightness);
  prefs.end();

  strlcpy(meter_wifi_ssid, ssid ? ssid : "", sizeof(meter_wifi_ssid));
  strlcpy(meter_wifi_pass, pass ? pass : "", sizeof(meter_wifi_pass));
  strlcpy(meter_host, host ? host : "", sizeof(meter_host));
  meter_port = port;
  meter_poll_sec = pollSec;
  meter_brightness = brightness;
  meter_has_settings = meter_wifi_ssid[0] != '\0' && meter_host[0] != '\0';
}

static bool meterBuildUrl(char *buffer, size_t bufferSize, const char *path) {
  if (!buffer || bufferSize == 0 || !path || !meter_host[0]) {
    return false;
  }
  snprintf(buffer, bufferSize, "http://%s:%u%s", meter_host, meter_port, path);
  return true;
}

static bool meterProbeApiReachable(uint8_t attempts = 3, uint16_t delayMs = 1200) {
  char url[192];
  if (!meterBuildUrl(url, sizeof(url), "/api/state")) {
    return false;
  }

  for (uint8_t attempt = 0; attempt < attempts; attempt++) {
    HTTPClient http;
    http.begin(url);
    http.setTimeout(3000);
    int code = http.GET();
    http.end();
    if (code == 200) {
      return true;
    }
    delay(delayMs);
  }
  return false;
}

static void meterShowPortalScreen() {
  if (!gfx) {
    return;
  }

  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(0x07FF);
  gfx->setTextSize(2);
  gfx->setCursor(22, 14);
  gfx->print("GHCPMeter CYD");

  gfx->setTextSize(1);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(14, 54);
  gfx->print("1. Join WiFi: GHCPMeterCYD-Setup");
  gfx->setCursor(14, 70);
  gfx->print("2. Open: http://192.168.4.1");
  gfx->setCursor(14, 86);
  gfx->print("3. Save WiFi + helper address");

  gfx->setTextColor(0xFFE0);
  gfx->setCursor(14, 122);
  gfx->print("Helper host:");
  gfx->setTextColor(0x07E0);
  gfx->setCursor(88, 122);
  gfx->print(meter_host[0] ? meter_host : "<unset>");

  char portBuf[16];
  snprintf(portBuf, sizeof(portBuf), "%u", meter_port);
  gfx->setTextColor(0xFFE0);
  gfx->setCursor(14, 138);
  gfx->print("Port:");
  gfx->setTextColor(0x07E0);
  gfx->setCursor(88, 138);
  gfx->print(portBuf);

  char pollBuf[16];
  snprintf(pollBuf, sizeof(pollBuf), "%us", meter_poll_sec);
  gfx->setTextColor(0xFFE0);
  gfx->setCursor(14, 154);
  gfx->print("Poll:");
  gfx->setTextColor(0x07E0);
  gfx->setCursor(88, 154);
  gfx->print(pollBuf);

  if (meter_has_settings) {
    gfx->setTextColor(0x07E0);
    gfx->setCursor(14, 190);
    gfx->print("Saved settings loaded.");
  }
}

static void meterHandlePortalRoot() {
  String html =
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>GHCPMeter CYD Setup</title>"
    "<style>"
    "body{background:#081018;color:#8ff;font-family:Arial,sans-serif;max-width:520px;margin:auto;padding:20px;}"
    "h1{color:#0ff;}label{display:block;margin-top:14px;color:#9cf;font-weight:bold;}"
    "input{width:100%;box-sizing:border-box;padding:10px;border-radius:6px;border:1px solid #1d5f7a;background:#102030;color:#dff;}"
    "button{width:100%;padding:14px;margin-top:18px;border:none;border-radius:8px;background:#0d6efd;color:#fff;font-weight:bold;}"
    ".hint{font-size:.9em;color:#9ab;}"
    ".rng{display:flex;align-items:center;gap:8px;margin-top:8px;}"
    ".rng input[type=range]{flex:1;accent-color:#0ff;}"
    "</style></head><body>"
    "<h1>GHCPMeter CYD Setup</h1>"
    "<p class='hint'>Point this display at the local GHCPMeter Node helper.</p>"
    "<form method='post' action='/save'>"
    "<label>WiFi SSID</label><input name='ssid' value='" + String(meter_wifi_ssid) + "' maxlength='63' required>"
    "<label>WiFi Password</label><input name='pass' type='password' value='" + String(meter_wifi_pass) + "' maxlength='63'>"
    "<label>Helper Host / IP</label><input name='host' value='" + String(meter_host) + "' maxlength='63' required>"
    "<label>Helper Port</label><input name='port' type='number' value='" + String(meter_port) + "' min='1' max='65535' required>"
    "<label>Poll Interval (seconds)</label><input name='poll' type='number' value='" + String(meter_poll_sec) + "' min='15' max='3600' required>"
    "<label>Brightness</label>"
    "<div class='rng'><input type='range' name='bright' min='10' max='255' value='" + String(meter_brightness) +
    "' oninput='this.nextElementSibling.value=this.value'><output>" + String(meter_brightness) + "</output></div>"
    "<button type='submit'>Save & Connect</button>"
    "</form></body></html>";
  meter_portal_server->send(200, "text/html", html);
}

static void meterHandlePortalSave() {
  String ssid = meter_portal_server->arg("ssid");
  String pass = meter_portal_server->arg("pass");
  String host = meter_portal_server->arg("host");
  uint16_t port = static_cast<uint16_t>(meter_portal_server->arg("port").toInt());
  uint16_t pollSec = static_cast<uint16_t>(meter_portal_server->arg("poll").toInt());
  uint8_t brightness = static_cast<uint8_t>(meter_portal_server->arg("bright").toInt());

  if (port == 0) {
    port = METER_DEFAULT_PORT;
  }
  if (pollSec < 15 || pollSec > 3600) {
    pollSec = METER_DEFAULT_POLL_SEC;
  }
  if (brightness < 10) {
    brightness = 10;
  }

  meterSaveSettings(ssid.c_str(), pass.c_str(), host.c_str(), port, pollSec, brightness);
  meter_portal_server->send(
    200,
    "text/html",
    "<html><body style='background:#081018;color:#8ff;font-family:Arial;padding:24px'>"
    "<h2>Saved. Rebooting...</h2></body></html>"
  );
  delay(1200);
  ESP.restart();
}

static void meterRunPortal() {
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP("GHCPMeterCYD-Setup");

  if (!meter_portal_dns) {
    meter_portal_dns = new DNSServer();
  }
  if (!meter_portal_server) {
    meter_portal_server = new WebServer(80);
  }

  meter_portal_dns->start(53, "*", WiFi.softAPIP());
  meter_portal_server->on("/", meterHandlePortalRoot);
  meter_portal_server->on("/save", HTTP_POST, meterHandlePortalSave);
  meter_portal_server->onNotFound(meterHandlePortalRoot);
  meter_portal_server->begin();

  meterShowPortalScreen();

  while (true) {
    meter_portal_dns->processNextRequest();
    meter_portal_server->handleClient();
    delay(2);
  }
}

static void meterOpenSetupPortal() {
  meterRunPortal();
}

static void meterStartWifiStation() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(meter_wifi_ssid, meter_wifi_pass);
  meter_last_wifi_retry_ms = millis();
}

static bool meterEnsureWifiConnected(bool forceRetry = false) {
  if (!meter_has_settings || meter_wifi_ssid[0] == '\0') {
    return false;
  }
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }
  unsigned long now = millis();
  if (forceRetry || meter_last_wifi_retry_ms == 0 || now - meter_last_wifi_retry_ms >= METER_WIFI_RETRY_INTERVAL_MS) {
    WiFi.disconnect(false, true);
    delay(100);
    meterStartWifiStation();
  }
  return WiFi.status() == WL_CONNECTED;
}

static bool meterConnect(bool forcePortal = false) {
  meterLoadSettings();
  if (forcePortal || !meter_has_settings) {
    meterRunPortal();
  }

  meterStartWifiStation();
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
  }
  meterProbeApiReachable();
  return WiFi.status() == WL_CONNECTED;
}
