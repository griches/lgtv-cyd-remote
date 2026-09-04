// Built-in web page: layout editor, live screen preview, status, firmware
// update. Served on port 80; announced as http://lgremote.local via mDNS.
// Also brings up ArduinoOTA so `pio run -t upload --upload-port lgremote.local` works.
#pragma once
#include <TFT_eSPI.h>
#include "lgtv.h"
#include "tvstore.h"

namespace webui {
  void begin(TFT_eSPI* tft, LGTV* lg, TVStore* store);
  void loop();          // call from loop(); serves requests and OTA
  bool started();
}
