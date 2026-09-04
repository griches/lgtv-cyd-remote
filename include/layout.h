// Data-driven remote layout: pages of tiles defined in JSON, stored in NVS,
// editable from the built-in web page. The default layout is compiled in.
#pragma once
#include <Arduino.h>

enum class TileKind : uint8_t { None, Power, Button, Ssap, App, Input, Mute, PlayPause, Screen, VolumeUp, VolumeDown };

struct TileDef {
  TileKind kind = TileKind::None;
  char icon[28] = {0};     // icon registry name; empty = derive from kind/arg
  char label[24] = {0};    // text drawn on a blank key when there is no art
  char arg[48] = {0};      // button name / ssap uri / app id / input id
  char payload[96] = {0};  // ssap payload json
  bool repeat = false;     // auto-repeat while held
};

static const int LAYOUT_COLS = 4, LAYOUT_ROWS = 3, LAYOUT_TILES = LAYOUT_COLS * LAYOUT_ROWS;
static const int LAYOUT_MAX_PAGES = 6;

struct PageDef {
  char name[16] = {0};
  TileDef tiles[LAYOUT_TILES];
};

struct Layout {
  PageDef pages[LAYOUT_MAX_PAGES];
  int pageCount = 0;
};

namespace layout {
  extern Layout current;
  extern volatile uint32_t generation;   // bumps whenever `current` changes

  void begin();                                   // NVS or compiled default
  bool loadJson(const char* json, String* err);   // parse into `current` (does not save)
  String toJson(const Layout& l = current);
  bool save();                                    // persist `current` to NVS
  void resetDefault();                            // compiled default, saved
  const char* defaultJson();

  const char* kindName(TileKind k);
  TileKind kindFromName(const char* n);
  const uint16_t* iconByName(const char* name);   // nullptr if unknown
  // Resolve the art for a tile. Returns nullptr when the tile should be drawn
  // as a blank key with `labelOut` on it.
  const uint16_t* tileIcon(const TileDef& t, bool altState, const char** labelOut);
  bool tileHasArt(const TileDef& t);
}
