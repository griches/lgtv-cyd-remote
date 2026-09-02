// LG TV remote for the ESP32-2432S028 "Cheap Yellow Display".
//
// A touch port of the lgtv-streamdeck plugin: same actions, same artwork,
// same webOS protocol, same pairings (imported from lgtvremote-cli, or made
// on the device via scan + PIN).
//
// Layout: 4x3 grid of 64px key tiles (80x72 cells) above a 24px status bar.
// Tap the left of the bar (the TV name) to open the TV list, the right of
// the bar to change page.

#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

#include "assets.h"
#include "lgtv.h"
#include "log.h"
#include "secrets.h"
#include "tvstore.h"

// ------------------------------------------------------------- hardware

static TFT_eSPI tft;
static SPIClass touchSPI(VSPI);
static XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
static TVStore store;
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
static const uint16_t TILE_BG = 0x18E3;           // dark tile for text buttons
static const uint16_t ACCENT = 0x3D9F;            // blue
static const uint16_t GREEN = 0x2E8B;

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
#define T_REQP(icon, uri, json) {icon, nullptr, CmdType::Request, uri, json, Dyn::None, false}
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
    T_BTN1(ic_settings, "MENU"),
    T_BTN1(ic_exit, "EXIT"),
    {ic_screen_off, ic_screen_on, CmdType::ScreenToggle, "", "", Dyn::Screen, false},
    T_APP(app_airplay, "airplay"),

    T_REQP(ic_hdmi1, "ssap://tv/switchInput", "{\"inputId\":\"HDMI_1\"}"),
    T_REQP(ic_hdmi2, "ssap://tv/switchInput", "{\"inputId\":\"HDMI_2\"}"),
    T_REQP(ic_hdmi3, "ssap://tv/switchInput", "{\"inputId\":\"HDMI_3\"}"),
    T_REQP(ic_hdmi4, "ssap://tv/switchInput", "{\"inputId\":\"HDMI_4\"}"),

    T_APP(app_com_webos_app_livetv, "com.webos.app.livetv"),
    T_APP(app_com_webos_app_photovideo, "com.webos.app.photovideo"),
    T_APP(app_com_webos_app_music, "com.webos.app.music"),
    T_APP(app_com_bskyb_skystore, "com.bskyb.skystore"),
  }},
};
static const int PAGE_COUNT = sizeof(PAGES) / sizeof(PAGES[0]);

// ---------------------------------------------------------------- state

enum class Mode : uint8_t { Grid, TVList, Keypad };
static Mode mode = Mode::Grid;
static int page = 0;
static int pressedTile = -1;        // grid: index in current page, -1 = none
static uint32_t pressStart = 0, lastRepeat = 0;

// TV list
static const int ROW_H = 36;
static int confirmForget = -1;      // row index awaiting a second tap on its [x]
static uint32_t confirmUntil = 0;
static uint32_t shownStoreGen = 0, shownFoundGen = 0;

// Keypad
static char pin[9] = {0};
static char pairName[32] = {0};
static char pairIp[16] = {0};
static enum { PK_Connecting, PK_Enter, PK_Checking, PK_Wrong, PK_Done } pkState = PK_Connecting;
static uint32_t pkDoneAt = 0;

// Snapshots of LGTV state so we redraw only on change.
static LinkState shownLink = LinkState::NoWifi;
static bool shownMuted = false, shownPlaying = false, shownScreenOff = false;
static int shownTV = -2;
static uint32_t shownError = 0;
static uint32_t errorFlashUntil = 0;

// ------------------------------------------------------------- helpers

static bool dynOn(Dyn d) {
  switch (d) {
    case Dyn::Mute:   return lg.muted.load();
    case Dyn::Play:   return lg.playing.load();
    case Dyn::Screen: return lg.screenOff.load();
    default:          return false;
  }
}

