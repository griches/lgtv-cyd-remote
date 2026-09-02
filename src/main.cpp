// LG TV remote for the ESP32-2432S028 "Cheap Yellow Display".
//
// A touch port of the lgtv-streamdeck plugin: same actions, same artwork,
// same webOS protocol, same pairings (imported from lgtvremote-cli).
//
// Layout: 4x3 grid of 64px key tiles (80x72 cells) above a 24px status bar.
// Tap the left of the bar to switch TV, the right of the bar to change page.

#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

#include "assets.h"
#include "lgtv.h"
#include "log.h"
#include "secrets.h"
#include "tv_config.h"

// ------------------------------------------------------------- hardware

static TFT_eSPI tft;
static SPIClass touchSPI(VSPI);
static XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
static LGTV lg;
SemaphoreHandle_t g_serialMutex;

static const int RAW_MIN = 200, RAW_MAX = 3700;  // touch calibration

// --------------------------------------------------------------- layout

static const int COLS = 4, ROWS = 3;
static const int CELL_W = 80, CELL_H = 72;
static const int GRID_H = ROWS * CELL_H;          // 216
static const int BAR_Y = GRID_H, BAR_H = 24;      // 216..240
static const uint16_t BG = TFT_BLACK;
static const uint16_t BAR_BG = 0x10A2;            // very dark grey

enum class Dyn : uint8_t { None, Mute, Play, Screen };

struct Tile {
  const uint16_t* icon;
  const uint16_t* iconAlt;  // shown when the dynamic state is "on"
  CmdType cmd;
  const char* a;
  const char* b;
  Dyn dyn;
  bool repeat;              // auto-repeat while held
};

#define T_REQ(icon, uri)        {icon, nullptr, CmdType::Request, uri, "", Dyn::None, false}
#define T_REQR(icon, uri)       {icon, nullptr, CmdType::Request, uri, "", Dyn::None, true}
#define T_BTN(icon, name)       {icon, nullptr, CmdType::Button, name, "", Dyn::None, true}
#define T_BTN1(icon, name)      {icon, nullptr, CmdType::Button, name, "", Dyn::None, false}
#define T_APP(icon, id)         {icon, nullptr, CmdType::LaunchApp, id, "", Dyn::None, false}
#define T_NONE                  {nullptr, nullptr, CmdType::None, "", "", Dyn::None, false}

struct Page { const char* name; Tile tiles[COLS * ROWS]; };

static const Page PAGES[] = {
  {"Remote", {
    {ic_power, nullptr, CmdType::PowerToggle, "", "", Dyn::None, false},
    T_BTN(ic_up, "UP"),
    {ic_unmuted, ic_muted, CmdType::MuteToggle, "", "", Dyn::Mute, false},
    T_REQR(ic_vol_up, "ssap://audio/volumeUp"),

    T_BTN(ic_left, "LEFT"),
    T_BTN1(ic_ok, "ENTER"),
    T_BTN(ic_right, "RIGHT"),
    T_REQR(ic_vol_down, "ssap://audio/volumeDown"),

    T_BTN1(ic_back, "BACK"),
    T_BTN(ic_down, "DOWN"),
    T_BTN1(ic_home, "HOME"),
    {ic_play, ic_pause, CmdType::PlayPause, "", "", Dyn::Play, false},
  }},
  {"Apps", {
    T_APP(app_netflix, "netflix"),
    T_APP(app_youtube_leanback_v4, "youtube.leanback.v4"),
    T_APP(app_com_disney_disneyplus_prod, "com.disney.disneyplus-prod"),
    T_APP(app_amazon, "amazon"),

    T_APP(app_com_apple_appletv, "com.apple.appletv"),
    T_APP(app_bbc_iplayer_3_0, "bbc.iplayer.3.0"),
    T_APP(app_com_fvp_itv, "com.fvp.itv"),
    T_APP(app_com_channel4_ondemand, "com.channel4.ondemand"),

    T_APP(app_demand5, "demand5"),
    T_APP(app_now_tv, "now.tv"),
    T_APP(app_spotify_beehive, "spotify-beehive"),
    T_APP(app_plex, "plex"),
  }},
  {"Extras", {
    {ic_screen_off, ic_screen_on, CmdType::ScreenToggle, "", "", Dyn::Screen, false},
    T_BTN1(ic_exit, "EXIT"),
    T_BTN1(ic_settings, "MENU"),
    T_APP(app_com_webos_app_livetv, "com.webos.app.livetv"),

    T_APP(app_airplay, "airplay"),
    T_APP(app_com_webos_app_photovideo, "com.webos.app.photovideo"),
    T_APP(app_com_webos_app_music, "com.webos.app.music"),
    T_APP(app_com_webos_app_igallery, "com.webos.app.igallery"),

    T_APP(app_lgchannels_uk, "lgchannels.uk"),
    T_APP(app_com_palm_app_settings, "com.palm.app.settings"),
    T_APP(app_com_bskyb_skystore, "com.bskyb.skystore"),
    T_APP(app_mubi, "mubi"),
  }},
};
static const int PAGE_COUNT = sizeof(PAGES) / sizeof(PAGES[0]);

