// Wi-Fi credentials in NVS. Seeded once from include/secrets.h if that file
// exists (optional); otherwise entered on the device's Wi-Fi setup screen.
#pragma once
#include <Arduino.h>
#include <Preferences.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

struct WifiCreds {
  char ssid[33] = {0};
  char pass[65] = {0};
  bool valid() const { return ssid[0] != 0; }
};

inline WifiCreds wifiLoad() {
  WifiCreds c;
  Preferences p;
  p.begin("wifi", true);
  String s = p.getString("ssid", ""), k = p.getString("pass", "");
  p.end();
  strlcpy(c.ssid, s.c_str(), sizeof(c.ssid));
  strlcpy(c.pass, k.c_str(), sizeof(c.pass));
#ifdef WIFI_SSID
  if (!c.valid() && strcmp(WIFI_SSID, "your-network")) {
    strlcpy(c.ssid, WIFI_SSID, sizeof(c.ssid));
    strlcpy(c.pass, WIFI_PASS, sizeof(c.pass));
  }
#endif
  return c;
}

inline void wifiSave(const char* ssid, const char* pass) {
  Preferences p;
  p.begin("wifi", false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
}
