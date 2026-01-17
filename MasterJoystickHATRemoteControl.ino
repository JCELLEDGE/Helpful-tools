// ============================================================
// SECTION 1 OF 6
// INCLUDES, GLOBALS, PROFILES, DEBUG MODE
// ============================================================

#include <M5StickCPlus2.h>
#include <M5GFX.h>
#include <Arduino.h>
#include <WiFi.h>
#include <PinButton.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TJpg_Decoder.h>
#include <Wire.h>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define JOY_ADDR 0x38

// ============================ DEBUG FLAG ============================
bool DEBUG = false;   // set true to enable debug prints
bool gWifiConnected = false;

// ============================ JOYSTICK HAT HELPER ===================
struct JoyState {
  int8_t x;   // -128..127 deflection on X axis
  int8_t y;   // -128..127 deflection on Y axis
  int8_t btn; // 0 = pressed, 1 = released
};

// Sprites
M5Canvas sprStatus(&M5.Display);
M5Canvas sprCard(&M5.Display);
M5Canvas sprTitle(&M5.Display);
M5Canvas sprSlide(&M5.Display);
M5Canvas sprThumb(&M5.Display);

// ============================ PROFILES ==============================
struct Profile {
  const char* name;
  const char* ssid;
  const char* pass;
  const char* host;
  uint16_t    port;
};

Profile gProfiles[] = {
  { "Home PC",       "TMobile-8EAA", "171217121712", "192.168.12.137", 5005 },
  { "Laptop",    "MySSID",        "MyPassword",  "192.168.10.10", 5005 },
  { "Mac Studio",    "MySSID",        "MyPassword",     "192.168.5.10",  5005 },
  { "Desktop", "MySSID",        "MyPassword",  "192.168.20.10",  5005 },
};
static const int NUM_PROFILES = sizeof(gProfiles) / sizeof(gProfiles[0]);

int   gSelectedProfile = 0;
char  wifiSSID[32]     = "";
char  wifiPassword[32] = "";
String pro7NetworkAddress = "";
String pro7NetworkPort    = "";

// ========================= BUTTONS / TIMERS =========================
PinButton btnM5(37);      // NEXT
PinButton btnAction(39);  // PREV

static const TickType_t kPollIntervalTicks = pdMS_TO_TICKS(500);
static const TickType_t kAfterTriggerDelay = pdMS_TO_TICKS(150);

static bool gProUnreachable = false;

// Background buffer
static std::vector<uint8_t> gBgBuf;
static bool gHasBg = false;

// Thumbnail buffer
std::vector<uint8_t> gThumbBuf;

// ============================ QUEUES ================================
enum CmdType : uint8_t {
  CMD_POLL,
  CMD_NEXT,
  CMD_PREV,
  CMD_NET_UP,
  CMD_HOME0,
  CMD_CLEAR_SLIDE,
  CMD_CLEAR_TEXT,
  CMD_NEXT_FOCUS,
  CMD_PREV_FOCUS,
  CMD_CLEAR_ALL
};

struct CmdMsg { CmdType type; };
// Forward declarations for functions defined later
bool fetchBackgroundBytes(const String& uuid);
bool fetchThumbnailBytes(const String& uuid, int slideIndex);

enum UiType : uint8_t { UI_STATUS, UI_SLIDE, UI_WIFI, UI_THUMB };
struct UiMsg {
  UiType type;
  char text[64];
  int  slideIndexOneBased;
  char presName[64];
};

static QueueHandle_t cmdQ;
static QueueHandle_t uiQ;

// ========================== MISC / STATS ============================
static SemaphoreHandle_t xMutex;
static volatile uint32_t millisTime = 0;

// ============================ COLORS ================================
#define COL_BG     TFT_BLACK
#define COL_TEXT   TFT_WHITE
#define COL_MUTED  0x8410
#define COL_BORDER 0xC618
#define COL_GOOD   0x04A0
#define COL_BAD    0xE800

// Screen and layout globals
static int LCD_W = 0, LCD_H = 0;
static int PAD   = 8;

static int STATUS_H = 16;
static int HEART_H  = 12;
static int STATUS_Y = 0;

static int TITLE_Y = 0;
static int TITLE_H = 20;
static int CARD_Y  = 0;
static int CARD_H  = 0;

// Command flash
static uint32_t gCmdFlashUntil = 0;
// Command flash state (UI-driven)
uint32_t gCmdFlashStart = 0;   // when the last command completed
uint16_t gCmdFlashColor = COL_GOOD;  // already have this


// Transport state (for BKG indicator)
struct TransportState {
  bool   isPlaying = false;
  String name;
};
TransportState gTransport;
static bool gHasBkgMedia = false;
// ============================================================
// SECTION 2 OF 6
// LAYOUT, SPRITES, MARQUEE, RENDERING HELPERS
// ============================================================

struct Marquee {
  String text;
  int x = 0;
  int w = 0;
  bool needed = true;
  uint32_t lastTick = 0;
  uint32_t pauseStart = 0;
  bool inPause = false;
};

Marquee gMarq;

// ============================ MARQUEE TUNABLES ======================
static const int SCROLL_STEP_PX   = 1;
static const int SCROLL_DELAY_MS  = 75;
static const int SCROLL_SPACER    = 20;
static const int SCROLL_PAUSE_MS  = 1000;