static uint16_t linkColour(LinkState ls) {
  switch (ls) {
    case LinkState::Registered: return TFT_GREEN;
    case LinkState::Connecting: return TFT_YELLOW;
    case LinkState::NeedsPin:   return ACCENT;
    case LinkState::NoTV:       return TFT_RED;
    default:                    return TFT_DARKGREY;
  }
}

static void textButton(int x, int y, int w, int h, const char* label, uint16_t bg, uint16_t fg, int font = 2) {
  tft.fillRoundRect(x, y, w, h, 8, bg);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(fg, bg);
  tft.drawString(label, x + w / 2, y + h / 2, font);
}

// ---------------------------------------------------------- grid view

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

// ------------------------------------------------------------ TV list

// Rows: stored TVs, then the scan button, then discovered TVs not yet stored.
struct ListRow { enum { Stored, Scan, Found } kind; int idx; };
static ListRow rows[COLS * ROWS];
static int rowCount = 0;

static void buildRows() {
  rowCount = 0;
  int maxRows = GRID_H / ROW_H;  // 6
  int n = store.count();
  for (int i = 0; i < n && rowCount < maxRows - 1; i++) rows[rowCount++] = {ListRow::Stored, i};
  rows[rowCount++] = {ListRow::Scan, 0};
  int f = lg.foundCount();
  for (int i = 0; i < f && rowCount < maxRows; i++) {
    FoundTV ft;
    if (!lg.getFound(i, ft)) continue;
    if (store.findByIp(ft.ip) >= 0) continue;
    rows[rowCount++] = {ListRow::Found, i};
  }
}

static void drawListRow(int r) {
  int y = r * ROW_H;
  tft.fillRect(0, y, tft.width(), ROW_H, BG);
  const ListRow& row = rows[r];
  if (row.kind == ListRow::Stored) {
    TVRecord rec;
    if (!store.get(row.idx, rec)) return;
    bool sel = row.idx == store.selected();
    if (sel) tft.fillRoundRect(2, y + 2, tft.width() - 4, ROW_H - 4, 6, TILE_BG);
    tft.fillCircle(14, y + ROW_H / 2, 4, sel ? linkColour(lg.link.load()) : TFT_DARKGREY);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_WHITE, sel ? TILE_BG : BG);
    tft.drawString(rec.name, 26, y + ROW_H / 2, 2);
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, sel ? TILE_BG : BG);
    tft.drawString(rec.clientKey[0] ? rec.ip : "not paired", 268, y + ROW_H / 2, 1);
    bool confirm = confirmForget == r && millis() < confirmUntil;
    textButton(280, y + 5, 36, ROW_H - 10, confirm ? "sure?" : "x", confirm ? TFT_RED : 0x4208, TFT_WHITE, 1);
  } else if (row.kind == ListRow::Scan) {
    bool busy = lg.scanning.load();
    textButton(6, y + 4, tft.width() - 12, ROW_H - 8, busy ? "Scanning..." : "Scan for TVs", busy ? 0x4208 : ACCENT, TFT_WHITE);
  } else {
    FoundTV ft;
    if (!lg.getFound(row.idx, ft)) return;
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(GREEN, BG);
    tft.drawString("+", 12, y + ROW_H / 2, 4);
    tft.setTextColor(TFT_WHITE, BG);
    tft.drawString(ft.name, 30, y + ROW_H / 2, 2);
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, BG);
    tft.drawString(ft.ip, tft.width() - 8, y + ROW_H / 2, 1);
  }
}

static void drawList() {
  buildRows();
  tft.fillRect(0, 0, tft.width(), GRID_H, BG);
  for (int r = 0; r < rowCount; r++) drawListRow(r);
  if (store.count() == 0 && lg.foundCount() == 0 && !lg.scanning.load()) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, BG);
    tft.drawString("No TVs yet. Scan to find one.", tft.width() / 2, GRID_H - 30, 2);
  }
}

// ------------------------------------------------------------- keypad

static const int KP_X = 140, KP_W = 60, KP_H = 54;
static const char* KP_KEYS[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "<", "0", "OK"};

