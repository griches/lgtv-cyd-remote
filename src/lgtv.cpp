#include "lgtv.h"
#include "registration.h"
#include "log.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>

static const char* TAG = "[lgtv]";

// ---------------------------------------------------------------- public

void LGTV::begin(TVStore* store) {
  store_ = store;
  queue_ = xQueueCreate(8, sizeof(Cmd));
  foundMtx_ = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(taskEntry, "lgtv", 16384, this, 1, nullptr, 0);
}

bool LGTV::post(CmdType t, const char* a, const char* b) {
  Cmd c;
  c.type = t;
  if (a) strlcpy(c.a, a, sizeof(c.a));
  if (b) strlcpy(c.b, b, sizeof(c.b));
  return xQueueSend(queue_, &c, 0) == pdTRUE;
}

int LGTV::foundCount() {
  xSemaphoreTake(foundMtx_, portMAX_DELAY);
  int n = foundCount_;
  xSemaphoreGive(foundMtx_);
  return n;
}

bool LGTV::getFound(int i, FoundTV& out) {
  xSemaphoreTake(foundMtx_, portMAX_DELAY);
  bool ok = i >= 0 && i < foundCount_;
  if (ok) out = found_[i];
  xSemaphoreGive(foundMtx_);
  return ok;
}

// ------------------------------------------------------------------ task

void LGTV::taskEntry(void* self) { static_cast<LGTV*>(self)->task(); }