// ============================ LAYOUT ================================
void initLayoutAndSprites() {
  LCD_W = M5.Display.width();
  LCD_H = M5.Display.height();

  PAD       = 8;
  STATUS_H  = 16;
  HEART_H   = 12;

  // === TOP: Title marquee ===
  TITLE_H  = 18;
  TITLE_Y  = PAD;

  // === MIDDLE: Slide card ===
  CARD_Y   = TITLE_Y + TITLE_H + PAD;
  int spaceForCard = LCD_H - CARD_Y - PAD - STATUS_H - PAD - 16;  // leave room for slide number bar
  if (spaceForCard < 60) spaceForCard = 60;
  CARD_H = min(spaceForCard, 72);  // or 80 for tighter layout

  // === BOTTOM: Status bar ===
  STATUS_Y = LCD_H - STATUS_H;

  sprStatus.deleteSprite();
  sprCard.deleteSprite();
  sprTitle.deleteSprite();
  sprSlide.deleteSprite();
  sprThumb.deleteSprite();

  sprCard.setColorDepth(8);
  sprCard.createSprite(LCD_W - PAD * 2, CARD_H);

  sprTitle.setColorDepth(8);
  sprTitle.createSprite(LCD_W, TITLE_H);
  sprTitle.setTextWrap(false);
  sprTitle.setTextDatum(TL_DATUM);

  sprSlide.setColorDepth(8);
  sprSlide.createSprite(1, 1);

  sprThumb.setColorDepth(8);
  sprThumb.createSprite(60, 60);
}

// ====================== CARD DRAW (AUTO MAX FONT) ====================
void drawSlideCard(int idx1, uint16_t panelColor, uint16_t borderColor) {
  sprCard.fillSprite(COL_BG);

  const int radius = 10;
  const int w = sprCard.width();
  const int h = sprCard.height();

  // Rounded panel
  sprCard.fillRoundRect(0, 0, w, h, radius, panelColor);

  // STATIC border only — no flashing logic here
  sprCard.drawRoundRect(0, 0, w, h, radius, borderColor);

  // DO NOT draw slide number inside the card anymore.
  // Thumbnails will be drawn by UI_THUMB.
  // Slide number bar is drawn separately below the card.

  sprCard.pushSprite(PAD, CARD_Y);
}

// ====================== TITLE MARQUEE ================================
void drawTitleMarqueeTick(uint16_t bg, uint16_t fg) {
  sprTitle.fillSprite(bg);
  sprTitle.fillRect(0, 0, LCD_W, 2, fg);

  sprTitle.setTextColor(fg, bg);
  sprTitle.setTextFont(2);

  const int lh = sprTitle.fontHeight();
  const int y0 = max(0, (TITLE_H - lh) / 2 + 2);
  const int totalSpan = gMarq.w + SCROLL_SPACER;
  uint32_t now = millis();

  if (!gMarq.needed) {
    int centeredX = PAD + (LCD_W - 2 * PAD - gMarq.w) / 2;
    if (centeredX < PAD) centeredX = PAD;
    sprTitle.setCursor(centeredX, y0);
    sprTitle.print(gMarq.text);
    sprTitle.pushSprite(0, TITLE_Y);
    return;
  }

  if (gMarq.inPause) {
    sprTitle.setCursor(PAD, y0);
    sprTitle.print(gMarq.text);
    if (now - gMarq.pauseStart >= SCROLL_PAUSE_MS) {
      gMarq.inPause = false;
      gMarq.lastTick = now;
      gMarq.x = 0;
    }
    sprTitle.pushSprite(0, TITLE_Y);
    return;
  }

  if (now - gMarq.lastTick >= SCROLL_DELAY_MS) {
    gMarq.lastTick = now;
    gMarq.x -= SCROLL_STEP_PX;
    if (gMarq.x < -totalSpan) {
      gMarq.x = 0;
      gMarq.inPause = true;
      gMarq.pauseStart = now;
    }
  }

  int x1 = PAD + gMarq.x;
  int x2 = x1 + gMarq.w + SCROLL_SPACER;

  sprTitle.setCursor(x1, y0); sprTitle.print(gMarq.text);
  sprTitle.setCursor(x2, y0); sprTitle.print(gMarq.text);

  sprTitle.pushSprite(0, TITLE_Y);
}

// ============================ WIFI / PRO HELPERS ====================
int wifiBars() {
  long rssi = WiFi.RSSI();
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
}

bool proReachable() {
  return (WiFi.status() == WL_CONNECTED && !gProUnreachable);
}

// ============================================================
// UI SECTION — STATUS BAR + SLIDE NUMBER BOX + UI_SLIDE HANDLER
// ============================================================
void drawStatusBar(bool heartbeatOn) {
    int barH = STATUS_H;
    int barY = STATUS_Y;

    // Background + raised divider line
    M5.Lcd.fillRect(0, barY, LCD_W, barH, COL_BG);
    M5.Lcd.drawLine(0, barY - 2, LCD_W, barY - 2, COL_BORDER);

    // Shared vertical baseline
    int baselineY = barY + 2;

    // HEARTBEAT
    int hbX = PAD;
    uint16_t hbColor = heartbeatOn ? TFT_RED : 0xA000;
    M5.Lcd.fillCircle(hbX, baselineY + 3, 3, hbColor);

    // PROFILE NAME
    const char* profName = gProfiles[gSelectedProfile].name;
    M5.Lcd.setTextFont(1);
    M5.Lcd.setTextColor(COL_TEXT, COL_BG);
    M5.Lcd.setCursor(hbX + 12, baselineY);
    M5.Lcd.print(profName);

    // PROPRESENTER "P"
    uint16_t BRIGHT_GOOD = 0x07E0;
    bool wifiOK = (WiFi.status() == WL_CONNECTED);
    bool proOK  = proReachable();

    int pX = hbX + 12 + M5.Lcd.textWidth(profName) + 10;
    uint16_t pColor =
        proOK      ? BRIGHT_GOOD :
        !wifiOK    ? COL_MUTED   :
                      COL_BAD;

    M5.Lcd.setTextColor(pColor, COL_BG);
    M5.Lcd.setCursor(pX, baselineY);
    M5.Lcd.print("P");

    // BKG GLYPH
    int bkgX = pX + 14;
    uint16_t bkgColor =
        !proOK        ? COL_MUTED :
        gHasBkgMedia  ? BRIGHT_GOOD :
                        COL_BAD;

    M5.Lcd.setTextColor(bkgColor, COL_BG);
    M5.Lcd.setCursor(bkgX, baselineY);
    M5.Lcd.print("BKG");

    // WIFI BARS
    int barsX = LCD_W - 20;
    int barsY = baselineY + 1;

    int barW = 3;
    int barGap = 2;

    long rssi = WiFi.RSSI();
    int level =
        (rssi > -50) ? 3 :
        (rssi > -70) ? 2 :
        (rssi > -85) ? 1 :
                       0;

    uint16_t barColor = wifiOK ? BRIGHT_GOOD : COL_MUTED;

    for (int i = 0; i < 3; i++) {
        int h = 3 + i * 2;
        int x = barsX + i * (barW + barGap);
        int y = barsY + (6 - h);
        if (i < level)
            M5.Lcd.fillRect(x, y, barW, h, barColor);
        else
            M5.Lcd.drawRect(x, y, barW, h, COL_MUTED);
    }
}