static void drawKeypad() {
  tft.fillRect(0, 0, tft.width(), GRID_H, BG);
  // Left panel
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, BG);
  tft.drawString("Pair with", 8, 8, 2);
  tft.setTextColor(TFT_WHITE, BG);
  tft.drawString(pairName, 8, 26, 2);
  tft.setTextColor(TFT_DARKGREY, BG);
  tft.drawString(pairIp, 8, 44, 1);
  // PIN box
  tft.fillRoundRect(8, 66, KP_X - 16, 34, 6, TILE_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TILE_BG);
  tft.drawString(pin[0] ? pin : "", 8 + (KP_X - 16) / 2, 83, 4);
  // Status
  const char* st = pkState == PK_Connecting ? "Connecting..."
                 : pkState == PK_Enter      ? "Enter the PIN on the TV"
                 : pkState == PK_Checking   ? "Checking..."
                 : pkState == PK_Wrong      ? "Wrong PIN, try again"
                                            : "Paired!";
  uint16_t sc = pkState == PK_Wrong ? TFT_RED : pkState == PK_Done ? TFT_GREEN : TFT_LIGHTGREY;
  tft.fillRect(0, 108, KP_X, 50, BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(sc, BG);
  // wrap by hand: two short lines max
  String s(st);
  int sp = s.length() > 16 ? s.lastIndexOf(' ', 16) : -1;
  if (sp > 0) { tft.drawString(s.substring(0, sp), 8, 110, 2); tft.drawString(s.substring(sp + 1), 8, 128, 2); }
  else tft.drawString(s, 8, 110, 2);
  textButton(8, GRID_H - 44, KP_X - 16, 36, "Cancel", 0x4208, TFT_WHITE);
  // Keys
  for (int i = 0; i < 12; i++) {
    int x = KP_X + (i % 3) * KP_W, y = (i / 3) * KP_H;
    bool ok = i == 11;
    textButton(x + 3, y + 3, KP_W - 6, KP_H - 6, KP_KEYS[i], ok ? ACCENT : TILE_BG, TFT_WHITE, 4);
  }
}

static void drawPinOnly() {
  tft.fillRoundRect(8, 66, KP_X - 16, 34, 6, TILE_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TILE_BG);
  tft.drawString(pin[0] ? pin : "", 8 + (KP_X - 16) / 2, 83, 4);
}

// ----------------------------------------------------------------- bar

static void drawBar() {
  tft.fillRect(0, BAR_Y, tft.width(), BAR_H, BAR_BG);
  LinkState ls = lg.link.load();
  uint16_t dot = linkColour(ls);
  if (millis() < errorFlashUntil) dot = TFT_RED;
  tft.fillCircle(10, BAR_Y + BAR_H / 2, 4, dot);

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, BAR_BG);
  char left[40];
  if (mode == Mode::TVList) snprintf(left, sizeof(left), "TVs");
  else if (mode == Mode::Keypad) snprintf(left, sizeof(left), "Pairing");
  else if (ls == LinkState::NoWifi) snprintf(left, sizeof(left), WiFi.status() == WL_CONNECTED ? "wifi ok" : "wifi...");
  else {
    TVRecord rec;
    if (store.get(store.selected(), rec)) snprintf(left, sizeof(left), "%s", rec.name);
    else snprintf(left, sizeof(left), "No TV - tap to add");
  }
  tft.drawString(left, 20, BAR_Y + BAR_H / 2, 2);

  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, BAR_BG);
  char right[32];
  if (mode == Mode::Grid) snprintf(right, sizeof(right), "%s  %d/%d >", PAGES[page].name, page + 1, PAGE_COUNT);
  else snprintf(right, sizeof(right), mode == Mode::TVList ? "Back" : "Cancel");
  tft.drawString(right, tft.width() - 6, BAR_Y + BAR_H / 2, 2);
}

