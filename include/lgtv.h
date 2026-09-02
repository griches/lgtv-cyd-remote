// One SSAP connection to one webOS TV, ported from lgtv-streamdeck/src/lg.
//
// Runs entirely on its own FreeRTOS task so a TV that is off (and therefore
// makes TCP connects block for seconds) never stalls the touch UI. The UI
// posts commands via a queue and reads state through the atomics below.
#pragma once
#include <Arduino.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <functional>
#include <atomic>

enum class LinkState : uint8_t { NoWifi, Offline, Connecting, Registered };

struct TVTarget {
  const char* name;
  const char* ip;
  const char* clientKey;
  const char* mac;
  const char* wifiMac;
};

enum class CmdType : uint8_t {
  None,
  PowerToggle,   // getPowerState -> turnOff, else Wake-on-LAN (plugin semantics)
  Request,       // a = ssap uri, b = json payload or ""
  Button,        // a = pointer-socket button name (UP, ENTER, BACK...)
  MuteToggle,    // getVolume -> setMute(!muted)
  PlayPause,     // getForegroundAppInfo -> play/pause
  ScreenToggle,  // turnOffScreen / turnOnScreen with luna fallback
  LaunchApp,     // a = app id
  SelectTV,      // a = index into targets
};

struct Cmd {
  CmdType type = CmdType::None;
  char a[96] = {0};
  char b[160] = {0};
};

class LGTV {
 public:
  void begin(const TVTarget* targets, int count, int initial = 0);
  bool post(CmdType t, const char* a = nullptr, const char* b = nullptr);

  // Shared state for the UI (written on the net task).
  std::atomic<LinkState> link{LinkState::NoWifi};
  std::atomic<bool> muted{false};
  std::atomic<bool> playing{false};
  std::atomic<bool> screenOff{false};
  std::atomic<int> tvIndex{0};
  std::atomic<uint32_t> lastActivity{0};  // millis of last TV response
  std::atomic<uint32_t> lastError{0};     // millis of last failed action

  const TVTarget& current() const { return targets_[tvIndex.load()]; }

 private:
  using ResponseCb = std::function<void(bool ok, JsonObjectConst payload)>;
  struct Pending { uint32_t id = 0; uint32_t deadline = 0; ResponseCb cb; };

  static void taskEntry(void* self);
  void task();
  void handleCmd(const Cmd& c);
  void connectMain();
  void onMainEvent(WStype_t type, uint8_t* data, size_t len);
  void onPointerEvent(WStype_t type, uint8_t* data, size_t len);
  void sendRegister();
  uint32_t request(const char* uri, const char* payloadJson, ResponseCb cb, uint32_t timeoutMs = 4000);
  void expirePending();
  void wake();
  void openPointerSocket();
  void sendButtonRaw(const char* name);
  void lunaCall(const char* lunaUri, const char* paramsJson);

  const TVTarget* targets_ = nullptr;
  int targetCount_ = 0;
  QueueHandle_t queue_ = nullptr;
  WebSocketsClient ws_;
  WebSocketsClient ptr_;
  bool wsBegun_ = false;
  bool ptrBegun_ = false;
  bool ptrOpen_ = false;
  bool useTls_ = true;
  uint8_t failStreak_ = 0;
  uint32_t nextId_ = 1;
  static const int MAX_PENDING = 6;
  Pending pending_[MAX_PENDING];
  char pendingButton_[16] = {0};  // button waiting for the pointer socket
};