// ============================================================
// SLIDE NUMBER BAR — UI‑driven flash
// ============================================================
void drawSlideNumberBar(int idx1, uint16_t bg, uint16_t fg) {
    if (idx1 <= 0) return;

    String s = String(idx1);

    M5.Lcd.setTextFont(6);

    int textW = M5.Lcd.textWidth(s);
    int textH = M5.Lcd.fontHeight();

    int padX = 10;
    int padY = 4;

    int boxW = textW + padX * 2;
    int boxH = textH + padY * 2;

    int boxX = (LCD_W - boxW) / 2;
    int boxY = STATUS_Y - boxH - 6;

    // ERASE ONLY THE REGION NEEDED
    M5.Lcd.fillRect(
        boxX - 4,
        boxY - 4,
        boxW + 8,
        boxH + 8,
        bg
    );

    // BOX
    M5.Lcd.fillRoundRect(boxX, boxY, boxW, boxH, 6, COL_MUTED);

    // FLASH BORDER (MiniJoyC logic)
    if (millis() < gCmdFlashUntil) {
        // 3‑pixel thick border for StickC Plus2 visibility
        for (int i = 0; i < 3; i++) {
            M5.Lcd.drawRoundRect(
                boxX - 2 - i,
                boxY - 2 - i,
                boxW + 4 + 2*i,
                boxH + 4 + 2*i,
                8 + i,
                gCmdFlashColor
            );
        }
    }

    // TEXT
    int textX = boxX + (boxW - textW) / 2;
    int textY = boxY + (boxH - textH) / 2;

    M5.Lcd.setTextColor(fg, COL_MUTED);
    M5.Lcd.setCursor(textX, textY);
    M5.Lcd.print(s);
}
  
// ============================================================
// UI_SLIDE HANDLER — FIX #1 & FIX #3 (correct draw order)
// ============================================================
void handleUiSlide(int slideIndex) {

    // ---------------------------------------------------------
    // Draw status bar FIRST
    // (prevents overwriting the slide number border)
    // ---------------------------------------------------------
    drawStatusBar((millis() / 500) % 2 == 0);

    // ---------------------------------------------------------
    // Draw slide number box LAST
    // (ensures flash border is always visible)
    // ---------------------------------------------------------
    drawSlideNumberBar(slideIndex, COL_BG, COL_TEXT);
}

// ============================================================
// SECTION 3 OF 6
// UI TASK AND UI QUEUE HELPERS
// ============================================================

void enqueueStatus(const char* s) {
  UiMsg m{};
  m.type = UI_STATUS;
  strncpy(m.text, s, sizeof(m.text) - 1);
  xQueueSend(uiQ, &m, 0);
}

void enqueueStatusCode(const char* p, int c) {
  UiMsg m{};
  m.type = UI_STATUS;
  snprintf(m.text, sizeof(m.text), "%s %d", p, c);
  xQueueSend(uiQ, &m, 0);
}

void enqueueWifi(const char* s) {
  UiMsg m{};
  m.type = UI_WIFI;
  strncpy(m.text, s, sizeof(m.text) - 1);
  xQueueSend(uiQ, &m, 0);
}

void enqueueSlide(int idx1, const char* nm) {
  UiMsg m{};
  m.type = UI_SLIDE;
  m.slideIndexOneBased = idx1;
  strncpy(m.presName, nm ? nm : "", sizeof(m.presName) - 1);
  xQueueSend(uiQ, &m, 0);
}

