#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <Arduino_GFX_Library.h>
#include <XPT2046_Touchscreen.h>

#include "GHCPMeterApi.h"
#include "Portal.h"

#define GFX_BL 21
#define BOOT_BTN 0

Arduino_DataBus *bus = new Arduino_HWSPI(2, 15, 14, 13, 12);
Arduino_GFX *gfx = new Arduino_ILI9341(bus, GFX_NOT_DEFINED, 1);

#define XPT2046_IRQ 36
#define XPT2046_CS 33
#define XPT2046_CLK 25
#define XPT2046_MOSI 32
#define XPT2046_MISO 39

SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

static const uint16_t COLOR_BG = RGB565_BLACK;
static const uint16_t COLOR_PANEL = 0x0841;
static const uint16_t COLOR_HEADER = 0x0016;
static const uint16_t COLOR_HEADER_TEXT = 0x07FF;
static const uint16_t COLOR_TEXT = RGB565_WHITE;
static const uint16_t COLOR_DIM = 0x7BEF;
static const uint16_t COLOR_OK = RGB565_GREEN;
static const uint16_t COLOR_WARN = 0xFFE0;
static const uint16_t COLOR_ERROR = RGB565_RED;
static const uint16_t COLOR_ACTION = 0x01CF;
static const uint16_t COLOR_ACTION_ALT = 0x02A0;
static const uint16_t COLOR_SETUP = 0x780F;
static const uint16_t COLOR_OVERAGE = 0xF800;

static constexpr unsigned long TOUCH_DEBOUNCE_MS = 220;
static constexpr unsigned long BOOT_PORTAL_HOLD_MS = 3000;
static constexpr uint32_t WIFI_RETRY_DRAW_MS = 2000;

enum class MeterPage : uint8_t {
  Summary = 0,
  Quota,
  Models,
  Graph,
};

struct TouchButton {
  const char *label;
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
  uint16_t bg;
};

static const TouchButton buttonRefresh = {"REFRESH", 10, 210, 92, 24, COLOR_ACTION};
static const TouchButton buttonSync = {"SYNC NOW", 114, 210, 92, 24, COLOR_ACTION_ALT};
static const TouchButton buttonSetup = {"SETUP", 218, 210, 92, 24, COLOR_SETUP};

static MeterUsage meterUsage;
static MeterPage currentPage = MeterPage::Summary;
static bool renderDirty = true;
static bool touchWasDown = false;
static unsigned long lastTouchMs = 0;
static unsigned long lastBootHoldStartMs = 0;
static unsigned long lastPollMs = 0;
static unsigned long lastWifiBannerMs = 0;
static String toastMessage;
static unsigned long toastUntilMs = 0;

static void mapTouch(uint16_t rawX, uint16_t rawY, int &screenX, int &screenY) {
  screenX = map(rawX, 200, 3800, 0, 320);
  screenY = map(rawY, 200, 3800, 0, 240);
  screenX = constrain(screenX, 0, 319);
  screenY = constrain(screenY, 0, 239);
}

static String trimTail(const String &value, size_t maxLen) {
  if (value.length() <= maxLen) {
    return value;
  }
  if (maxLen <= 3) {
    return value.substring(0, maxLen);
  }
  return value.substring(0, maxLen - 3) + "...";
}

static String formatUsd(double value) {
  return "$" + String(value, 2);
}

static String formatWhole(double value) {
  return String(static_cast<long>(value + (value >= 0 ? 0.5 : -0.5)));
}

static String formatCountValue(double value) {
  double roundedWhole = value >= 0 ? floor(value + 0.5) : ceil(value - 0.5);
  if (fabs(value - roundedWhole) < 0.05) {
    return String(static_cast<long>(roundedWhole));
  }
  return String(value, 1);
}

static const char *pageLabel(MeterPage page) {
  switch (page) {
    case MeterPage::Summary:
      return "Summary";
    case MeterPage::Quota:
      return "Quota";
    case MeterPage::Models:
      return "Models";
    case MeterPage::Graph:
      return "Graph";
  }
  return "Meter";
}

static MeterPage stepPage(int delta) {
  constexpr int pageCount = 4;
  int index = static_cast<int>(currentPage);
  index = (index + delta + pageCount) % pageCount;
  return static_cast<MeterPage>(index);
}

static void setToast(const String &message, unsigned long durationMs = 2400) {
  toastMessage = message;
  toastUntilMs = millis() + durationMs;
  renderDirty = true;
}

static bool pointInButton(const TouchButton &button, int x, int y) {
  return x >= button.x && x < button.x + button.w && y >= button.y && y < button.y + button.h;
}

