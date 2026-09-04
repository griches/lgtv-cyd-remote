// The layout the remote ships with. Edit on the device's web page instead of
// here unless you want to change the factory default.
#pragma once
#include <Arduino.h>

static const char DEFAULT_LAYOUT_JSON[] PROGMEM = R"json({"pages":[
{"name":"Remote","tiles":[
 {"kind":"power"},{"kind":"button","arg":"UP"},{"kind":"playpause"},{"kind":"volume_up"},
 {"kind":"button","arg":"LEFT"},{"kind":"button","arg":"ENTER"},{"kind":"button","arg":"RIGHT"},{"kind":"volume_down"},
 {"kind":"button","arg":"BACK"},{"kind":"button","arg":"DOWN"},{"kind":"button","arg":"HOME"},{"kind":"mute"}]},
{"name":"Apps","tiles":[
 {"kind":"app","arg":"netflix"},{"kind":"app","arg":"youtube.leanback.v4"},{"kind":"app","arg":"com.disney.disneyplus-prod"},{"kind":"app","arg":"amazon"},
 {"kind":"app","arg":"com.apple.appletv"},{"kind":"app","arg":"bbc.iplayer.3.0"},{"kind":"app","arg":"com.fvp.itv"},{"kind":"app","arg":"com.channel4.ondemand"},
 {"kind":"app","arg":"demand5"},{"kind":"app","arg":"now.tv"},{"kind":"app","arg":"spotify-beehive"},{"kind":"app","arg":"plex"}]},
{"name":"Extras","tiles":[
 {"kind":"button","arg":"MENU"},{"kind":"button","arg":"EXIT"},{"kind":"screen"},{"kind":"app","arg":"airplay"},
 {"kind":"input","arg":"HDMI_1"},{"kind":"input","arg":"HDMI_2"},{"kind":"input","arg":"HDMI_3"},{"kind":"input","arg":"HDMI_4"},
 {"kind":"app","arg":"com.webos.app.livetv"},{"kind":"app","arg":"com.webos.app.photovideo"},{"kind":"app","arg":"com.webos.app.music"},{"kind":"app","arg":"com.bskyb.skystore"}]}
]})json";