// ============================ UI TASK ===============================
void uiTask(void*) {

  M5.Lcd.fillScreen(COL_BG);
  initLayoutAndSprites();

  drawStatusBar((millisTime / 500) % 2 == 0);
  
  gMarq.text = "";
  gMarq.w = 0;
  gMarq.x = 0;
  gMarq.inPause = false;

  drawTitleMarqueeTick(COL_BG, COL_TEXT);

  static int     lastSlide      = -9999;
  static String  lastTitle      = "";
  static char    lastStatus[64] = {0};
  static bool    wifiConnected  = false;
  static bool    lastReachable  = false;

  gWifiConnected = wifiConnected;

  const TickType_t UI_TICK = pdMS_TO_TICKS(40);
  UiMsg msg{};

  // flash state must be static inside the function
  static bool lastFlashActive = false;

  for (;;) {

    if (xQueueReceive(uiQ, &msg, UI_TICK) == pdTRUE) {

      // ================= STATUS / WIFI =================
      if (msg.type == UI_STATUS || msg.type == UI_WIFI) {

        if (msg.type == UI_WIFI) {
          wifiConnected = (strstr(msg.text, "OK") || strstr(msg.text, "Connected"));
        }

        uint16_t fg = COL_MUTED;
        if (strstr(msg.text, "OK") || strstr(msg.text, "Connected") ||
            strstr(msg.text, "NEXT") || strstr(msg.text, "PREV") ||
            strstr(msg.text, "HOME") || strstr(msg.text, "Wi-Fi")) {
          fg = COL_GOOD;
        } else if (strstr(msg.text, "Unreachable") || strstr(msg.text, "Request Failed")) {
          fg = COL_BAD;
        }

        if (strcmp(lastStatus, msg.text) != 0) {
          strncpy(lastStatus, msg.text, sizeof(lastStatus) - 1);
          drawStatusBar((millisTime / 500) % 2 == 0);
        }

        bool reachable = wifiConnected && !gProUnreachable;
        if (reachable != lastReachable) {
          lastReachable = reachable;
          drawSlideCard(lastSlide, COL_BG, COL_BORDER);
        }
      }

      // ================= SLIDE UPDATE =================
      else if (msg.type == UI_SLIDE) {
        bool reachable = wifiConnected && !gProUnreachable;

        if (msg.slideIndexOneBased != lastSlide) {
          lastSlide = msg.slideIndexOneBased;
          // drawSlideCard(lastSlide, COL_BG, COL_BORDER);
          drawSlideNumberBar(lastSlide, COL_BG, COL_TEXT);
        }

        String newTitle = String(msg.presName);
        if (newTitle.length() == 0) newTitle = "(no title)";
        gMarq.text = newTitle;

        if (lastTitle != newTitle) {
          lastTitle = newTitle;
          sprTitle.setTextFont(4);
          gMarq.w = sprTitle.textWidth(gMarq.text);
          gMarq.needed = (gMarq.w > LCD_W - 2 * PAD);
          gMarq.x = 0;
          gMarq.lastTick = millis();
          gMarq.inPause = gMarq.needed;
          gMarq.pauseStart = gMarq.lastTick;
        }
      }

      // ================= THUMBNAIL COMPOSITE =================
      else if (msg.type == UI_THUMB) {
        int cardW = sprCard.width();
        int cardH = sprCard.height();

        sprCard.fillSprite(COL_BG);

        bool drewSomething = false;

        // 1) Background first
        if (gHasBg && !gBgBuf.empty()) {
          sprCard.drawJpg(gBgBuf.data(), gBgBuf.size(), 0, 0, cardW, cardH);
          drewSomething = true;
        }

        // 2) Slide thumbnail
        if (!gThumbBuf.empty()) {
          uint16_t imgW = 0, imgH = 0;
          TJpgDec.getJpgSize(&imgW, &imgH, gThumbBuf.data(), gThumbBuf.size());

          float scaleW = (float)cardW / imgW;
          float scaleH = (float)cardH / imgH;
          float scale  = min(scaleW, scaleH);

          int drawW = (int)(imgW * scale);
          int drawH = (int)(imgH * scale);
          int drawX = (cardW - drawW) / 2;
          int drawY = (cardH - drawH) / 2;

          sprCard.drawJpg(gThumbBuf.data(), gThumbBuf.size(), drawX, drawY, drawW, drawH);
          drewSomething = true;
        }

        // 3) Fallback
        if (!drewSomething) {
          sprCard.fillRoundRect(0, 0, cardW, cardH, 10, COL_MUTED);
          sprCard.setTextFont(4);
          sprCard.setTextColor(TFT_WHITE, COL_MUTED);
          sprCard.setTextDatum(MC_DATUM);
          sprCard.drawString("--", cardW / 2, cardH / 2);
        }

        // 4) Push card (NO BORDER)
        sprCard.pushSprite(PAD, CARD_Y);
      }

    } else {
      // Timeout → animate marquee
      drawTitleMarqueeTick(COL_BG, COL_TEXT);
    }

    // ---------------------------------------------------------
    // Command flash window → keep redrawing slide number bar
    // ---------------------------------------------------------
    bool flashActive = (millis() < gCmdFlashUntil);

    if (flashActive) {
      if (lastSlide > 0) {
        drawSlideNumberBar(lastSlide, COL_BG, COL_TEXT);
      }
    } else if (lastFlashActive && !flashActive) {
      if (lastSlide > 0) {
        drawSlideNumberBar(lastSlide, COL_BG, COL_TEXT);
      }
    }

    lastFlashActive = flashActive;

  }
} 

// ============================================================
// SECTION 4 OF 6
// HTTP, PROPRESENTER API, THUMBNAILS, TRANSPORT
// ============================================================

bool httpGET_once(const String& url, int& code, String& body) {
  WiFiClient client;
  client.setNoDelay(true);
  client.setTimeout(2000);

  HTTPClient http;
  http.useHTTP10(true);

  if (!http.begin(client, url)) {
    code = -1000;
    return false;
  }

  http.addHeader("Connection", "close");
  http.setTimeout(2000);

  code = http.GET();
  if (code > 0) body = http.getString();

  http.end();
  return code > 0;
}

String baseUrl() {
  return "http://" + pro7NetworkAddress + ":" + pro7NetworkPort;
}

// Fetch active presentation UUID
String fetchActiveUUID() {
  int code = 0;
  String body;
  if (!httpGET_once(baseUrl() + "/v1/presentation/active", code, body))
    return "";

  if (code != HTTP_CODE_OK)
    return "";

  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, body)) return "";

  const char* uuid = doc["presentation"]["id"]["uuid"];
  return uuid ? String(uuid) : "";
}
bool fetchBackgroundBytes(const String& uuid) {
  String url = baseUrl() + "/v1/presentation/" + uuid + "/thumbnail/background";

  int code = 0;
  String body;
  if (!httpGET_once(url, code, body)) return false;
  if (code != HTTP_CODE_OK) return false;

  gBgBuf.assign(body.begin(), body.end());
  gHasBg = true;
  return true;
}

bool fetchThumbnailBytes(const String& uuid, int slideIndex) {
  String thumbUrl = baseUrl() + "/v1/presentation/" + uuid +
                    "/thumbnail/" + String(slideIndex) +
                    "?quality=136&thumbnail_type=jpeg";

  String bgUrl = baseUrl() + "/v1/presentation/" + uuid +
                 "/thumbnail/" + String(slideIndex) + "/background";

  int code = 0;
  String body;

  // 1) Fetch background first
  if (httpGET_once(bgUrl, code, body) && code == HTTP_CODE_OK && body.length() > 0) {
    gBgBuf.assign(body.begin(), body.end());
    gHasBg = true;
  } else {
    gBgBuf.clear();
    gHasBg = false;
  }

  // 2) Fetch slide thumbnail
  code = 0;
  body = "";
  if (httpGET_once(thumbUrl, code, body) && code == HTTP_CODE_OK && body.length() > 0) {
    gThumbBuf.assign(body.begin(), body.end());
  } else {
    gThumbBuf.clear();
  }

  // 3) Notify UI to composite
  UiMsg m{};
  m.type = UI_THUMB;
  xQueueSend(uiQ, &m, 0);

  return true;
}