// ---------------------------------------------------------------- state

static int page = 0;
static int pressedTile = -1;        // index in current page, -1 = none
static bool barPressed = false;
static uint32_t pressStart = 0, lastRepeat = 0;

// Snapshots of LGTV state so we redraw only on change.
static LinkState shownLink = LinkState::NoWifi;
static bool shownMuted = false, shownPlaying = false, shownScreenOff = false;
static int shownTV = -1;
static uint32_t shownError = 0;
static uint32_t errorFlashUntil = 0;

// ------------------------------------------------------------- drawing

static bool dynOn(Dyn d) {
  switch (d) {
    case Dyn::Mute:   return lg.muted.load();
    case Dyn::Play:   return lg.playing.load();
    case Dyn::Screen: return lg.screenOff.load();
    default:          return false;
  }
}

static void tileRect(int idx, int& x, int& y) {
  x = (idx % COLS) * CELL_W;
  y = (idx / COLS) * CELL_H;
}

static void drawTile(int idx, bool highlight) {
  const Tile& t = PAGES[page].tiles[idx];
  int x, y;
  tileRect(idx, x, y);
  tft.fillRect(x, y, CELL_W, CELL_H, BG);
  if (!t.icon) return;
  const uint16_t* icon = (t.iconAlt && dynOn(t.dyn)) ? t.iconAlt : t.icon;
  int ix = x + (CELL_W - ICON_W) / 2, iy = y + (CELL_H - ICON_H) / 2;
  tft.pushImage(ix, iy, ICON_W, ICON_H, icon);
  if (highlight) {
    bool usable = lg.link.load() == LinkState::Registered || t.cmd == CmdType::PowerToggle;
    uint16_t c = usable ? TFT_WHITE : TFT_RED;
    tft.drawRoundRect(ix - 3, iy - 3, ICON_W + 6, ICON_H + 6, 8, c);
    tft.drawRoundRect(ix - 4, iy - 4, ICON_W + 8, ICON_H + 8, 9, c);
  }
}

static void drawGrid() {
  for (int i = 0; i < COLS * ROWS; i++) drawTile(i, i == pressedTile);
}

