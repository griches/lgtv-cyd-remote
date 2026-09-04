#include "webui.h"
#include "layout.h"
#include "assets.h"
#include "log.h"
#include "version.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <ArduinoJson.h>

namespace webui {

static WebServer server(80);
static TFT_eSPI* tft_ = nullptr;
static LGTV* lg_ = nullptr;
static TVStore* store_ = nullptr;
static bool started_ = false;

// ------------------------------------------------------------------ page

static const char PAGE_HTML[] PROGMEM = R"html(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>LG Remote</title>
<style>
:root{--bg:#0f1115;--card:#181b22;--line:#2a2f3a;--fg:#e8e6e1;--dim:#9aa0ad;--acc:#3d9fff;--ok:#3ddc84;--bad:#ff5d5d}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 -apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif}
header{display:flex;align-items:center;gap:16px;padding:14px 20px;border-bottom:1px solid var(--line)}
header h1{font-size:18px;margin:0}header .st{color:var(--dim);font-size:13px}
main{display:grid;grid-template-columns:1fr 360px;gap:20px;padding:20px;max-width:1200px}
@media(max-width:900px){main{grid-template-columns:1fr}}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px}
.tabs{display:flex;gap:6px;flex-wrap:wrap;margin-bottom:10px}
.tabs button{background:#222632;border:1px solid var(--line);color:var(--fg);padding:6px 12px;border-radius:8px;cursor:pointer}
.tabs button.on{background:var(--acc);border-color:var(--acc);color:#fff}
.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:8px}
.tile{background:#11141a;border:1px solid var(--line);border-radius:8px;padding:8px;display:flex;flex-direction:column;gap:5px}
.tile img{width:48px;height:48px;border-radius:8px;background:#000;align-self:center}
.tile select,.tile input{width:100%;background:#0b0d11;color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:4px 6px;font-size:12px}
.tile label{font-size:11px;color:var(--dim)}.tile .tname{font-size:12px;text-align:center;min-height:16px;color:var(--fg);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}

.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:10px}
button.pri{background:var(--acc);color:#fff;border:0;padding:8px 14px;border-radius:8px;cursor:pointer}
button.sec{background:#222632;color:var(--fg);border:1px solid var(--line);padding:8px 14px;border-radius:8px;cursor:pointer}
button.danger{background:transparent;color:var(--bad);border:1px solid var(--bad)}
input.pname{background:#0b0d11;color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:6px 8px}
#shot{width:320px;height:240px;border-radius:8px;border:1px solid var(--line);background:#000;display:block}
.kv{display:grid;grid-template-columns:auto 1fr;gap:4px 12px;font-size:13px}.kv b{color:var(--dim);font-weight:500}
.msg{font-size:13px;color:var(--dim);min-height:18px}.msg.ok{color:var(--ok)}.msg.bad{color:var(--bad)}
h2{font-size:15px;margin:0 0 10px}small{color:var(--dim)}
</style></head><body>
<header><h1>LG Remote</h1><span class="st" id="status">…</span></header>
<main>
<section class="card">
 <h2>Layout <small>— build your own remote</small></h2>
 <div class="tabs" id="tabs"></div>
 <div class="row" style="margin:0 0 10px">
  <input class="pname" id="pname" placeholder="Page name" maxlength="15">
  <button class="sec" onclick="addPage()">+ Page</button>
  <button class="sec danger" onclick="delPage()">Delete page</button>
 </div>
 <div class="grid" id="grid"></div>
 <div class="row">
  <button class="pri" onclick="save()">Save to device</button>
  <button class="sec" onclick="load()">Reload</button>
  <button class="sec danger" onclick="resetLayout()">Reset to default</button>
  <span class="msg" id="msg"></span>
 </div>
 <p><small>Pick a kind, then what it should do. App and input lists come from the TV itself, so it needs to be on. Tiles with artwork show it; the rest show their name on a blank key. Icon and label are optional overrides.</small></p>
</section>
<aside style="display:flex;flex-direction:column;gap:20px">
 <section class="card"><h2>Live screen</h2><img id="shot" src="/screen.bmp" alt="screen">
  <div class="row"><button class="sec" onclick="shot()">Refresh</button><label><input type="checkbox" id="auto"> auto</label></div></section>
 <section class="card"><h2>Device</h2><div class="kv" id="kv"></div>
  <div class="row"><button class="sec" onclick="fetch('/api/reboot',{method:'POST'})">Reboot</button></div></section>
 <section class="card"><h2>Firmware update</h2>
  <form method="POST" action="/update" enctype="multipart/form-data" onsubmit="document.getElementById('umsg').textContent='Uploading… the device reboots when done.'">
   <input type="file" name="firmware" accept=".bin" required> <button class="pri" type="submit">Flash</button></form>
  <div class="msg" id="umsg"></div><small>Upload <code>.pio/build/cyd/firmware.bin</code>. OTA from PlatformIO also works: <code>pio run -t upload --upload-port lgremote.local</code></small></section>
</aside>
</main>
<script>
const KINDS=[["none","Empty"],["power","Power"],["button","Remote key"],["app","App"],["input","Input"],["volume_up","Volume up"],["volume_down","Volume down"],["mute","Mute"],["playpause","Play / Pause"],["screen","Screen off"],["ssap","Raw SSAP request"]];
const KEYS=["UP","DOWN","LEFT","RIGHT","ENTER","BACK","HOME","MENU","EXIT","INFO","GUIDE","CC","DASH","PLAY","PAUSE","STOP","REWIND","FASTFORWARD","CHANNELUP","CHANNELDOWN","VOLUMEUP","VOLUMEDOWN","MUTE","RED","GREEN","YELLOW","BLUE","0","1","2","3","4","5","6","7","8","9"];
const KEYNAME={ENTER:"OK",CC:"Subtitles",DASH:"Dash (-)"};
const ACTION_ICONS=["power","vol_up","vol_down","unmuted","muted","up","down","left","right","ok","back","home","exit","settings","play","pause","screen_off","screen_on","input","key_blank"];
const AUTO={power:'power',mute:'unmuted',playpause:'play',screen:'screen_off',volume_up:'vol_up',volume_down:'vol_down'};
const BTN={UP:'up',DOWN:'down',LEFT:'left',RIGHT:'right',ENTER:'ok',BACK:'back',HOME:'home',EXIT:'exit',MENU:'settings',PLAY:'play',PAUSE:'pause',VOLUMEUP:'vol_up',VOLUMEDOWN:'vol_down',MUTE:'unmuted'};
let L={pages:[]},cur=0,ICONS=[],APPIDS=[],APPS=null,INPUTS=[];
const $=id=>document.getElementById(id);
function msg(t,c){const m=$('msg');m.textContent=t;m.className='msg '+(c||'');}
function esc(s){return String(s).replace(/&/g,'&amp;').replace(/"/g,'&quot;').replace(/</g,'&lt;')}
function opt(v,label,sel){return `<option value="${esc(v)}" ${sel?'selected':''}>${esc(label)}</option>`}
async function load(){ICONS=await (await fetch('/api/icons')).json();APPIDS=ICONS.filter(n=>!ACTION_ICONS.includes(n));
 try{INPUTS=(await (await fetch('/api/inputs')).json()).inputs||[]}catch(e){}
 L=await (await fetch('/api/layout')).json();cur=Math.min(cur,L.pages.length-1);render();msg('');loadApps();}
async function loadApps(){try{const r=await (await fetch('/api/apps')).json();if(r.loaded){APPS=r.apps.sort((a,b)=>a.title.localeCompare(b.title));render();}
 else if(r.loading||r.connected){setTimeout(loadApps,1500)}}catch(e){}}
function iconUrl(t){let n=t.icon;if(!n){if(AUTO[t.kind])n=AUTO[t.kind];else if(t.kind==='button')n=BTN[(t.arg||'').toUpperCase()];else if(t.kind==='app'&&APPIDS.includes(t.arg))n=t.arg;}
 if(!n)n=t.kind==='none'?'':'key_blank';return n?'/icon/'+encodeURIComponent(n)+'.bmp':'data:image/gif;base64,R0lGODlhAQABAAAAACw=';}
function appName(id){const a=APPS&&APPS.find(a=>a.id===id);return a?a.title:id}
function argControl(t){
 if(t.kind==='button'){const known=KEYS.includes((t.arg||'').toUpperCase());
  return `<label>key</label><select data-k="arg">${opt('','— choose —',!t.arg)}${KEYS.map(k=>opt(k,KEYNAME[k]||k.charAt(0)+k.slice(1).toLowerCase(),t.arg===k)).join('')}${opt('__other','Other…',t.arg&&!known)}</select>${t.arg&&!known?`<input data-k="arg" value="${esc(t.arg)}" placeholder="key name">`:''}`}
 if(t.kind==='app'){const ids=new Set((APPS||[]).map(a=>a.id));const list=APPS?APPS.map(a=>opt(a.id,a.title+(APPIDS.includes(a.id)?' ★':''),t.arg===a.id)).join(''):APPIDS.map(id=>opt(id,id+' ★',t.arg===id)).join('');
  const known=ids.has(t.arg)||(!APPS&&APPIDS.includes(t.arg));
  return `<label>app ${APPS?`<small>(${APPS.length} on the TV, ★ has artwork)</small>`:'<small>(loading the TV\'s app list…)</small>'}</label><select data-k="arg">${opt('','— choose —',!t.arg)}${list}${opt('__other','Other app id…',t.arg&&!known)}</select>${t.arg&&!known?`<input data-k="arg" value="${esc(t.arg)}" placeholder="app id">`:''}`}
 if(t.kind==='input'){const ids=INPUTS.map(i=>i.id);const list=(INPUTS.length?INPUTS:[1,2,3,4].map(n=>({id:'HDMI_'+n,label:'HDMI '+n}))).map(i=>opt(i.id,i.label+' ('+i.id+')',t.arg===i.id)).join('');
  const known=ids.includes(t.arg)||/^HDMI_[1-4]$/.test(t.arg||'');
  return `<label>input <small>(names come from the TV)</small></label><select data-k="arg">${opt('','— choose —',!t.arg)}${list}${opt('__other','Other input id…',t.arg&&!known)}</select>${t.arg&&!known?`<input data-k="arg" value="${esc(t.arg)}" placeholder="input id">`:''}`}
 if(t.kind==='ssap'){return `<label>uri</label><input data-k="arg" value="${esc(t.arg||'')}" placeholder="ssap://audio/volumeUp"><label>payload (JSON, optional)</label><input data-k="payload" value="${esc(t.payload||'')}" placeholder='{"volume":10}'>`}
 return '';}
function render(){const tabs=$('tabs');tabs.innerHTML='';L.pages.forEach((p,i)=>{const b=document.createElement('button');b.textContent=p.name||('Page '+(i+1));b.className=i===cur?'on':'';b.onclick=()=>{cur=i;render()};tabs.appendChild(b)});
 const p=L.pages[cur];$('pname').value=p.name||'';$('pname').oninput=e=>{p.name=e.target.value};
 const g=$('grid');g.innerHTML='';while(p.tiles.length<12)p.tiles.push({kind:'none'});
 p.tiles.forEach((t,i)=>{const d=document.createElement('div');d.className='tile';const hasArt=iconUrl(t).startsWith('/icon/')&&!iconUrl(t).includes('key_blank');
  d.innerHTML=`<img src="${iconUrl(t)}"><div class="tname">${t.kind==='app'?esc(appName(t.arg||'')):t.kind==='input'?esc((INPUTS.find(x=>x.id===t.arg)||{}).label||t.arg||''):esc(t.arg||'')}</div>
  <label>kind</label><select data-k="kind">${KINDS.map(([k,n])=>opt(k,n,t.kind===k)).join('')}</select>
  <div class="argc">${argControl(t)}</div>
  ${t.kind!=='none'?`<label>icon</label><select data-k="icon">${opt('','auto',!t.icon)}${ICONS.map(n=>opt(n,n,t.icon===n)).join('')}</select>
   <label>label <small>${hasArt?'(shown only without artwork)':'(drawn on the key)'}</small></label><input data-k="label" value="${esc(t.label||'')}" maxlength="23">
   <label><input type="checkbox" data-k="repeat" ${t.repeat?'checked':''}> repeat while held</label>`:''}`;
  d.querySelectorAll('[data-k]').forEach(el=>{el.onchange=()=>{const k=el.dataset.k;let v=el.type==='checkbox'?el.checked:el.value;
   if(v==='__other'){t[k]='';render();const inp=g.children[i].querySelector('input[data-k=arg]');if(inp)inp.focus();return;}
   t[k]=v;if(!t[k]&&k!=='repeat')delete t[k];
   if(k==='kind'){delete t.arg;delete t.payload;if(!t.icon)delete t.icon;t.repeat=['volume_up','volume_down'].includes(v);}
   if(k==='arg'&&t.kind==='button'&&['UP','DOWN','LEFT','RIGHT'].includes(v))t.repeat=true;
   if(k==='kind'||k==='arg'||k==='icon')render();else d.querySelector('img').src=iconUrl(t)}});
  g.appendChild(d)});}
function addPage(){if(L.pages.length>=6)return msg('Max 6 pages','bad');L.pages.push({name:'Page '+(L.pages.length+1),tiles:[]});cur=L.pages.length-1;render()}
function delPage(){if(L.pages.length<=1)return msg('Keep at least one page','bad');if(!confirm('Delete this page?'))return;L.pages.splice(cur,1);cur=Math.max(0,cur-1);render()}
async function save(){const bad=L.pages.flatMap((p,pi)=>p.tiles.map((t,i)=>({t,pi,i}))).filter(x=>['button','app','input','ssap'].includes(x.t.kind)&&!x.t.arg);
 if(bad.length){cur=bad[0].pi;render();return msg(`Tile ${bad[0].i+1} on "${L.pages[bad[0].pi].name}" needs a ${bad[0].t.kind==='button'?'key':bad[0].t.kind==='ssap'?'URI':bad[0].t.kind}.`,'bad')}
 const r=await fetch('/api/layout',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(L)});msg(r.ok?'Saved. The remote redrew itself.':'Rejected: '+await r.text(),r.ok?'ok':'bad');setTimeout(shot,600)}
async function resetLayout(){if(!confirm('Restore the factory layout?'))return;await fetch('/api/layout/reset',{method:'POST'});await load();setTimeout(shot,600)}
function shot(){$('shot').src='/screen.bmp?'+Date.now()}
async function status(){try{const s=await (await fetch('/api/status')).json();$('status').textContent=`${s.tv||'no TV'} · ${s.link} · ${s.ip}`;
 $('kv').innerHTML=Object.entries({Firmware:s.fw+' '+s.version,IP:s.ip,mDNS:'http://lgremote.local',TV:s.tv||'—',Link:s.link,'Free heap':(s.heap/1024).toFixed(0)+' KB',Uptime:Math.floor(s.uptime/60)+' min'}).map(([k,v])=>`<b>${k}</b><span>${v}</span>`).join('')}catch(e){}}
load();status();setInterval(status,5000);setInterval(()=>{if($('auto').checked)shot()},3000);
</script></body></html>)html";

// ------------------------------------------------------------- helpers

static void bmpHeader(uint8_t* h, int w, int hgt) {
  uint32_t rowBytes = ((w * 3 + 3) / 4) * 4;
  uint32_t size = 54 + rowBytes * hgt;
  memset(h, 0, 54);
  h[0] = 'B'; h[1] = 'M';
  h[2] = size; h[3] = size >> 8; h[4] = size >> 16; h[5] = size >> 24;
  h[10] = 54; h[14] = 40;
  h[18] = w; h[19] = w >> 8; h[20] = w >> 16; h[21] = w >> 24;
  h[22] = hgt; h[23] = hgt >> 8; h[24] = hgt >> 16; h[25] = hgt >> 24;   // positive = bottom-up
  h[26] = 1; h[28] = 24;
  uint32_t img = rowBytes * hgt;
  h[34] = img; h[35] = img >> 8; h[36] = img >> 16; h[37] = img >> 24;
}

static void sendScreen() {
  int w = tft_->width(), h = tft_->height();
  uint32_t rowBytes = ((w * 3 + 3) / 4) * 4;
  uint8_t hdr[54];
  bmpHeader(hdr, w, h);
  server.setContentLength(54 + rowBytes * h);
  server.send(200, "image/bmp", "");
  WiFiClient c = server.client();
  c.write(hdr, 54);
  static uint8_t row[320 * 3 + 4];
  for (int y = h - 1; y >= 0; y--) {
    tft_->readRectRGB(0, y, w, 1, row);
    // This panel returns the row shifted by one byte (extra dummy); realign
    // and swap to BGR for BMP.
    for (int x = 0; x < w; x++) {
      uint8_t r = row[x * 3 + 1], g = row[x * 3 + 2], b = (x * 3 + 3 < w * 3) ? row[x * 3 + 3] : 0;
      row[x * 3] = b; row[x * 3 + 1] = g; row[x * 3 + 2] = r;
    }
    c.write(row, rowBytes);
  }
}

static void sendIcon(const uint16_t* icon) {
  int w = ICON_W, h = ICON_H;
  uint32_t rowBytes = ((w * 3 + 3) / 4) * 4;
  uint8_t hdr[54];
  bmpHeader(hdr, w, h);
  server.setContentLength(54 + rowBytes * h);
  server.sendHeader("Cache-Control", "max-age=86400");
  server.send(200, "image/bmp", "");
  WiFiClient c = server.client();
  c.write(hdr, 54);
  uint8_t row[64 * 3 + 4];
  for (int y = h - 1; y >= 0; y--) {
    for (int x = 0; x < w; x++) {
      uint16_t p = pgm_read_word(icon + y * w + x);
      row[x * 3] = (p & 0x1F) << 3; row[x * 3 + 1] = ((p >> 5) & 0x3F) << 2; row[x * 3 + 2] = (p >> 11) << 3;
    }
    c.write(row, rowBytes);
  }
}

static const char* linkName(LinkState s) {
  switch (s) {
    case LinkState::Registered: return "connected";
    case LinkState::Connecting: return "connecting";
    case LinkState::NeedsPin:   return "pairing";
    case LinkState::NoTV:       return "no TV";
    default:                    return "no wifi";
  }
}

// -------------------------------------------------------------- routes

static void setupRoutes() {
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", PAGE_HTML); });

  server.on("/api/layout", HTTP_GET, []() { server.send(200, "application/json", layout::toJson()); });
  server.on("/api/layout", HTTP_POST, []() {
    String err;
    if (!layout::loadJson(server.arg("plain").c_str(), &err)) { server.send(400, "text/plain", err); return; }
    layout::save();
    server.send(200, "text/plain", "ok");
  });
  server.on("/api/layout/reset", HTTP_POST, []() { layout::resetDefault(); server.send(200, "text/plain", "ok"); });
  server.on("/api/layout/default", HTTP_GET, []() { server.send(200, "application/json", layout::defaultJson()); });

  server.on("/api/icons", HTTP_GET, []() {
    String out = "[";
    for (int i = 0; i < ICON_COUNT; i++) { if (i) out += ','; out += '"'; out += ICONS[i].name; out += '"'; }
    out += ']';
    server.send(200, "application/json", out);
  });

  // Installed apps from the TV (fetched on first request; poll until loaded=true)
  server.on("/api/apps", HTTP_GET, []() {
    if (lg_->appCount() == 0 && !lg_->appsLoading.load()) lg_->post(CmdType::FetchApps);
    JsonDocument doc;
    doc["loaded"] = lg_->appCount() > 0;
    doc["loading"] = lg_->appsLoading.load();
    doc["connected"] = lg_->link.load() == LinkState::Registered;
    JsonArray arr = doc["apps"].to<JsonArray>();
    AppInfo a;
    for (int i = 0; lg_->getAppAt(i, a); i++) { JsonObject o = arr.add<JsonObject>(); o["id"] = a.id; o["title"] = a.title; }
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
  });
  server.on("/api/inputs", HTTP_GET, []() {
    JsonDocument doc;
    JsonArray arr = doc["inputs"].to<JsonArray>();
    InputInfo in;
    for (int i = 0; lg_->getInputAt(i, in); i++) { JsonObject o = arr.add<JsonObject>(); o["id"] = in.id; o["label"] = in.label; o["connected"] = in.connected; }
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/api/status", HTTP_GET, []() {
    JsonDocument doc;
    TVRecord rec;
    doc["fw"] = FW_NAME;
    doc["version"] = FW_VERSION;
    doc["ip"] = WiFi.localIP().toString();
    doc["tv"] = store_->get(store_->selected(), rec) ? rec.name : "";
    doc["link"] = linkName(lg_->link.load());
    doc["heap"] = ESP.getFreeHeap();
    doc["uptime"] = millis() / 1000;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/api/reboot", HTTP_POST, []() { server.send(200, "text/plain", "rebooting"); delay(200); ESP.restart(); });

  server.on("/screen.bmp", HTTP_GET, sendScreen);

  server.on("/update", HTTP_POST,
    []() {
      bool ok = !Update.hasError();
      server.sendHeader("Connection", "close");
      server.send(200, "text/html", ok ? "<meta http-equiv=refresh content='8;url=/'>Update OK, rebooting..." : "Update FAILED");
      if (ok) { delay(300); ESP.restart(); }
    },
    []() {
      HTTPUpload& up = server.upload();
      if (up.status == UPLOAD_FILE_START) {
        LOGF("[web] update: %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) LOGF("[web] update begin failed\n");
      } else if (up.status == UPLOAD_FILE_WRITE) {
        Update.write(up.buf, up.currentSize);
      } else if (up.status == UPLOAD_FILE_END) {
        LOGF("[web] update %s (%u bytes)\n", Update.end(true) ? "ok" : "failed", up.totalSize);
      }
    });

  server.onNotFound([]() {
    String uri = server.uri();
    if (uri.startsWith("/icon/") && uri.endsWith(".bmp")) {
      String name = uri.substring(6, uri.length() - 4);
      const uint16_t* icon = layout::iconByName(name.c_str());
      if (icon) { sendIcon(icon); return; }
    }
    server.send(404, "text/plain", "not found");
  });
}

void begin(TFT_eSPI* tft, LGTV* lg, TVStore* store) {
  tft_ = tft; lg_ = lg; store_ = store;
  setupRoutes();
}

bool started() { return started_; }

void loop() {
  if (!started_) {
    if (WiFi.status() != WL_CONNECTED) return;
    server.begin();
    if (MDNS.begin("lgremote")) MDNS.addService("http", "tcp", 80);
    ArduinoOTA.setHostname("lgremote");
    ArduinoOTA.onStart([]() { LOGF("[ota] start\n"); });
    ArduinoOTA.onEnd([]() { LOGF("[ota] done\n"); });
    ArduinoOTA.begin();
    started_ = true;
    LOGF("[web] http://%s/  (http://lgremote.local)\n", WiFi.localIP().toString().c_str());
    return;
  }
  server.handleClient();
  ArduinoOTA.handle();
}

}  // namespace webui