static void drawButton(const TouchButton &button) {
  gfx->fillRoundRect(button.x, button.y, button.w, button.h, 5, button.bg);
  gfx->drawRoundRect(button.x, button.y, button.w, button.h, 5, COLOR_HEADER_TEXT);
  gfx->setTextColor(COLOR_TEXT, button.bg);
  gfx->setTextSize(1);
  int16_t textX = button.x + 8;
  int16_t textY = button.y + 8;
  gfx->setCursor(textX, textY);
  gfx->print(button.label);
}

static void drawCard(int x, int y, int w, int h, const char *label, const String &value, uint16_t valueColor = COLOR_TEXT) {
  gfx->fillRoundRect(x, y, w, h, 6, COLOR_PANEL);
  gfx->drawRoundRect(x, y, w, h, 6, COLOR_DIM);
  gfx->setTextColor(COLOR_DIM, COLOR_PANEL);
  gfx->setTextSize(1);
  gfx->setCursor(x + 8, y + 8);
  gfx->print(label);
  gfx->setTextColor(valueColor, COLOR_PANEL);
  gfx->setTextSize(2);
  gfx->setCursor(x + 8, y + 24);
  gfx->print(trimTail(value, 12));
}

static void drawProgressBar(int x, int y, int w, int h, double percent, uint16_t fillColor) {
  int clamped = constrain(static_cast<int>(percent + 0.5), 0, 100);
  gfx->drawRoundRect(x, y, w, h, 4, COLOR_DIM);
  gfx->fillRoundRect(x + 1, y + 1, w - 2, h - 2, 4, COLOR_BG);
  int fillWidth = ((w - 2) * clamped) / 100;
  if (fillWidth > 0) {
    gfx->fillRoundRect(x + 1, y + 1, fillWidth, h - 2, 4, fillColor);
  }
}

static void drawHeader() {
  gfx->fillRect(0, 0, 320, 26, COLOR_HEADER);
  gfx->setTextColor(COLOR_HEADER_TEXT, COLOR_HEADER);
  gfx->setTextSize(2);
  gfx->setCursor(8, 6);
  gfx->print("GHCPMeter");

  gfx->setTextSize(1);
  gfx->setCursor(170, 8);
  gfx->setTextColor(COLOR_DIM, COLOR_HEADER);
  gfx->print(pageLabel(currentPage));
  gfx->setCursor(270, 8);
  if (WiFi.status() == WL_CONNECTED) {
    gfx->setTextColor(COLOR_OK, COLOR_HEADER);
    gfx->print("WiFi OK");
  } else {
    gfx->setTextColor(COLOR_WARN, COLOR_HEADER);
    gfx->print("WiFi...");
  }
}

static void drawFooter() {
  drawButton(buttonRefresh);
  drawButton(buttonSync);
  drawButton(buttonSetup);

  gfx->setTextColor(COLOR_DIM, COLOR_BG);
  gfx->setTextSize(1);
  gfx->setCursor(10, 194);
  if (toastUntilMs > millis() && toastMessage.length()) {
    gfx->print(trimTail(toastMessage, 46));
  } else if (!meterUsage.ok && meterUsage.lastError.length()) {
    gfx->print(trimTail("Err: " + meterUsage.lastError, 46));
  } else {
    gfx->print("Left prev | Right next");
  }
}

static void drawSummaryPage() {
  drawCard(10, 34, 145, 48, "Plan", meterUsage.plan.length() ? meterUsage.plan : String("offline"));
  drawCard(165, 34, 145, 48, "Monthly spend", formatUsd(meterUsage.totalMonthlySpendUsd), COLOR_WARN);
  drawCard(10, 88, 145, 48, "Subscription", formatUsd(meterUsage.subscriptionUsd), COLOR_OK);
  drawCard(165, 88, 145, 48, "Total usage", formatUsd(meterUsage.totalUsageUsd), COLOR_HEADER_TEXT);
  drawCard(10, 142, 145, 48, "Included", formatUsd(meterUsage.includedCoveredUsd), COLOR_OK);
  drawCard(165, 142, 145, 48, "Overage", formatUsd(meterUsage.overageUsd), meterUsage.overageUsd > 0.0 ? COLOR_OVERAGE : COLOR_TEXT);
}

