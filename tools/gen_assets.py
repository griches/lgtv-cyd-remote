#!/usr/bin/env python3
"""Convert Stream Deck plugin PNG artwork into RGB565 C arrays for TFT_eSPI.

Reads 144x144 key art from the lgtv-streamdeck plugin, composites it over
black, resizes to ICON px, and writes src/assets.cpp + include/assets.h.
"""
import os, sys, pathlib
from PIL import Image

# Path to the lgtv-streamdeck plugin's imgs folder (override with LGTV_PLUGIN_IMGS).
PLUGIN = pathlib.Path(os.environ.get(
    "LGTV_PLUGIN_IMGS",
    "/Users/garyriches/Documents/.Source.nosync/lgtv-streamdeck/mobi.bouncingball.smartremote.sdPlugin/imgs"))
ROOT = pathlib.Path(__file__).resolve().parent.parent
ICON = 64
RADIUS = 12   # rounded-corner radius at ICON px, applied to every tile (the screen bg is black)

# (c identifier, relative png path)
ACTIONS = [
    ("ic_power",      "actions/power/key.png"),
    ("ic_vol_up",     "actions/volume-up/key.png"),
    ("ic_vol_down",   "actions/volume-down/key.png"),
    ("ic_unmuted",    "actions/mute/unmuted.png"),
    ("ic_muted",      "actions/mute/muted.png"),
    ("ic_up",         "actions/nav/up.png"),
    ("ic_down",       "actions/nav/down.png"),
    ("ic_left",       "actions/nav/left.png"),
    ("ic_right",      "actions/nav/right.png"),
    ("ic_ok",         "actions/nav/ok.png"),
    ("ic_back",       "actions/nav/back.png"),
    ("ic_home",       "actions/nav/home.png"),
    ("ic_exit",       "actions/nav/exit.png"),
    ("ic_settings",   "actions/nav/settings.png"),
    ("ic_play",       "actions/playpause/play.png"),
    ("ic_pause",      "actions/playpause/pause.png"),
    ("ic_screen_off", "actions/screen/off.png"),
    ("ic_screen_on",  "actions/screen/on.png"),
    ("ic_input",      "actions/input/key.png"),
]

# webOS app ids whose art we bundle (identifier derived from the id)
APPS = [
    "netflix", "youtube.leanback.v4", "com.disney.disneyplus-prod", "amazon",
    "com.apple.appletv", "bbc.iplayer.3.0", "com.fvp.itv", "com.channel4.ondemand",
    "demand5", "now.tv", "com.webos.app.igallery", "spotify-beehive",
    "plex",
    "com.webos.app.livetv", "airplay", "com.webos.app.photovideo", "com.webos.app.music",
    "com.bskyb.skystore",
]

# Generated blank key tile in the plugin's style; the firmware draws input
# labels (from the TV) on top of it at runtime.

def cname(app_id):
    return "app_" + "".join(c if c.isalnum() else "_" for c in app_id)

def rounded_mask(size, radius):
    # Supersampled for smooth anti-aliased corners.
    from PIL import ImageDraw
    ss = 4
    m = Image.new("L", (size * ss, size * ss), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, size * ss - 1, size * ss - 1], radius=radius * ss, fill=255)
    return m.resize((size, size), Image.LANCZOS)

_MASK = None

