#include "lgtv.h"
#include "registration.h"
#include "log.h"
#include <WiFi.h>
#include <WiFiUdp.h>

static const char* TAG = "[lgtv]";

// ---------------------------------------------------------------- public

void LGTV::begin(const TVTarget* targets, int count, int initial) {
  targets_ = targets;
  targetCount_ = count;
  tvIndex = initial;
  queue_ = xQueueCreate(8, sizeof(Cmd));
  xTaskCreatePinnedToCore(taskEntry, "lgtv", 16384, this, 1, nullptr, 0);
}

bool LGTV::post(CmdType t, const char* a, const char* b) {
  Cmd c;
  c.type = t;
  if (a) strlcpy(c.a, a, sizeof(c.a));
  if (b) strlcpy(c.b, b, sizeof(c.b));
  return xQueueSend(queue_, &c, 0) == pdTRUE;
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
        if (wsBegun_) { ws_.disconnect(); wsBegun_ = false; }
        if (ptrBegun_) { ptr_.disconnect(); ptrBegun_ = false; ptrOpen_ = false; }
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if (!wsBegun_) connectMain();

    ws_.loop();
    if (ptrBegun_) ptr_.loop();
    expirePending();

    Cmd c;
    while (xQueueReceive(queue_, &c, 0) == pdTRUE) handleCmd(c);

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ----------------------------------------------------------- connection

void LGTV::connectMain() {
  const TVTarget& tv = current();
  link = LinkState::Connecting;
  LOGF("%s connecting to %s (%s) via %s\n", TAG, tv.name, tv.ip, useTls_ ? "wss:3001" : "ws:3000");
  if (useTls_) ws_.beginSSL(tv.ip, 3001, "/", "", "");
  else         ws_.begin(tv.ip, 3000, "/", "");
  wsBegun_ = true;
}

void LGTV::sendRegister() {
  const TVTarget& tv = current();
  String msg;
  msg.reserve(strlen_P(LG_MANIFEST) + 256);
  msg += F("{\"type\":\"register\",\"id\":\"register_0\",\"payload\":{\"client-key\":\"");
  msg += tv.clientKey;
  msg += F("\",\"pairingType\":\"prompt\",\"forcePairing\":false,\"manifest\":");
  msg += FPSTR(LG_MANIFEST);
  msg += F("}}");
  ws_.sendTXT(msg);
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
      link = LinkState::Connecting;
      ptrOpen_ = false;
      if (ptrBegun_) { ptr_.disconnect(); ptrBegun_ = false; }
      for (auto& p : pending_) p.id = 0;
      break;

    case WStype_TEXT: {
      JsonDocument doc;
      if (deserializeJson(doc, data, len)) return;
      const char* mtype = doc["type"] | "";
      JsonObjectConst payload = doc["payload"].as<JsonObjectConst>();
      lastActivity = millis();

      if (!strcmp(mtype, "registered") || (!strcmp(mtype, "response") && payload["client-key"].is<const char*>())) {
        LOGF("%s registered with %s\n", TAG, current().name);
        link = LinkState::Registered;
        failStreak_ = 0;
        // Live mute/volume state: subscribe, so the tile follows the TV remote too.
        String sub = F("{\"type\":\"subscribe\",\"id\":\"sub_volume\",\"uri\":\"ssap://audio/getVolume\"}");
        ws_.sendTXT(sub);
        return;
      }
      if (!strcmp(mtype, "error") && link.load() != LinkState::Registered) {
        LOGF("%s registration error: %s\n", TAG, doc["error"] | "?");
        return;
      }

      const char* id = doc["id"] | "";
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
  if (link.load() != LinkState::Registered) {
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
  const TVTarget& tv = current();
  const char* macs[2] = {tv.mac, tv.wifiMac};
  IPAddress bcastSubnet = IPAddress((uint32_t)WiFi.localIP() | ~(uint32_t)WiFi.subnetMask());
  IPAddress bcastAll(255, 255, 255, 255);
  WiFiUDP udp;
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
  }
  udp.stop();
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

// ------------------------------------------------------------ commands

void LGTV::handleCmd(const Cmd& c) {
  switch (c.type) {
    case CmdType::SelectTV: {
      int idx = atoi(c.a);
      if (idx < 0 || idx >= targetCount_ || idx == tvIndex.load()) return;
      tvIndex = idx;
      if (ptrBegun_) { ptr_.disconnect(); ptrBegun_ = false; ptrOpen_ = false; }
      if (wsBegun_) { ws_.disconnect(); wsBegun_ = false; }
      for (auto& p : pending_) p.id = 0;
      failStreak_ = 0;
      useTls_ = true;
      muted = false; playing = false; screenOff = false;
      connectMain();
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
