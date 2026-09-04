#include "layout.h"
#include "default_layout.h"
#include "assets.h"
#include "log.h"
#include <ArduinoJson.h>
#include <Preferences.h>

namespace layout {

Layout current;
volatile uint32_t generation = 0;

static const char* KIND_NAMES[] = {"none", "power", "button", "ssap", "app", "input", "mute", "playpause", "screen", "volume_up", "volume_down"};

const char* kindName(TileKind k) { return KIND_NAMES[(int)k]; }

TileKind kindFromName(const char* n) {
  for (int i = 0; i < (int)(sizeof(KIND_NAMES) / sizeof(KIND_NAMES[0])); i++)
    if (!strcmp(n, KIND_NAMES[i])) return (TileKind)i;
  return TileKind::None;
}

const char* defaultJson() { return DEFAULT_LAYOUT_JSON; }

const uint16_t* iconByName(const char* name) {
  if (!name || !*name) return nullptr;
  for (int i = 0; i < ICON_COUNT; i++) if (!strcmp(ICONS[i].name, name)) return ICONS[i].icon;
  return nullptr;
}

static const char* buttonIconName(const char* btn) {
  struct { const char* b; const char* i; } map[] = {
    {"UP", "up"}, {"DOWN", "down"}, {"LEFT", "left"}, {"RIGHT", "right"}, {"ENTER", "ok"},
    {"BACK", "back"}, {"HOME", "home"}, {"EXIT", "exit"}, {"MENU", "settings"},
    {"PLAY", "play"}, {"PAUSE", "pause"}, {"VOLUMEUP", "vol_up"}, {"VOLUMEDOWN", "vol_down"}, {"MUTE", "unmuted"},
  };
  for (auto& m : map) if (!strcasecmp(btn, m.b)) return m.i;
  return nullptr;
}

const uint16_t* tileIcon(const TileDef& t, bool altState, const char** labelOut) {
  *labelOut = t.label[0] ? t.label : t.arg;
  const uint16_t* explicitIcon = iconByName(t.icon);
  if (explicitIcon) return explicitIcon;
  switch (t.kind) {
    case TileKind::Power:      return iconByName("power");
    case TileKind::Mute:       return iconByName(altState ? "muted" : "unmuted");
    case TileKind::PlayPause:  return iconByName(altState ? "pause" : "play");
    case TileKind::Screen:     return iconByName(altState ? "screen_on" : "screen_off");
    case TileKind::VolumeUp:   return iconByName("vol_up");
    case TileKind::VolumeDown: return iconByName("vol_down");
    case TileKind::Button:     { const char* n = buttonIconName(t.arg); return n ? iconByName(n) : nullptr; }
    case TileKind::App:        return iconByName(t.arg);   // app art by id, else blank + label
    default:                   return nullptr;             // input / ssap: blank + label
  }
}

bool tileHasArt(const TileDef& t) {
  const char* l;
  return tileIcon(t, false, &l) != nullptr;
}

bool loadJson(const char* json, String* err) {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, json);
  if (e) { if (err) *err = e.c_str(); return false; }
  JsonArrayConst pages = doc["pages"].as<JsonArrayConst>();
  if (pages.isNull() || pages.size() == 0) { if (err) *err = "no pages"; return false; }
  if (pages.size() > LAYOUT_MAX_PAGES) { if (err) *err = "too many pages"; return false; }

  // Static scratch copy: Layout is ~14 KB, far too big for the loop task stack.
  static Layout l;
  l = Layout();
  for (JsonObjectConst p : pages) {
    PageDef& pd = l.pages[l.pageCount++];
    strlcpy(pd.name, p["name"] | "Page", sizeof(pd.name));
    int i = 0;
    for (JsonObjectConst t : p["tiles"].as<JsonArrayConst>()) {
      if (i >= LAYOUT_TILES) break;
      TileDef& td = pd.tiles[i++];
      td.kind = kindFromName(t["kind"] | "none");
      strlcpy(td.icon, t["icon"] | "", sizeof(td.icon));
      strlcpy(td.label, t["label"] | "", sizeof(td.label));
      strlcpy(td.arg, t["arg"] | "", sizeof(td.arg));
      strlcpy(td.payload, t["payload"] | "", sizeof(td.payload));
      bool defRepeat = td.kind == TileKind::VolumeUp || td.kind == TileKind::VolumeDown ||
                       (td.kind == TileKind::Button && (!strcasecmp(td.arg, "UP") || !strcasecmp(td.arg, "DOWN") ||
                                                        !strcasecmp(td.arg, "LEFT") || !strcasecmp(td.arg, "RIGHT")));
      td.repeat = t["repeat"] | defRepeat;
    }
  }
  current = l;
  generation++;
  return true;
}

String toJson(const Layout& l) {
  JsonDocument doc;
  JsonArray pages = doc["pages"].to<JsonArray>();
  for (int p = 0; p < l.pageCount; p++) {
    JsonObject po = pages.add<JsonObject>();
    po["name"] = l.pages[p].name;
    JsonArray tiles = po["tiles"].to<JsonArray>();
    for (int i = 0; i < LAYOUT_TILES; i++) {
      const TileDef& t = l.pages[p].tiles[i];
      JsonObject to = tiles.add<JsonObject>();
      to["kind"] = kindName(t.kind);
      if (t.icon[0]) to["icon"] = t.icon;
      if (t.label[0]) to["label"] = t.label;
      if (t.arg[0]) to["arg"] = t.arg;
      if (t.payload[0]) to["payload"] = t.payload;
      if (t.repeat) to["repeat"] = true;
    }
  }
  String out;
  serializeJson(doc, out);
  return out;
}

bool save() {
  String json = toJson();
  Preferences prefs;
  prefs.begin("layout", false);
  size_t n = prefs.putBytes("json", json.c_str(), json.length());
  prefs.end();
  LOGF("[layout] saved %u bytes\n", (unsigned)n);
  return n == json.length();
}

void resetDefault() {
  String err;
  loadJson(defaultJson(), &err);
  Preferences prefs;
  prefs.begin("layout", false);
  prefs.remove("json");
  prefs.end();
}

void begin() {
  Preferences prefs;
  prefs.begin("layout", true);
  size_t n = prefs.getBytesLength("json");
  String json;
  if (n) {
    char* buf = (char*)malloc(n + 1);
    if (buf) { prefs.getBytes("json", buf, n); buf[n] = 0; json = buf; free(buf); }
  }
  prefs.end();
  String err;
  if (json.length() && loadJson(json.c_str(), &err)) { LOGF("[layout] loaded %d pages from NVS\n", current.pageCount); return; }
  if (json.length()) LOGF("[layout] stored layout invalid (%s), using default\n", err.c_str());
  loadJson(defaultJson(), &err);
  LOGF("[layout] default layout, %d pages\n", current.pageCount);
}

}  // namespace layout