static void drawBar() {
  tft.fillRect(0, BAR_Y, tft.width(), BAR_H, BAR_BG);
  LinkState ls = lg.link.load();
  uint16_t dot = ls == LinkState::Registered ? TFT_GREEN
               : ls == LinkState::Connecting ? TFT_YELLOW
               : ls == LinkState::Offline    ? TFT_RED
                                             : TFT_DARKGREY;
  if (millis() < errorFlashUntil) dot = TFT_RED;
  tft.fillCircle(10, BAR_Y + BAR_H / 2, 4, dot);

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, BAR_BG);
  const char* label = ls == LinkState::NoWifi ? (WiFi.status() == WL_CONNECTED ? "wifi ok" : "wifi...")
                                              : lg.current().name;
  tft.drawString(label, 20, BAR_Y + BAR_H / 2, 2);

  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, BAR_BG);
  char pg[32];
  snprintf(pg, sizeof(pg), "%s  %d/%d >", PAGES[page].name, page + 1, PAGE_COUNT);
  tft.drawString(pg, tft.width() - 6, BAR_Y + BAR_H / 2, 2);
}

// --------------------------------------------------------------- input

static void fire(const Tile& t) {
  switch (t.cmd) {
    case CmdType::None: return;
    case CmdType::Request:    lg.post(CmdType::Request, t.a, t.b); break;
    case CmdType::Button:     lg.post(CmdType::Button, t.a); break;
    case CmdType::LaunchApp:  lg.post(CmdType::LaunchApp, t.a); break;
    default:                  lg.post(t.cmd); break;
  }
}

static void onPress(int x, int y) {
  if (y >= BAR_Y) {
    barPressed = true;
    if (x < tft.width() / 2) {
      int next = (lg.tvIndex.load() + 1) % TV_COUNT;
      char idx[4];
      snprintf(idx, sizeof(idx), "%d", next);
      lg.post(CmdType::SelectTV, idx);
    } else {
      page = (page + 1) % PAGE_COUNT;
      drawGrid();
      drawBar();
    }
    return;
  }
  int idx = (y / CELL_H) * COLS + (x / CELL_W);
  if (idx < 0 || idx >= COLS * ROWS) return;
  const Tile& t = PAGES[page].tiles[idx];
  if (!t.icon) return;
  pressedTile = idx;
  pressStart = lastRepeat = millis();
  drawTile(idx, true);
  fire(t);
}

static void onHold() {
  if (pressedTile < 0) return;
  const Tile& t = PAGES[page].tiles[pressedTile];
  if (!t.repeat) return;
  uint32_t now = millis();
  if (now - pressStart > 450 && now - lastRepeat > 180) {
    lastRepeat = now;
    fire(t);
  }
}

static void onRelease() {
  if (pressedTile >= 0) {
    int idx = pressedTile;
    pressedTile = -1;
    drawTile(idx, false);
  }
  barPressed = false;
}

static void pollTouch() {
  static bool wasDown = false;
  bool down = touch.touched();
  if (down && !wasDown) {
    TS_Point p = touch.getPoint();
    // Ignore phantom presses: the IRQ line can twitch (notably at boot) with
    // no real pressure, in which case the controller reports z = 0.
    if (p.z < 150 || millis() < 1500) return;
    int x = constrain((int)map(p.x, RAW_MIN, RAW_MAX, 0, tft.width()), 0, tft.width() - 1);
    int y = constrain((int)map(p.y, RAW_MIN, RAW_MAX, 0, tft.height()), 0, tft.height() - 1);
    onPress(x, y);
  } else if (down && wasDown) {
    onHold();
  } else if (!down && wasDown) {
    onRelease();
  }
  wasDown = down;
}

// --------------------------------------------------------- serial debug
//
//   S          dump the framebuffer: "SCREEN w h\n" then h rows of w*3 raw
//              RGB bytes (see tools/screenshot.py)
//   t X Y      simulate a tap at screen coords
//   p N        jump to page N (0-based)
//   b NAME     send a remote button (UP, ENTER, HOME...) via the pointer socket
//   h          free heap

static void dumpScreen() {
  static uint8_t row[320 * 3];
  xSemaphoreTake(g_serialMutex, portMAX_DELAY);  // nobody else may print mid-dump
  Serial.printf("SCREEN %d %d\n", tft.width(), tft.height());
  Serial.flush();
  for (int y = 0; y < tft.height(); y++) {
    tft.readRectRGB(0, y, tft.width(), 1, row);
    Serial.write(row, tft.width() * 3);
  }
  Serial.flush();
  Serial.println();
  Serial.println("SCREEN_END");
  xSemaphoreGive(g_serialMutex);
}