def key_style():
    """Sample the plugin's key art for its tile background and glyph colours."""
    im = Image.open(PLUGIN / "actions/power/key.png").convert("RGBA")
    w, h = im.size
    bg_top = im.getpixel((w // 2, 10))[:3]
    bg_mid = im.getpixel((14, h // 2))[:3]
    bg_bot = im.getpixel((w // 2, h - 10))[:3]
    glyph = max((im.getpixel((x, y))[:3] for x in range(0, w, 3) for y in range(0, h, 3)), key=sum)
    return bg_top, bg_mid, bg_bot, glyph

def render_blank_key():
    """A 144px empty key in the plugin's style (background only)."""
    from PIL import ImageDraw, ImageFont
    size = 144
    bg_top, bg_mid, bg_bot, glyph = key_style()
    im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    # vertical gradient tile
    grad = Image.new("RGBA", (size, size))
    gp = grad.load()
    for y in range(size):
        t = y / (size - 1)
        c = tuple(int(bg_top[i] * (1 - t) + bg_bot[i] * t) for i in range(3)) + (255,)
        for x in range(size):
            gp[x, y] = c
    tile_mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(tile_mask).rounded_rectangle([4, 4, size - 5, size - 5], radius=28, fill=255)
    im.paste(grad, (0, 0), tile_mask)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([4, 4, size - 5, size - 5], radius=28, outline=tuple(min(255, c + 22) for c in bg_mid) + (255,), width=2)
    def font(px):
        for f in ("/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/System/Library/Fonts/Helvetica.ttc",
                  "/Library/Fonts/Arial Bold.ttf"):
            try:
                return ImageFont.truetype(f, px)
            except OSError:
                continue
        return ImageFont.load_default()
    return im

def to_rgb565(path):
    global _MASK
    if _MASK is None:
        _MASK = rounded_mask(ICON, RADIUS)
    im = path if isinstance(path, Image.Image) else Image.open(path).convert("RGBA")
    im = im.resize((ICON, ICON), Image.LANCZOS)
    # Clip to a rounded rectangle, then flatten onto black.
    a = im.getchannel("A")
    from PIL import ImageChops
    im.putalpha(ImageChops.multiply(a, _MASK))
    bg = Image.new("RGBA", im.size, (0, 0, 0, 255))
    im = Image.alpha_composite(bg, im).convert("RGB")
    out = []
    for r, g, b in im.getdata():
        out.append(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    return out

def emit(cpp, h, ident, pixels):
    h.write(f"extern const uint16_t {ident}[{ICON * ICON}] PROGMEM;\n")
    cpp.write(f"const uint16_t {ident}[{ICON * ICON}] PROGMEM = {{\n")
    for i in range(0, len(pixels), 16):
        cpp.write("  " + ",".join(f"0x{p:04X}" for p in pixels[i:i + 16]) + ",\n")
    cpp.write("};\n\n")

def main():
    inc = ROOT / "include" / "assets.h"
    src = ROOT / "src" / "assets.cpp"
    with inc.open("w") as h, src.open("w") as cpp:
        h.write("// Generated by tools/gen_assets.py — do not edit.\n#pragma once\n#include <Arduino.h>\n\n")
        h.write(f"#define ICON_W {ICON}\n#define ICON_H {ICON}\n\n")
        cpp.write('// Generated by tools/gen_assets.py — do not edit.\n#include "assets.h"\n\n')
        for ident, rel in ACTIONS:
            emit(cpp, h, ident, to_rgb565(PLUGIN / rel))
        h.write("\n// Generated blank key (labels drawn at runtime)\n")
        emit(cpp, h, "ic_key_blank", to_rgb565(render_blank_key()))
        g = key_style()[3]
        h.write(f"#define KEY_GLYPH_COLOUR 0x{((g[0] & 0xF8) << 8) | ((g[1] & 0xFC) << 3) | (g[2] >> 3):04X}\n")
        h.write("\n// App art, keyed by webOS app id\n")
        for app in APPS:
            emit(cpp, h, cname(app), to_rgb565(PLUGIN / "apps" / f"{app}.png"))
        # Registry: every icon by name (action icons by short name, app art by app id)
        names = [(ident[3:], ident) for ident, _ in ACTIONS] + [("key_blank", "ic_key_blank")] + [(app, cname(app)) for app in APPS]
        h.write("\n// Icon registry for layouts: action icons by short name, app art by app id\n")
        h.write("struct IconEntry { const char* name; const uint16_t* icon; };\n")
        h.write(f"#define ICON_COUNT {len(names)}\nextern const IconEntry ICONS[ICON_COUNT];\n")
        cpp.write("const IconEntry ICONS[ICON_COUNT] = {\n")
        for name, ident in names:
            cpp.write(f'  {{"{name}", {ident}}},\n')
        cpp.write("};\n")
    total = (len(ACTIONS) + 1 + len(APPS)) * ICON * ICON * 2
    print(f"wrote {len(ACTIONS)} action icons + blank key + {len(APPS)} app icons ({total // 1024} KB) -> {src.relative_to(ROOT)}, {inc.relative_to(ROOT)}")

if __name__ == "__main__":
    main()