static void drawQuotaPage() {
  int premiumUsed = meterUsage.premiumInteractionsLimit - meterUsage.premiumInteractionsRemaining;
  drawCard(10, 34, 145, 48, "Covered", formatUsd(meterUsage.includedCoveredUsd), COLOR_OK);
  drawCard(165, 34, 145, 48, "Billed", formatUsd(meterUsage.overageUsd), meterUsage.overageUsd > 0.0 ? COLOR_OVERAGE : COLOR_TEXT);
  drawCard(10, 88, 145, 48, "Premium used", formatWhole(premiumUsed), premiumUsed > meterUsage.premiumInteractionsLimit ? COLOR_OVERAGE : COLOR_TEXT);
  drawCard(165, 88, 145, 48, "Premium left", formatWhole(meterUsage.premiumInteractionsRemaining), meterUsage.premiumInteractionsRemaining < 0 ? COLOR_OVERAGE : COLOR_OK);
  drawCard(10, 142, 145, 48, "Premium cap", formatWhole(meterUsage.premiumInteractionsLimit), COLOR_HEADER_TEXT);
  drawCard(165, 142, 145, 48, "Reset", meterUsage.resetDate.length() ? meterUsage.resetDate : String("n/a"), COLOR_TEXT);

  double usagePercent = meterUsage.includedUsagePercent;
  if (meterUsage.premiumInteractionsLimit > 0) {
    usagePercent = (static_cast<double>(premiumUsed) / static_cast<double>(meterUsage.premiumInteractionsLimit)) * 100.0;
  }
  uint16_t barColor = usagePercent >= 100.0 ? COLOR_OVERAGE : (usagePercent >= 80.0 ? COLOR_WARN : COLOR_OK);
  drawProgressBar(10, 198, 300, 10, usagePercent, barColor);
}

static void drawModelsPage() {
  gfx->fillRoundRect(8, 34, 304, 176, 6, COLOR_PANEL);
  gfx->drawRoundRect(8, 34, 304, 176, 6, COLOR_DIM);

  gfx->setTextColor(COLOR_DIM, COLOR_PANEL);
  gfx->setTextSize(1);
  gfx->setCursor(16, 42);
  gfx->print("Model");
  gfx->setCursor(188, 42);
  gfx->print("Inc");
  gfx->setCursor(228, 42);
  gfx->print("Bill");
  gfx->setCursor(276, 42);
  gfx->print("$");

  if (meterUsage.modelCount == 0) {
    gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
    gfx->setTextSize(2);
    gfx->setCursor(54, 108);
    gfx->print("No model rows");
    return;
  }

  for (size_t i = 0; i < meterUsage.modelCount; i++) {
    int y = 58 + static_cast<int>(i) * 14;
    if (y > 186) {
      break;
    }
    const MeterModelUsage &row = meterUsage.models[i];
    gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
    gfx->setTextSize(1);
    gfx->setCursor(16, y);
    gfx->print(trimTail(row.model, 20));
    gfx->setCursor(184, y);
    gfx->print(formatCountValue(row.includedRequests));
    gfx->setCursor(224, y);
    gfx->print(formatCountValue(row.billedRequests));
    gfx->setCursor(268, y);
    gfx->print(trimTail(formatUsd(row.billedAmountUsd), 6));
  }
}

static void drawGraphPage() {
  drawCard(10, 34, 145, 40, "Latest day", meterUsage.chartCount ? formatUsd(meterUsage.chart[meterUsage.chartCount - 1].grossUsd) : String("$0.00"), COLOR_HEADER_TEXT);

  double maxGrossUsd = 0.0;
  for (size_t i = 0; i < meterUsage.chartCount; i++) {
    if (meterUsage.chart[i].grossUsd > maxGrossUsd) {
      maxGrossUsd = meterUsage.chart[i].grossUsd;
    }
  }
  drawCard(165, 34, 145, 40, "Max day", formatUsd(maxGrossUsd), COLOR_WARN);

  gfx->fillRoundRect(8, 82, 304, 108, 6, COLOR_PANEL);
  gfx->drawRoundRect(8, 82, 304, 108, 6, COLOR_DIM);
  gfx->setTextColor(COLOR_DIM, COLOR_PANEL);
  gfx->setTextSize(1);
  gfx->setCursor(16, 90);
  gfx->print("30-day usage");

  if (meterUsage.chartCount < 2 || maxGrossUsd <= 0.0) {
    gfx->setTextColor(COLOR_TEXT, COLOR_PANEL);
    gfx->setTextSize(2);
    gfx->setCursor(72, 132);
    gfx->print("No chart yet");
    return;
  }

  const int graphX = 18;
  const int graphY = 104;
  const int graphW = 286;
  const int graphH = 72;
  gfx->drawRect(graphX, graphY, graphW, graphH, COLOR_DIM);
  gfx->setCursor(22, 182);
  gfx->print("0");
  gfx->setCursor(246, 182);
  gfx->print(formatUsd(meterUsage.chart[meterUsage.chartCount - 1].grossUsd));
  gfx->setCursor(22, 96);
  gfx->print(formatUsd(maxGrossUsd));

  int lastX = graphX;
  int lastY = graphY + graphH - 1;
  for (size_t i = 0; i < meterUsage.chartCount; i++) {
    const MeterChartPoint &point = meterUsage.chart[i];
    int x = graphX + static_cast<int>((static_cast<float>(i) / static_cast<float>(meterUsage.chartCount - 1)) * static_cast<float>(graphW - 1));
    int y = graphY + graphH - 1 - static_cast<int>((point.grossUsd / maxGrossUsd) * static_cast<float>(graphH - 4));
    y = constrain(y, graphY + 2, graphY + graphH - 1);
    if (i > 0) {
      gfx->drawLine(lastX, lastY, x, y, COLOR_HEADER_TEXT);
    }
    gfx->fillCircle(x, y, 1, COLOR_WARN);
    lastX = x;
    lastY = y;
  }
}