static void handleSerial() {
  static char line[48];
  static int n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') { if (n < (int)sizeof(line) - 1) line[n++] = c; continue; }
    line[n] = 0; n = 0;
    if (!line[0]) continue;
    char* arg = line + 1;
    while (*arg == ' ') arg++;
    switch (line[0]) {
      case 'S': dumpScreen(); break;
      case 'h': LOGF("[dbg] heap %u\n", ESP.getFreeHeap()); break;
      case 'p': page = constrain(atoi(arg), 0, PAGE_COUNT - 1); drawGrid(); drawBar(); LOGF("[dbg] page %d\n", page); break;
      case 'b': lg.post(CmdType::Button, arg); break;
      case 't': {
        int x = 0, y = 0;
        if (sscanf(arg, "%d %d", &x, &y) == 2) {
          LOGF("[dbg] tap %d,%d\n", x, y);
          onPress(x, y);
          delay(120);
          onRelease();
        }
        break;
      }
      default: LOGF("[dbg] unknown '%s'\n", line); break;
    }
  }
}

// ---------------------------------------------------------- state sync

static void syncState() {
  bool barDirty = false, gridDirty = false;

  LinkState ls = lg.link.load();
  if (ls != shownLink) { shownLink = ls; barDirty = true; }
  int tv = lg.tvIndex.load();
  if (tv != shownTV) { shownTV = tv; barDirty = true; }

  bool m = lg.muted.load(), p = lg.playing.load(), s = lg.screenOff.load();
  if (m != shownMuted || p != shownPlaying || s != shownScreenOff) {
    shownMuted = m; shownPlaying = p; shownScreenOff = s;
    gridDirty = true;
  }

  uint32_t err = lg.lastError.load();
  if (err != shownError) { shownError = err; errorFlashUntil = millis() + 600; barDirty = true; }
  static bool flashing = false;
  bool nowFlashing = millis() < errorFlashUntil;
  if (nowFlashing != flashing) { flashing = nowFlashing; barDirty = true; }

  // Wi-Fi label refresh while connecting
  static wl_status_t shownWifi = WL_IDLE_STATUS;
  if (WiFi.status() != shownWifi) { shownWifi = WiFi.status(); barDirty = true; }

  if (gridDirty) {
    for (int i = 0; i < COLS * ROWS; i++)
      if (PAGES[page].tiles[i].dyn != Dyn::None) drawTile(i, i == pressedTile);
  }
  if (barDirty) drawBar();
}

// ---------------------------------------------------------------- main

void setup() {
  Serial.begin(460800);
  g_serialMutex = xSemaphoreCreateMutex();
  delay(100);
  Serial.println("\n[cyd] LG remote booting");

  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  digitalWrite(LED_R, HIGH); digitalWrite(LED_G, HIGH); digitalWrite(LED_B, HIGH);

  tft.init();
  tft.setRotation(1);
  tft.setSwapBytes(true);
  tft.fillScreen(BG);

  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  touch.begin(touchSPI);
  touch.setRotation(1);

  drawGrid();
  drawBar();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  LOGF("[cyd] wifi connecting to %s\n", WIFI_SSID);

  lg.begin(TVS, TV_COUNT, 0);
  LOGF("[cyd] %d TVs, default %s\n", TV_COUNT, TVS[0].name);
}

void loop() {
  static bool wifiLogged = false;
  if (!wifiLogged && WiFi.status() == WL_CONNECTED) {
    wifiLogged = true;
    LOGF("[cyd] wifi up, ip %s\n", WiFi.localIP().toString().c_str());
  }
  pollTouch();
  handleSerial();
  syncState();
  delay(10);
}