void LGTV::task() {
  ws_.onEvent([this](WStype_t t, uint8_t* d, size_t l) { onMainEvent(t, d, l); });
  ptr_.onEvent([this](WStype_t t, uint8_t* d, size_t l) { onPointerEvent(t, d, l); });
  ws_.setReconnectInterval(5000);
  ws_.enableHeartbeat(15000, 5000, 2);
  // NB: the library gates connects on (millis() - lastFail) < interval with
  // lastFail starting at 0, so a huge interval blocks the *first* connect
  // too. Keep it short; ptrBegun_ is what stops us looping a dead socket.
  ptr_.setReconnectInterval(1000);

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      if (link.load() != LinkState::NoWifi) {
        LOGF("%s wifi lost\n", TAG);
        link = LinkState::NoWifi;
        dropAll();
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if (!wsBegun_) connectMain();

    if (wsBegun_) ws_.loop();
    if (ptrBegun_) ptr_.loop();
    expirePending();

    Cmd c;
    while (xQueueReceive(queue_, &c, 0) == pdTRUE) handleCmd(c);

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ----------------------------------------------------------- connection

void LGTV::dropAll() {
  if (ptrBegun_) { ptr_.disconnect(); ptrBegun_ = false; }
  ptrOpen_ = false;
  if (wsBegun_) { ws_.disconnect(); wsBegun_ = false; }
  for (auto& p : pending_) p.id = 0;
  pendingButton_[0] = 0;
}

void LGTV::connectMain() {
  curIdx_ = store_->selected();
  if (!store_->get(curIdx_, cur_)) {
    curIdx_ = -1;
    if (link.load() != LinkState::NoTV) { LOGF("%s no TV configured\n", TAG); link = LinkState::NoTV; }
    return;
  }
  link = LinkState::Connecting;
  LOGF("%s connecting to %s (%s) via %s%s\n", TAG, cur_.name, cur_.ip, useTls_ ? "wss:3001" : "ws:3000",
       pairing.load() ? " [pairing]" : "");
  if (useTls_) ws_.beginSSL(cur_.ip, 3001, "/", "", "");
  else         ws_.begin(cur_.ip, 3000, "/", "");
  wsBegun_ = true;
}

void LGTV::sendRegister() {
  bool withKey = cur_.clientKey[0] && !pairing.load();
  String msg;
  msg.reserve(strlen_P(LG_MANIFEST) + 256);
  msg += F("{\"type\":\"register\",\"id\":\"register_0\",\"payload\":{");
  if (withKey) { msg += F("\"client-key\":\""); msg += cur_.clientKey; msg += F("\","); }
  msg += withKey ? F("\"pairingType\":\"prompt\"") : F("\"pairingType\":\"PIN\"");
  msg += F(",\"forcePairing\":false,\"manifest\":");
  msg += FPSTR(LG_MANIFEST);
  msg += F("}}");
  ws_.sendTXT(msg);
}

void LGTV::onRegistered(const char* key) {
  LOGF("%s registered with %s\n", TAG, cur_.name);
  link = LinkState::Registered;
  failStreak_ = 0;
  bool wasPairing = pairing.exchange(false);
  pairFailed = false;

  if (key && *key && strcmp(key, cur_.clientKey)) {
    // New pairing, or the TV rotated the key: persist it (plugin does the same).
    strlcpy(cur_.clientKey, key, sizeof(cur_.clientKey));
    store_->update(curIdx_, cur_);
    LOGF("%s stored %s client key for %s\n", TAG, wasPairing ? "new" : "rotated", cur_.name);
  }

  // Live mute/volume state: subscribe, so the tile follows the TV remote too.
  String sub = F("{\"type\":\"subscribe\",\"id\":\"sub_volume\",\"uri\":\"ssap://audio/getVolume\"}");
  ws_.sendTXT(sub);

  // Self-heal missing MACs so power-off never strands the TV unwakeable.
  if (!cur_.mac[0] && !cur_.wifiMac[0]) fetchMacs(false);
}

static bool formatMac(const char* in, char* out, size_t outLen) {
  char hex[13]; int n = 0;
  for (const char* p = in; *p && n < 12; p++) if (isxdigit((unsigned char)*p)) hex[n++] = tolower((unsigned char)*p);
  hex[n] = 0;
  if (n != 12 || !strcmp(hex, "000000000000")) return false;
  snprintf(out, outLen, "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c", hex[0], hex[1], hex[2], hex[3], hex[4], hex[5],
           hex[6], hex[7], hex[8], hex[9], hex[10], hex[11]);
  return true;
}

void LGTV::fetchMacs(bool secondTry) {
  const char* uri = secondTry ? "ssap://com.webos.service.connectionmanager/getStatus"
                              : "ssap://com.webos.service.connectionmanager/getinfo";
  request(uri, nullptr, [this, secondTry](bool ok, JsonObjectConst r) {
    char mac[18] = {0}, wmac[18] = {0};
    if (ok) {
      const char* sections[][2] = {{"wiredInfo", "mac"}, {"wifiInfo", "wifi"}, {"wired", "mac"}, {"wifi", "wifi"}};
      for (auto& s : sections) {
        const char* m = r[s[0]]["macAddress"] | "";
        if (*m) { if (!strcmp(s[1], "mac")) { if (!mac[0]) formatMac(m, mac, sizeof(mac)); } else if (!wmac[0]) formatMac(m, wmac, sizeof(wmac)); }
      }
      if (!mac[0]) { const char* m = r["macAddress"] | (r["wiredMacAddress"] | ""); if (*m) formatMac(m, mac, sizeof(mac)); }
      if (!wmac[0]) { const char* m = r["wifiMacAddress"] | ""; if (*m) formatMac(m, wmac, sizeof(wmac)); }
    }
    if (mac[0] || wmac[0]) {
      strlcpy(cur_.mac, mac, sizeof(cur_.mac));
      strlcpy(cur_.wifiMac, wmac, sizeof(cur_.wifiMac));
      store_->update(curIdx_, cur_);
      LOGF("%s stored MACs for %s: %s / %s\n", TAG, cur_.name, mac[0] ? mac : "-", wmac[0] ? wmac : "-");
    } else if (!secondTry) {
      fetchMacs(true);
    } else {
      LOGF("%s MAC fetch returned nothing for %s\n", TAG, cur_.name);
    }
  }, 5000);
}

void LGTV::onMainEvent(WStype_t type, uint8_t* data, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      LOGF("%s socket open, registering\n", TAG);
      sendRegister();
      break;

    case WStype_DISCONNECTED:
      if (link.load() == LinkState::Registered) LOGF("%s disconnected\n", TAG);
      else if (++failStreak_ >= 2) {
        // Same fallback as the plugin: old firmware only speaks plain ws:3000.
        failStreak_ = 0;
        useTls_ = !useTls_;
        ws_.disconnect();
        wsBegun_ = false;
      }
      if (link.load() != LinkState::NoTV) link = LinkState::Connecting;
      ptrOpen_ = false;
      if (ptrBegun_) { ptr_.disconnect(); ptrBegun_ = false; }
      for (auto& p : pending_) p.id = 0;
      break;

    case WStype_TEXT: {
      JsonDocument doc;
      if (deserializeJson(doc, data, len)) return;
      const char* mtype = doc["type"] | "";
      const char* id = doc["id"] | "";
      JsonObjectConst payload = doc["payload"].as<JsonObjectConst>();
      lastActivity = millis();

      if (!strcmp(mtype, "registered") || (!strcmp(mtype, "response") && payload["client-key"].is<const char*>())) {
        onRegistered(payload["client-key"] | "");
        return;
      }
      if (link.load() != LinkState::Registered) {
        const char* pt = payload["pairingType"] | "";
        if (!strcasecmp(pt, "pin")) {
          LOGF("%s TV is showing a PIN\n", TAG);
          link = LinkState::NeedsPin;
          return;
        }
        if (!strcmp(mtype, "error")) {
          const char* err = doc["error"] | "?";
          LOGF("%s registration error: %s\n", TAG, err);
          if (pairing.load()) {
            // Wrong PIN (or prompt dismissed). Reconnect so the TV shows a fresh PIN.
            pairFailed = true;
            ws_.disconnect();
            wsBegun_ = false;
          } else if (strstr(err, "403") || strstr(err, "401")) {
            // Stored key refused: the TV forgot us. Fall into pairing mode so the
            // UI can offer to re-pair rather than spinning forever.
            LOGF("%s stored key rejected by %s\n", TAG, cur_.name);
          }
          return;
        }
      }

      if (!strcmp(id, "sub_volume")) {
        bool m = payload["muteStatus"] | (payload["volumeStatus"]["muteStatus"] | false);
        muted = m;
        return;
      }
      uint32_t nid = (uint32_t)strtoul(id, nullptr, 10);
      for (auto& p : pending_) {
        if (p.id && p.id == nid) {
          p.id = 0;
          bool ok = strcmp(mtype, "error") != 0;
          if (!ok) LOGF("%s error for #%u: %s\n", TAG, nid, doc["error"] | "?");
          if (p.cb) p.cb(ok, payload);
          break;
        }
      }
      break;
    }

    case WStype_ERROR:
      LOGF("%s socket error: %.*s\n", TAG, (int)len, (const char*)data);
      break;
    default:
      break;
  }
}

uint32_t LGTV::request(const char* uri, const char* payloadJson, ResponseCb cb, uint32_t timeoutMs) {
  bool open = wsBegun_ && ws_.isConnected();
  if (!open || (link.load() != LinkState::Registered && link.load() != LinkState::NeedsPin)) {
    lastError = millis();
    if (cb) cb(false, JsonObjectConst());
    return 0;
  }
  uint32_t id = nextId_++;
  String msg;
  msg.reserve(96 + strlen(uri) + (payloadJson ? strlen(payloadJson) : 0));
  msg += F("{\"type\":\"request\",\"id\":\"");
  msg += id;
  msg += F("\",\"uri\":\"");
  msg += uri;
  msg += '"';
  if (payloadJson && *payloadJson) { msg += F(",\"payload\":"); msg += payloadJson; }
  msg += '}';

  if (cb) {
    for (auto& p : pending_) {
      if (!p.id) { p.id = id; p.deadline = millis() + timeoutMs; p.cb = cb; break; }
    }
  }
  LOGF("%s -> %s %s\n", TAG, uri, payloadJson ? payloadJson : "");
  ws_.sendTXT(msg);
  return id;
}

void LGTV::expirePending() {
  uint32_t now = millis();
  for (auto& p : pending_) {
    if (p.id && (int32_t)(now - p.deadline) >= 0) {
      p.id = 0;
      LOGF("%s request timed out\n", TAG);
      lastError = now;
      if (p.cb) p.cb(false, JsonObjectConst());
    }
  }
}

// -------------------------------------------------------- pointer socket

void LGTV::openPointerSocket() {
  request("ssap://com.webos.service.networkinput/getPointerInputSocket", nullptr,
          [this](bool ok, JsonObjectConst payload) {
            const char* path = payload["socketPath"] | "";
            if (!ok || !*path) { LOGF("%s no pointer socket offered\n", TAG); return; }
            // wss://host:port/resources/<id>/netinput.pointer.sock
            String url(path);
            bool tls = url.startsWith("wss://");
            int hostStart = url.indexOf("://") + 3;
            int slash = url.indexOf('/', hostStart);
            String hostPort = url.substring(hostStart, slash);
            String p = url.substring(slash);
            int colon = hostPort.indexOf(':');
            String host = colon >= 0 ? hostPort.substring(0, colon) : hostPort;
            uint16_t port = colon >= 0 ? hostPort.substring(colon + 1).toInt() : (tls ? 3001 : 3000);
            LOGF("%s opening pointer socket %s:%u%s (heap %u)\n", TAG, host.c_str(), port, p.c_str(), ESP.getFreeHeap());
            if (tls) ptr_.beginSSL(host.c_str(), port, p.c_str(), "", "");
            else     ptr_.begin(host.c_str(), port, p.c_str(), "");
            ptrBegun_ = true;
          });
}

void LGTV::onPointerEvent(WStype_t type, uint8_t* data, size_t len) {
  if (type == WStype_CONNECTED) {
    LOGF("%s pointer socket open (heap %u)\n", TAG, ESP.getFreeHeap());
    ptrOpen_ = true;
    if (pendingButton_[0]) { sendButtonRaw(pendingButton_); pendingButton_[0] = 0; }
  } else if (type == WStype_DISCONNECTED) {
    LOGF("%s pointer socket closed: %.*s\n", TAG, (int)len, data ? (const char*)data : "");
    ptrOpen_ = false;
    ptrBegun_ = false;  // stop looping it; next button re-requests the path
  } else if (type == WStype_ERROR) {
    LOGF("%s pointer socket error: %.*s\n", TAG, (int)len, data ? (const char*)data : "");
  }
}

void LGTV::sendButtonRaw(const char* name) {
  String frame = F("type:button\nname:");
  frame += name;
  frame += F("\n\n");
  LOGF("%s -> button %s\n", TAG, name);
  ptr_.sendTXT(frame);
}

// ------------------------------------------------------------- helpers

void LGTV::wake() {
  const char* macs[2] = {cur_.mac, cur_.wifiMac};
  IPAddress bcastSubnet = IPAddress((uint32_t)WiFi.localIP() | ~(uint32_t)WiFi.subnetMask());
  IPAddress bcastAll(255, 255, 255, 255);
  WiFiUDP udp;
  bool any = false;
  for (const char* mac : macs) {
    if (!mac || !*mac) continue;
    uint8_t m[6];
    if (sscanf(mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) continue;
    uint8_t pkt[102];
    memset(pkt, 0xFF, 6);
    for (int i = 0; i < 16; i++) memcpy(pkt + 6 + i * 6, m, 6);
    for (IPAddress dst : {bcastAll, bcastSubnet}) {
      for (uint16_t port : {9, 7}) {
        udp.beginPacket(dst, port);
        udp.write(pkt, sizeof(pkt));
        udp.endPacket();
      }
    }
    LOGF("%s wake-on-lan sent for %s\n", TAG, mac);
    any = true;
  }
  udp.stop();
  if (!any) { LOGF("%s no MAC stored for %s, cannot wake\n", TAG, cur_.name); lastError = millis(); }
}

void LGTV::lunaCall(const char* lunaUri, const char* paramsJson) {
  // createAlert/closeAlert workaround — the write path that works on most firmware.
  String p;
  p.reserve(200 + 3 * strlen(paramsJson));
  p += F("{\"message\":\" \",\"buttons\":[{\"label\":\"\",\"onClick\":\"");
  p += lunaUri; p += F("\",\"params\":"); p += paramsJson;
  p += F("}],\"onclose\":{\"uri\":\""); p += lunaUri; p += F("\",\"params\":"); p += paramsJson;
  p += F("},\"onfail\":{\"uri\":\""); p += lunaUri; p += F("\",\"params\":"); p += paramsJson;
  p += F("}}");
  request("ssap://system.notifications/createAlert", p.c_str(), [this](bool ok, JsonObjectConst payload) {
    const char* alertId = payload["alertId"] | "";
    if (!ok || !*alertId) { lastError = millis(); return; }
    String close = F("{\"alertId\":\"");
    close += alertId; close += F("\"}");
    request("ssap://system.notifications/closeAlert", close.c_str(), nullptr);
  });
}

// ------------------------------------------------------------ discovery

static String urlDecode(const String& in) {
  String out; out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '%' && i + 2 < in.length()) {
      char h[3] = {in[i + 1], in[i + 2], 0};
      out += (char)strtol(h, nullptr, 16);
      i += 2;
    } else if (c == '+') out += ' ';
    else out += c;
  }
  return out;
}