static void renderUi() {
  if (!renderDirty) {
    return;
  }
  renderDirty = false;

  gfx->fillScreen(COLOR_BG);
  drawHeader();

  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiBannerMs > WIFI_RETRY_DRAW_MS) {
    lastWifiBannerMs = millis();
  }

  if (currentPage == MeterPage::Summary) {
    drawSummaryPage();
  } else if (currentPage == MeterPage::Quota) {
    drawQuotaPage();
  } else if (currentPage == MeterPage::Models) {
    drawModelsPage();
  } else {
    drawGraphPage();
  }

  drawFooter();
}

static void pollUsage(bool triggerRemoteRefresh = false) {
  if (!meterEnsureWifiConnected()) {
    meterUsage.ok = false;
    meterUsage.lastError = "wifi offline";
    renderDirty = true;
    return;
  }

  bool ok = triggerRemoteRefresh ? meterApiTriggerRefresh(meterUsage) : meterApiFetchUsage(meterUsage);
  if (ok) {
    if (triggerRemoteRefresh) {
      setToast("Helper refreshed.");
    } else {
      setToast("Usage updated.", 1200);
    }
  } else if (meterUsage.lastError.isEmpty()) {
    meterUsage.lastError = "request failed";
  }
  lastPollMs = millis();
  renderDirty = true;
}

static void handleTouch(int x, int y) {
  if (pointInButton(buttonRefresh, x, y)) {
    pollUsage(false);
    return;
  }
  if (pointInButton(buttonSync, x, y)) {
    pollUsage(true);
    return;
  }
  if (pointInButton(buttonSetup, x, y)) {
    meterOpenSetupPortal();
    return;
  }
  if (y < buttonRefresh.y) {
    currentPage = x < 160 ? stepPage(-1) : stepPage(1);
  } else {
    currentPage = stepPage(1);
  }
  renderDirty = true;
}

static void handleTouchInput() {
  bool touchDown = ts.touched();
  if (!touchDown) {
    touchWasDown = false;
    return;
  }
  if (touchWasDown || millis() - lastTouchMs < TOUCH_DEBOUNCE_MS) {
    return;
  }

  TS_Point point = ts.getPoint();
  int x = 0;
  int y = 0;
  mapTouch(point.x, point.y, x, y);
  lastTouchMs = millis();
  touchWasDown = true;
  handleTouch(x, y);
}

static void handleBootPortalHold() {
  if (digitalRead(BOOT_BTN) == LOW) {
    if (lastBootHoldStartMs == 0) {
      lastBootHoldStartMs = millis();
    } else if (millis() - lastBootHoldStartMs >= BOOT_PORTAL_HOLD_MS) {
      meterOpenSetupPortal();
    }
  } else {
    lastBootHoldStartMs = 0;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(GFX_BL, OUTPUT);
  pinMode(BOOT_BTN, INPUT_PULLUP);

  digitalWrite(GFX_BL, HIGH);
  gfx->begin();
  gfx->invertDisplay(true);
  gfx->fillScreen(COLOR_BG);
  gfx->setTextColor(COLOR_HEADER_TEXT, COLOR_BG);
  gfx->setTextSize(2);
  gfx->setCursor(18, 92);
  gfx->print("GHCPMeter CYD");
  gfx->setTextSize(1);
  gfx->setCursor(62, 114);
  gfx->print("Inverted Display");

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  bool forcePortal = digitalRead(BOOT_BTN) == LOW;
  meterConnect(forcePortal);
  analogWrite(GFX_BL, meter_brightness);
  pollUsage(false);
}

void loop() {
  meterEnsureWifiConnected();
  handleBootPortalHold();
  handleTouchInput();

  if (toastUntilMs > 0 && millis() > toastUntilMs) {
    toastUntilMs = 0;
    toastMessage = "";
    renderDirty = true;
  }

  const unsigned long pollIntervalMs = static_cast<unsigned long>(meter_poll_sec) * 1000UL;
  if (pollIntervalMs > 0 && millis() - lastPollMs >= pollIntervalMs) {
    pollUsage(false);
  }

  renderUi();
  delay(20);
}