// Transport polling
bool pollTransportPresentationCurrent() {
  int code = 0;
  String body;
  String url = baseUrl() + "/v1/transport/presentation/current?chunked=false";

  if (!httpGET_once(url, code, body)) return false;
  if (code != HTTP_CODE_OK) return false;

  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, body)) return false;

  gTransport.isPlaying = doc["is_playing"] | false;
  const char* nm = doc["name"] | "";
  gTransport.name = nm ? String(nm) : "";

  gHasBkgMedia = gTransport.isPlaying && gTransport.name.length() > 0;

  return true;
}

// ============================ WIFI HELPER ===========================
bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  static uint32_t lastAttemptMs = 0;
  uint32_t now = millis();
  if (now - lastAttemptMs < 2000) return false;
  lastAttemptMs = now;

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID, wifiPassword);
  enqueueWifi("Wi-Fi…");

  for (int i = 0; i < 10; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      enqueueWifi("Wi-Fi OK");
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  enqueueWifi("No Wi-Fi");
  return false;
}

// ============================ STATUS TASK ===========================
void statusTask(void*) {
  for (;;) {
    if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {
      millisTime = millis();
      xSemaphoreGive(xMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(700));
  }
}
// ============================ POLL SLIDE INDEX ======================
bool pollSlideOnce(String& errMsg) {
  int code = 0;
  String payload;

  if (!httpGET_once(baseUrl() + "/v1/presentation/slide_index", code, payload)) {
    errMsg = "Poll fail";
    if (!gProUnreachable) {
      gProUnreachable = true;
      enqueueStatus("Pro Unreachable");
    }
    return false;
  }

  if (code == HTTP_CODE_OK) {
    StaticJsonDocument<2048> doc;
    auto err = deserializeJson(doc, payload);
    if (err) {
      errMsg = "Bad JSON";
      return false;
    }

    int idx1 = -1;
    const char* name = "";
    JsonObject pi = doc["presentation_index"];
    if (!pi.isNull()) {
      if (pi.containsKey("index")) idx1 = (int)pi["index"] + 1;
      JsonObject pid = pi["presentation_id"];
      if (!pid.isNull() && pid.containsKey("name")) name = pid["name"];
    }

    if (gProUnreachable) {
      gProUnreachable = false;
      enqueueStatus("Pro OK");
    }

    enqueueSlide(idx1, name);

    String uuid = fetchActiveUUID();

    if (uuid.length() > 0 && !gHasBg) {
      fetchBackgroundBytes(uuid);
    }

    if (idx1 >= 1 && uuid.length() > 0) {
      int zeroBased = idx1 - 1;
      if (zeroBased >= 0) {
        fetchThumbnailBytes(uuid, zeroBased);
      }
    }

    return true;
  }

  errMsg = "HTTP";
  if (!gProUnreachable) {
    gProUnreachable = true;
    enqueueStatus("Pro Unreachable");
  }
  return false;
}

// ============================================================
// HTTP TASK — Core 0 (Unified, Flash‑Corrected, Stable)
// ============================================================
void httpTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(50));

    TickType_t nextForcedPoll     = 0;
    TickType_t lastBgPoll         = xTaskGetTickCount();
    TickType_t lastTransportPoll  = xTaskGetTickCount();

    for (;;) {
        CmdMsg cmd;
        bool didWork = false;

        // -----------------------------------------------------
        // PROCESS COMMAND QUEUE
        // -----------------------------------------------------
        while (xQueueReceive(cmdQ, &cmd, 0) == pdTRUE) {
            didWork = true;

    auto triggerFlash = [&](bool ok) {
        gCmdFlashColor = ok ? COL_GOOD : COL_BAD;
        gCmdFlashUntil = millis() + 500;   // <-- correct MiniJoyC behavior
    };
            switch (cmd.type) {

                // ---------------------------------------------------------
                // NEXT
                // ---------------------------------------------------------
                case CMD_NEXT: {
                    enqueueStatus("NEXT…");
                    if (ensureWiFi()) {
                        int code = 0;
                        String body;

                        bool ok =
                            httpGET_once(baseUrl() + "/v1/trigger/next", code, body) &&
                            code == HTTP_CODE_NO_CONTENT;

                        enqueueStatus(ok ? "NEXT OK" : "Request Failed");
                        triggerFlash(ok);

                        if (ok)
                            nextForcedPoll = xTaskGetTickCount() + kAfterTriggerDelay;
                    }
                } break;

                // ---------------------------------------------------------
                // PREV
                // ---------------------------------------------------------
                case CMD_PREV: {
                    enqueueStatus("PREV…");
                    if (ensureWiFi()) {
                        int code = 0;
                        String body;

                        bool ok =
                            httpGET_once(baseUrl() + "/v1/trigger/previous", code, body) &&
                            code == HTTP_CODE_NO_CONTENT;

                        enqueueStatus(ok ? "PREV OK" : "Request Failed");
                        triggerFlash(ok);

                        if (ok)
                            nextForcedPoll = xTaskGetTickCount() + kAfterTriggerDelay;
                    }
                } break;

                // ---------------------------------------------------------
                // HOME
                // ---------------------------------------------------------
                case CMD_HOME0: {
                    enqueueStatus("HOME…");
                    if (ensureWiFi()) {
                        int code = 0;
                        String body;

                        bool ok =
                            httpGET_once(baseUrl() + "/v1/presentation/active/trigger", code, body) &&
                            code == HTTP_CODE_NO_CONTENT;

                        enqueueStatus(ok ? "HOME OK" : "Request Failed");
                        triggerFlash(ok);

                        if (ok)
                            nextForcedPoll = xTaskGetTickCount() + kAfterTriggerDelay;
                    }
                } break;

                // ---------------------------------------------------------
                // CLEAR SLIDE
                // ---------------------------------------------------------
                case CMD_CLEAR_SLIDE: {
                    enqueueStatus("CLEAR SLIDE…");
                    if (ensureWiFi()) {
                        int code = 0;
                        String body;

                        bool ok =
                            httpGET_once(baseUrl() + "/v1/clear/layer/slide", code, body) &&
                            code == HTTP_CODE_NO_CONTENT;

                        enqueueStatus(ok ? "CLEAR SLIDE OK" : "Request Failed");
                        triggerFlash(ok);
                    }
                } break;
                // ---------------------------------------------------------
                // CLEAR ALL
                // ---------------------------------------------------------
                case CMD_CLEAR_ALL: {
                    enqueueStatus("CLEAR ALL…");
                    if (ensureWiFi()) {
                        int code = 0;
                        String body;

                        bool ok =
                            httpGET_once(baseUrl() + "/v1/clear/group/Clear%20All/trigger", code, body) &&
                            code == HTTP_CODE_NO_CONTENT;

                        enqueueStatus(ok ? "CLEAR ALL OK" : "Request Failed");
                        triggerFlash(ok);
                    }
                } break;
                // ---------------------------------------------------------
                // NEXT FOCUS
                // ---------------------------------------------------------
                case CMD_NEXT_FOCUS: {
                    enqueueStatus("NEXT FOCUS…");
                    if (ensureWiFi()) {
                        int code = 0;
                        String body;

                        bool ok =
                            httpGET_once(baseUrl() + "/v1/playlist/focused/next/trigger", code, body) &&
                            code == HTTP_CODE_NO_CONTENT;

                        enqueueStatus(ok ? "NEXT FOCUS OK" : "Request Failed");
                        triggerFlash(ok);

                        if (ok)
                            nextForcedPoll = xTaskGetTickCount() + kAfterTriggerDelay;
                    }
                } break;

                // ---------------------------------------------------------
                // PREV FOCUS
                // ---------------------------------------------------------
                case CMD_PREV_FOCUS: {
                    enqueueStatus("PREV FOCUS…");
                    if (ensureWiFi()) {
                        int code = 0;
                        String body;

                        bool ok =
                            httpGET_once(baseUrl() + "/v1/playlist/focused/previous/trigger", code, body) &&
                            code == HTTP_CODE_NO_CONTENT;

                        enqueueStatus(ok ? "PREV FOCUS OK" : "Request Failed");
                        triggerFlash(ok);

                        if (ok)
                            nextForcedPoll = xTaskGetTickCount() + kAfterTriggerDelay;
                    }
                } break;

                // ---------------------------------------------------------
                // NETWORK UP
                // ---------------------------------------------------------
                case CMD_NET_UP:
                    (void)ensureWiFi();
                    break;

                // ---------------------------------------------------------
                // POLL
                // ---------------------------------------------------------
                case CMD_POLL: {
                    if (ensureWiFi()) {
                        String err;
                        if (!pollSlideOnce(err)) {
                            vTaskDelay(pdMS_TO_TICKS(150));
                            pollSlideOnce(err);
                        }
                    }
                } break;
            }
        }

        TickType_t now = xTaskGetTickCount();

        // -----------------------------------------------------
        // FORCED POLL AFTER NEXT/PREV/HOME/FOCUS
        // -----------------------------------------------------
        if (nextForcedPoll && now >= nextForcedPoll) {
            nextForcedPoll = 0;
            if (ensureWiFi()) {
                String err;
                if (!pollSlideOnce(err)) {
                    vTaskDelay(pdMS_TO_TICKS(150));
                    pollSlideOnce(err);
                }
            }
        }

        // -----------------------------------------------------
        // BACKGROUND CADENCE POLL
        // -----------------------------------------------------
        if (!didWork && (now - lastBgPoll) >= kPollIntervalTicks) {
            lastBgPoll = now;
            if (ensureWiFi()) {
                String err;
                if (!pollSlideOnce(err)) {
                    vTaskDelay(pdMS_TO_TICKS(150));
                    pollSlideOnce(err);
                }
            }
        }

        // -----------------------------------------------------
        // TRANSPORT POLL
        // -----------------------------------------------------
        if ((now - lastTransportPoll) >= pdMS_TO_TICKS(1000)) {
            lastTransportPoll = now;
            if (ensureWiFi()) {
                (void)pollTransportPresentationCurrent();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
// ============================================================
// SECTION 5 OF 6
// JOYSTICK, DEBUG HELPER, LOOP LOGIC
// ============================================================
JoyState readJoystickHat() {
  JoyState js{0, 0, 1};

  Wire.beginTransmission(JOY_ADDR);
  Wire.write(0x02);
  Wire.endTransmission();

  Wire.requestFrom(JOY_ADDR, (uint8_t)3);
  if (Wire.available() >= 3) {
    js.x   = (int8_t)Wire.read();
    js.y   = (int8_t)Wire.read();
    js.btn = (int8_t)Wire.read();
  }

  return js;
}
// ====================== JOYSTICK DEBUG HELPERS ======================

// Print raw X/Y/button values directly from the Joystick HAT
void debugJoystickRaw() {
  JoyState js = readJoystickHat();
  Serial.printf("RAW  X=%d  Y=%d  Btn=%d\n", js.x, js.y, js.btn);
}

// Print normalized X/Y, direction, and button state
void debugJoystickNorm() {
  JoyState js = readJoystickHat();

  int nx = normAxis(js.x);
  int ny = normAxis(js.y);
  String dir = classifyDirection(nx, ny);

  Serial.printf("NORM X=%d  Y=%d  DIR=%s  Btn=%d\n",
                nx, ny, dir.c_str(), js.btn);
}

// Print only when something changes (cleanest output)
void debugJoystickOnChange() {
  static int lastX = 999, lastY = 999, lastBtn = 999;
  static String lastDir = "";

  JoyState js = readJoystickHat();
  int nx = normAxis(js.x);
  int ny = normAxis(js.y);
  String dir = classifyDirection(nx, ny);

  if (nx != lastX || ny != lastY || js.btn != lastBtn || dir != lastDir) {
    lastX = nx;
    lastY = ny;
    lastBtn = js.btn;
    lastDir = dir;

    Serial.printf("JOY  X=%d  Y=%d  DIR=%s  Btn=%d\n",
                  nx, ny, dir.c_str(), js.btn);
  }
}


// ====================== JOYSTICK NORMALIZATION ======================

int normAxis(int v) {
  const int DZ = 20;  // Deadzone tuned for Joystick HAT
  if (v > DZ) return +1;
  if (v < -DZ) return -1;
  return 0;
}

String classifyDirection(int nx, int ny) {
  if (abs(nx) > abs(ny)) {
    if (nx > 0) return "RIGHT";
    if (nx < 0) return "LEFT";
  } else {
    if (ny > 0) return "UP";
    if (ny < 0) return "DOWN";
  }
  return "CENTER";
}

void processJoystick() {
  static String lastDir = "CENTER";
  static bool dirLatched = false;
  static int lastBtn = 1;

  JoyState js = readJoystickHat();

  int nx = normAxis(js.x);
  int ny = normAxis(js.y);
  String dir = classifyDirection(nx, ny);

  // ============================
  // Direction latch logic
  // ============================

  // When joystick returns to CENTER, unlock it
  if (dir == "CENTER") {
    dirLatched = false;
  }

  // Only fire when:
  // 1. We are NOT latched
  // 2. Direction is NOT CENTER
  if (!dirLatched && dir != "CENTER") {

    if (dir == "UP") {
      CmdMsg c{ CMD_NEXT };
      xQueueSend(cmdQ, &c, 0);
    }
    else if (dir == "DOWN") {
      CmdMsg c{ CMD_PREV };
      xQueueSend(cmdQ, &c, 0);
    }
    else if (dir == "LEFT") {
      CmdMsg c{ CMD_PREV_FOCUS };
      xQueueSend(cmdQ, &c, 0);
    }
    else if (dir == "RIGHT") {
      CmdMsg c{ CMD_NEXT_FOCUS };
      xQueueSend(cmdQ, &c, 0);
    }

    // Latch until CENTER is reached again
    dirLatched = true;
  }

  // ============================
  // Button press → CLEAR SLIDE
  // ============================
  if (js.btn != lastBtn) {
    if (lastBtn == 1 && js.btn == 0) {
      CmdMsg c{ CMD_CLEAR_SLIDE };
      xQueueSend(cmdQ, &c, 0);
    }
    lastBtn = js.btn;
  }
}

// === Unified Joystick Debug Helper ===
void debugJoystick() {
  if (!DEBUG) return;

  JoyState js = readJoystickHat();
  int x   = js.x;
  int y   = js.y;
  int btn = js.btn;

  const int DEADZONE = 5;
  static int lastX = 0, lastY = 0, lastBtn = -1;
  static String lastDir = "";

  String dir = "CENTER";
  if (abs(x) > abs(y)) {
    if (x > DEADZONE) dir = "RIGHT";
    else if (x < -DEADZONE) dir = "LEFT";
  } else {
    if (y > DEADZONE) dir = "UP";
    else if (y < -DEADZONE) dir = "DOWN";
  }

  bool changed = false;
  if (abs(x - lastX) > DEADZONE) { lastX = x; changed = true; }
  if (abs(y - lastY) > DEADZONE) { lastY = y; changed = true; }
  if (btn != lastBtn) { lastBtn = btn; changed = true; }
  if (dir != lastDir) { lastDir = dir; changed = true; }

  if (changed) {
    Serial.printf("Joystick: %s (X=%d Y=%d Btn=%d)\n", dir.c_str(), x, y, btn);
  }
}

// ============================== LOOP ================================
void loop() {
  M5.update();
  btnAction.update();
  btnM5.update();

  processJoystick();       // <-- ONLY joystick logic
  debugJoystickOnChange(); // optional

  // ================= FRONT BUTTONS =================
  if (btnAction.isClick()) {
    enqueueStatus("CLICK CLEAR ALL");
    CmdMsg c{ CMD_CLEAR_ALL };
    xQueueSend(cmdQ, &c, 0);
  }
  if (btnM5.isClick()) {
    enqueueStatus("CLICK NEXT");
    CmdMsg c{ CMD_NEXT };
    xQueueSend(cmdQ, &c, 0);
  }

  if (M5.BtnPWR.wasPressed()) {
    // reserved for future use
  }

  // ================= NETWORK KEEPALIVE =================
  static uint32_t lastNetKick = 0;
  uint32_t now = millis();
  if (now - lastNetKick > 3000) {
    lastNetKick = now;
    if (WiFi.status() != WL_CONNECTED) {
      CmdMsg c{ CMD_NET_UP };
      xQueueSend(cmdQ, &c, 0);
    }
  }

  static bool lastConn = false;
  bool nowConn = (WiFi.status() == WL_CONNECTED);
  if (nowConn != lastConn) {
    lastConn = nowConn;
    enqueueWifi(nowConn ? "Wi-Fi OK" : "No Wi-Fi");
  }

  vTaskDelay(pdMS_TO_TICKS(10));
}

// ============================================================
// SECTION 6 OF 6
// PROFILE SELECTOR, INFO PAGE, SETUP
// ============================================================
void showNetworkInfoPage() {
  M5.Lcd.fillScreen(COL_BG);
  M5.Lcd.setTextFont(2);
  M5.Lcd.setTextColor(COL_TEXT, COL_BG);

  uint32_t start = millis();

  // Wait for Arduino WiFi to finish connecting
  while (WiFi.localIP().toString() == "0.0.0.0" &&
         millis() - start < 3000) {
    delay(100);
  }

  String ip   = WiFi.localIP().toString();
  String mac  = WiFi.macAddress();
  String ssid = WiFi.SSID();
  String prof = gProfiles[gSelectedProfile].name;

  if (ssid.length() == 0) ssid = "(no SSID)";
  if (ip == "0.0.0.0")    ip   = "(no IP)";
  if (mac.length() == 0)  mac  = "(no MAC)";

  M5.Lcd.setCursor(10, 10);
  M5.Lcd.println("Network Info");

  M5.Lcd.setCursor(10, 40);
  M5.Lcd.printf("Profile: %s\n", prof.c_str());

  M5.Lcd.setCursor(10, 70);
  M5.Lcd.printf("SSID:    %s\n", ssid.c_str());

  M5.Lcd.setCursor(10, 100);
  M5.Lcd.printf("IP:      %s\n", ip.c_str());

  M5.Lcd.setCursor(10, 130);
  M5.Lcd.printf("MAC:     %s\n", mac.c_str());

  bool connected = (ip != "(no IP)");

  M5.Lcd.setCursor(10, 160);
  M5.Lcd.printf("Status:  %s\n", connected ? "Connected" : "Not connected");

  // Debounce exit
  uint32_t debounceStart = millis();
  while (true) {
    M5.update();
    btnM5.update();
    btnAction.update();

    JoyState js = readJoystickHat();
    int nx = normAxis(js.x);
    int ny = normAxis(js.y);
    String dir = classifyDirection(nx, ny);

    if (dir != "CENTER") break;
    if (millis() - debounceStart > 300 && js.btn == 0) break;
    if (M5.BtnA.wasPressed()) break;
    if (btnM5.isClick() || btnAction.isClick()) break;

    delay(20);
  }

  M5.Lcd.fillScreen(COL_BG);
}

void runProfileMenu() {
  int sel = gSelectedProfile;
  if (sel < 0 || sel >= NUM_PROFILES) sel = 0;

  int lastSel = -1;
  String lastDir = "CENTER";
  bool dirLatched = false;

  for (;;) {

    // Redraw only when selection changes
    if (sel != lastSel) {
      lastSel = sel;

      M5.Lcd.fillScreen(COL_BG);
      M5.Lcd.setTextColor(COL_TEXT, COL_BG);
      M5.Lcd.setTextFont(2);

      M5.Lcd.setCursor(PAD, 10);
      M5.Lcd.println("Select Profile:");

      int y = 40;
      for (int i = 0; i < NUM_PROFILES; i++) {
        M5.Lcd.setTextColor((i == sel) ? COL_GOOD : COL_MUTED, COL_BG);
        M5.Lcd.setCursor(PAD, y);
        M5.Lcd.println(gProfiles[i].name);
        y += 18;
      }
    }

    // Update inputs
    M5.update();
    btnM5.update();
    btnAction.update();

    // ============================
    // Joystick navigation (latch‑safe)
    // ============================
    JoyState js = readJoystickHat();
    int nx = normAxis(js.x);
    int ny = normAxis(js.y);
    String dir = classifyDirection(nx, ny);

    // Reset latch when returning to CENTER
    if (dir == "CENTER") {
      dirLatched = false;
    }

    // Fire only once per direction entry
    if (!dirLatched && dir != "CENTER") {

      if (dir == "UP") {
        sel--;
        if (sel < 0) sel = NUM_PROFILES - 1;
      }
      else if (dir == "DOWN") {
        sel++;
        if (sel >= NUM_PROFILES) sel = 0;
      }

      dirLatched = true;
    }

    lastDir = dir;

    // ============================
    // Selection actions
    // ============================

    // Joystick button selects
    if (js.btn == 0) {
      gSelectedProfile = sel;
      break;
    }

    // BtnA selects
    if (M5.BtnA.wasPressed()) {
      gSelectedProfile = sel;
      break;
    }

    // Existing buttons still work
    if (btnM5.isClick()) {
      sel = (sel + 1) % NUM_PROFILES;
    }
    if (btnAction.isClick()) {
      sel = (sel - 1 + NUM_PROFILES) % NUM_PROFILES;
    }

    // Power button selects
    if (M5.BtnPWR.wasPressed()) {
      gSelectedProfile = sel;
      break;
    }

    vTaskDelay(pdMS_TO_TICKS(30));
  }

  // Apply selected profile
  strncpy(wifiSSID, gProfiles[gSelectedProfile].ssid, sizeof(wifiSSID) - 1);
  strncpy(wifiPassword, gProfiles[gSelectedProfile].pass, sizeof(wifiPassword) - 1);
  pro7NetworkAddress = gProfiles[gSelectedProfile].host;
  pro7NetworkPort    = String(gProfiles[gSelectedProfile].port);

  M5.Lcd.fillScreen(COL_BG);
}

void setup() {
  Serial.begin(115200);

  xMutex = xSemaphoreCreateMutex();

  M5.begin();
  M5.Lcd.setRotation(0);
  initLayoutAndSprites();

  M5.Lcd.fillScreen(COL_BG);
  M5.Lcd.setTextColor(COL_TEXT, COL_BG);
  WiFi.setSleep(false);

  Wire.begin(0, 26, 400000);

  // ⭐ FIX: Create queues BEFORE any task or WiFi call
  cmdQ = xQueueCreate(16, sizeof(CmdMsg));
  uiQ  = xQueueCreate(16, sizeof(UiMsg));

  // Profile selection
  runProfileMenu();

  // WiFi must run AFTER queues exist
  ensureWiFi();

  // Now safe to show info page
  showNetworkInfoPage();

  xTaskCreatePinnedToCore(statusTask, "statusTask", 2048, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(uiTask,     "uiTask",     6144, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(httpTask,   "httpTask",   6144, nullptr, 4, nullptr, 0);

  enqueueWifi("Booting");
  CmdMsg c{ CMD_POLL };
  xQueueSend(cmdQ, &c, 0);
}