static String headerValue(const String& text, const char* nameLower) {
  int from = 0;
  String lower = text; lower.toLowerCase();
  while (true) {
    int at = lower.indexOf(nameLower, from);
    if (at < 0) return "";
    if (at == 0 || lower[at - 1] == '\n') {
      int colon = text.indexOf(':', at);
      int eol = text.indexOf('\n', at);
      if (colon < 0 || (eol >= 0 && colon > eol)) return "";
      String v = text.substring(colon + 1, eol < 0 ? text.length() : eol);
      v.trim();
      return v;
    }
    from = at + 1;
  }
}

static String xmlTag(const String& xml, const char* tag) {
  String open = String("<") + tag + ">";
  int a = xml.indexOf(open);
  if (a < 0) return "";
  int b = xml.indexOf(String("</") + tag + ">", a);
  if (b < 0) return "";
  String v = xml.substring(a + open.length(), b);
  v.trim();
  return v;
}

void LGTV::scan() {
  // Port of the plugin's discovery.ts: SSDP M-SEARCH for the webOS second-
  // screen service, two rounds, 4 s window; then the UPnP description XML
  // for the friendly name (the SSDP name is just "[LG] webOS TV MODEL").
  scanning = true;
  xSemaphoreTake(foundMtx_, portMAX_DELAY);
  foundCount_ = 0;
  xSemaphoreGive(foundMtx_);
  foundGen++;

  struct Hit { char ip[16]; String name; String location; };
  Hit hits[MAX_FOUND]; int nHits = 0;

  const char* msearch = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 3\r\n"
                        "ST: urn:lge-com:service:webos-second-screen:1\r\n\r\n";
  WiFiUDP udp;
  udp.begin(50000);
  uint32_t start = millis();
  int probesSent = 0;
  while (millis() - start < 4000) {
    if ((probesSent == 0) || (probesSent == 1 && millis() - start > 1200)) {
      for (IPAddress dst : {IPAddress(239, 255, 255, 250), IPAddress(255, 255, 255, 255)}) {
        udp.beginPacket(dst, 1900);
        udp.write((const uint8_t*)msearch, strlen(msearch));
        udp.endPacket();
      }
      probesSent++;
    }
    int sz = udp.parsePacket();
    if (sz > 0) {
      String text; text.reserve(sz + 1);
      while (udp.available()) text += (char)udp.read();
      String lower = text; lower.toLowerCase();
      if (lower.indexOf("lge") >= 0 || text.indexOf("webos-second-screen") >= 0) {
        String ip = udp.remoteIP().toString();
        bool dup = false;
        for (int i = 0; i < nHits; i++) if (ip == hits[i].ip) dup = true;
        if (!dup && nHits < MAX_FOUND) {
          Hit& h = hits[nHits++];
          strlcpy(h.ip, ip.c_str(), sizeof(h.ip));
          String n = headerValue(text, "dlnadevicename.lge.com");
          h.name = n.length() ? urlDecode(n) : ip;
          h.location = headerValue(text, "location");
          LOGF("%s ssdp: %s %s\n", TAG, h.ip, h.name.c_str());
        }
      }
    }
    if (wsBegun_) ws_.loop();  // keep the TV link alive while we wait
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  udp.stop();

  // Friendly names from the device description.
  for (int i = 0; i < nHits; i++) {
    String url = hits[i].location.length() ? hits[i].location : String("http://") + hits[i].ip + ":1787/";
    HTTPClient http;
    http.setTimeout(2000);
    http.setConnectTimeout(2000);
    if (http.begin(url) && http.GET() == 200) {
      String fn = xmlTag(http.getString(), "friendlyName");
      if (fn.length()) hits[i].name = fn;
    }
    http.end();
    if (wsBegun_) ws_.loop();
  }

  xSemaphoreTake(foundMtx_, portMAX_DELAY);
  foundCount_ = nHits;
  for (int i = 0; i < nHits; i++) {
    strlcpy(found_[i].ip, hits[i].ip, sizeof(found_[i].ip));
    strlcpy(found_[i].name, hits[i].name.c_str(), sizeof(found_[i].name));
  }
  xSemaphoreGive(foundMtx_);
  scanning = false;
  foundGen++;
  LOGF("%s scan done: %d TVs\n", TAG, nHits);
}

// ------------------------------------------------------------ commands

void LGTV::handleCmd(const Cmd& c) {
  switch (c.type) {
    case CmdType::SelectTV: {
      int idx = atoi(c.a);
      if (idx == curIdx_ && !pairing.load()) return;
      TVRecord r;
      if (!store_->get(idx, r)) return;
      store_->setSelected(idx);
      pairing = false;
      dropAll();
      failStreak_ = 0;
      useTls_ = true;
      muted = false; playing = false; screenOff = false;
      connectMain();
      break;
    }

    case CmdType::Scan:
      if (!scanning.load()) scan();
      break;

    case CmdType::PairStart: {
      int idx = store_->findByIp(c.a);
      if (idx < 0) {
        TVRecord r = {};
        strlcpy(r.name, c.b[0] ? c.b : "LG TV", sizeof(r.name));
        strlcpy(r.ip, c.a, sizeof(r.ip));
        idx = store_->add(r);
        if (idx < 0) { LOGF("%s TV store full\n", TAG); lastError = millis(); return; }
      }
      store_->setSelected(idx);
      pairing = true;
      pairFailed = false;
      dropAll();
      failStreak_ = 0;
      useTls_ = true;
      connectMain();
      break;
    }

    case CmdType::SubmitPin: {
      if (link.load() != LinkState::NeedsPin) { lastError = millis(); return; }
      pairFailed = false;
      String p = F("{\"pin\":\"");
      p += c.a; p += F("\"}");
      // The reply to setPin is the "registered" message itself, handled above.
      request("ssap://pairing/setPin", p.c_str(), nullptr);
      break;
    }

    case CmdType::CancelPair: {
      if (!pairing.exchange(false)) return;
      // A record that never got a key is useless: drop it.
      if (curIdx_ >= 0 && !cur_.clientKey[0]) store_->remove(curIdx_);
      dropAll();
      connectMain();
      break;
    }

    case CmdType::Forget: {
      int idx = atoi(c.a);
      bool wasCurrent = idx == curIdx_;
      if (!store_->remove(idx)) return;
      LOGF("%s forgot TV %d\n", TAG, idx);
      if (wasCurrent) { pairing = false; dropAll(); connectMain(); }
      else curIdx_ = store_->selected();
      break;
    }

    case CmdType::PowerToggle:
      if (link.load() != LinkState::Registered) { wake(); return; }
      request("ssap://com.webos.service.tvpower/power/getPowerState", nullptr,
              [this](bool ok, JsonObjectConst payload) {
                const char* state = payload["state"] | "";
                bool panelOn = ok && strcmp(state, "Standby") && strcmp(state, "Active Standby") && strcmp(state, "Suspend");
                LOGF("%s power state '%s' -> %s\n", TAG, state, panelOn ? "turnOff" : "wake");
                if (panelOn) request("ssap://system/turnOff", nullptr, nullptr);
                else wake();
              }, 1500);
      break;

    case CmdType::Request:
      request(c.a, c.b, nullptr);
      break;

    case CmdType::Button:
      if (link.load() != LinkState::Registered) { lastError = millis(); return; }
      if (ptrOpen_) { sendButtonRaw(c.a); return; }
      strlcpy(pendingButton_, c.a, sizeof(pendingButton_));
      if (!ptrBegun_) openPointerSocket();
      break;

    case CmdType::MuteToggle:
      request("ssap://audio/getVolume", nullptr, [this](bool ok, JsonObjectConst payload) {
        bool m = ok ? (bool)(payload["muteStatus"] | (payload["volumeStatus"]["muteStatus"] | false)) : muted.load();
        request("ssap://audio/setMute", m ? "{\"mute\":false}" : "{\"mute\":true}", nullptr);
        muted = !m;
      });
      break;

    case CmdType::PlayPause:
      request("ssap://com.webos.media/getForegroundAppInfo", nullptr, [this](bool ok, JsonObjectConst payload) {
        const char* playState = nullptr;
        if (ok) {
          for (JsonObjectConst info : payload["foregroundAppInfo"].as<JsonArrayConst>()) {
            const char* ps = info["playState"] | (const char*)nullptr;
            if (ps && *ps) { playState = ps; break; }
          }
        }
        bool shouldPlay = playState ? strcmp(playState, "playing") != 0 : !playing.load();
        request(shouldPlay ? "ssap://media.controls/play" : "ssap://media.controls/pause", nullptr, nullptr);
        playing = shouldPlay;
      }, 2000);
      break;

    case CmdType::ScreenToggle: {
      bool turningOff = !screenOff.load();
      request(turningOff ? "ssap://com.webos.service.tvpower/power/turnOffScreen"
                         : "ssap://com.webos.service.tvpower/power/turnOnScreen",
              nullptr, [this, turningOff](bool ok, JsonObjectConst payload) {
                bool rv = payload["returnValue"] | false;
                if (!ok || !rv) {
                  lunaCall("luna://com.webos.settingsservice/setSystemSettings",
                           turningOff ? "{\"category\":\"picture\",\"settings\":{\"energySaving\":\"screen_off\"}}"
                                      : "{\"category\":\"picture\",\"settings\":{\"energySaving\":\"off\"}}");
                }
              });
      screenOff = turningOff;
      break;
    }

    case CmdType::LaunchApp: {
      String p = F("{\"id\":\"");
      p += c.a; p += F("\"}");
      request("ssap://system.launcher/launch", p.c_str(), nullptr);
      break;
    }

    default:
      break;
  }
}