static void setMode(Mode m) {
  mode = m;
  pressedTile = -1;
  confirmForget = -1;
  tft.fillRect(0, 0, tft.width(), GRID_H, BG);
  if (mode == Mode::Grid) drawGrid();
  else if (mode == Mode::TVList) drawList();
  else drawKeypad();
  drawBar();
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

static void startPairing(const char* ip, const char* name) {
  strlcpy(pairIp, ip, sizeof(pairIp));
  strlcpy(pairName, name, sizeof(pairName));
  pin[0] = 0;
  pkState = PK_Connecting;
  lg.post(CmdType::PairStart, ip, name);
  setMode(Mode::Keypad);
}

static void onPressGrid(int x, int y) {
  if (y >= BAR_Y) {
    if (x < tft.width() / 2) setMode(Mode::TVList);
    else { page = (page + 1) % PAGE_COUNT; drawGrid(); drawBar(); }
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

static void onPressList(int x, int y) {
  if (y >= BAR_Y) { setMode(Mode::Grid); return; }
  int r = y / ROW_H;
  if (r >= rowCount) return;
  const ListRow& row = rows[r];
  char idx[4];
  if (row.kind == ListRow::Stored) {
    if (x >= 280) {
      if (confirmForget == r && millis() < confirmUntil) {
        snprintf(idx, sizeof(idx), "%d", row.idx);
        lg.post(CmdType::Forget, idx);
        confirmForget = -1;
      } else {
        confirmForget = r;
        confirmUntil = millis() + 3000;
        drawListRow(r);
      }
      return;
    }
    TVRecord rec;
    if (!store.get(row.idx, rec)) return;
    if (!rec.clientKey[0]) { startPairing(rec.ip, rec.name); return; }
    snprintf(idx, sizeof(idx), "%d", row.idx);
    lg.post(CmdType::SelectTV, idx);
    setMode(Mode::Grid);
  } else if (row.kind == ListRow::Scan) {
    if (!lg.scanning.load()) { lg.post(CmdType::Scan); drawListRow(r); }
  } else {
    FoundTV ft;
    if (lg.getFound(row.idx, ft)) startPairing(ft.ip, ft.name);
  }
}

static void onPressKeypad(int x, int y) {
  if (y >= BAR_Y || (x < KP_X && y >= GRID_H - 44)) {  // bar or Cancel
    lg.post(CmdType::CancelPair);
    setMode(Mode::TVList);
    return;
  }
  if (x < KP_X) return;
  int i = (y / KP_H) * 3 + (x - KP_X) / KP_W;
  if (i < 0 || i > 11) return;
  if (pkState == PK_Done) return;
  if (i == 9) {                       // backspace
    size_t n = strlen(pin);
    if (n) pin[n - 1] = 0;
    drawPinOnly();
  } else if (i == 11) {               // OK
    if (!pin[0]) return;
    if (pkState != PK_Enter && pkState != PK_Wrong) return;
    pkState = PK_Checking;
    lg.post(CmdType::SubmitPin, pin);
    drawKeypad();
  } else {
    size_t n = strlen(pin);
    if (n < 8) { pin[n] = KP_KEYS[i][0]; pin[n + 1] = 0; drawPinOnly(); }
  }
}

static void onPress(int x, int y) {
  switch (mode) {
    case Mode::Grid:   onPressGrid(x, y); break;
    case Mode::TVList: onPressList(x, y); break;
    case Mode::Keypad: onPressKeypad(x, y); break;
  }
}

static void onHold() {
  if (mode != Mode::Grid || pressedTile < 0) return;
  const Tile& t = PAGES[page].tiles[pressedTile];
  if (!t.repeat) return;
  uint32_t now = millis();
  if (now - pressStart > 450 && now - lastRepeat > 180) {
    lastRepeat = now;
    fire(t);
  }
}

static void onRelease() {
  if (mode == Mode::Grid && pressedTile >= 0) {
    int idx = pressedTile;
    pressedTile = -1;
    drawTile(idx, false);
  }
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
//              RGB bytes (see tools/cyd.py)
//   t X Y      simulate a tap at screen coords
//   p N        jump to page N (0-based)
//   b NAME     send a remote button (UP, ENTER, HOME...) via the pointer socket
//   v          open the TV list       scan     start discovery
//   pair IP    start pairing with IP  pin N    submit PIN
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
    if (!strcmp(line, "scan")) { lg.post(CmdType::Scan); continue; }
    if (!strncmp(line, "pair ", 5)) { startPairing(line + 5, "LG TV"); continue; }
    if (!strncmp(line, "pin ", 4)) { strlcpy(pin, line + 4, sizeof(pin)); pkState = PK_Checking; lg.post(CmdType::SubmitPin, pin); if (mode == Mode::Keypad) drawKeypad(); continue; }
    char* arg = line + 1;
    while (*arg == ' ') arg++;
    switch (line[0]) {
      case 'S': dumpScreen(); break;
      case 'h': LOGF("[dbg] heap %u\n", ESP.getFreeHeap()); break;
      case 'v': setMode(mode == Mode::TVList ? Mode::Grid : Mode::TVList); break;
      case 'p': page = constrain(atoi(arg), 0, PAGE_COUNT - 1); if (mode != Mode::Grid) setMode(Mode::Grid); else { drawGrid(); drawBar(); } LOGF("[dbg] page %d\n", page); break;
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
  bool barDirty = false, gridDirty = false, listDirty = false, keypadDirty = false;

  LinkState ls = lg.link.load();
  if (ls != shownLink) { shownLink = ls; barDirty = true; listDirty = true; keypadDirty = true; }
  int tv = store.selected();
  if (tv != shownTV) { shownTV = tv; barDirty = true; listDirty = true; }
  uint32_t sg = store.generation();
  if (sg != shownStoreGen) { shownStoreGen = sg; barDirty = true; listDirty = true; }
  uint32_t fg = lg.foundGen.load();
  if (fg != shownFoundGen) { shownFoundGen = fg; listDirty = true; }
  static bool shownScanning = false;
  if (lg.scanning.load() != shownScanning) { shownScanning = lg.scanning.load(); listDirty = true; }

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

  static wl_status_t shownWifi = WL_IDLE_STATUS;
  if (WiFi.status() != shownWifi) { shownWifi = WiFi.status(); barDirty = true; }

  // Forget confirmation timeout
  if (confirmForget >= 0 && millis() >= confirmUntil) { confirmForget = -1; listDirty = true; }

  // Pairing state machine (driven by the net task's atomics)
  if (mode == Mode::Keypad) {
    auto prev = pkState;
    if (lg.pairFailed.load() && pkState != PK_Wrong) { pkState = PK_Wrong; pin[0] = 0; }
    else if (ls == LinkState::NeedsPin && (pkState == PK_Connecting || (pkState == PK_Wrong && !lg.pairFailed.load()))) pkState = PK_Enter;
    else if (ls == LinkState::Registered && !lg.pairing.load() && pkState != PK_Done) { pkState = PK_Done; pkDoneAt = millis(); }
    if (pkState != prev) keypadDirty = true;
    if (pkState == PK_Done && millis() - pkDoneAt > 1200) { setMode(Mode::Grid); return; }
  }

  if (mode == Mode::Grid && gridDirty) {
    for (int i = 0; i < COLS * ROWS; i++)
      if (PAGES[page].tiles[i].dyn != Dyn::None) drawTile(i, i == pressedTile);
  }
  if (mode == Mode::TVList && listDirty) drawList();
  if (mode == Mode::Keypad && keypadDirty) drawKeypad();
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

  store.begin();
  drawGrid();
  drawBar();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  LOGF("[cyd] wifi connecting to %s\n", WIFI_SSID);

  lg.begin(&store);
  LOGF("[cyd] %d TVs, selected %d\n", store.count(), store.selected());
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
