#include "web_ui.h"

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

#include "config.h"

static WebServer g_server(80);
static Preferences g_prefs;
static bool g_running = false;
static bool g_prefsReady = false;
static uint8_t g_ledBrightness = MAX_BRIGHTNESS;
static uint8_t g_idleBatteryBars = 1;  // 1-4 = manual bars
static bool g_trackTestMode = false;
static uint8_t g_trackTestStatus = 1;  // 1=CLEAR,2=YELLOW,4=SC,5=RED,6=VSC,7=VSC_END,99=SESSION_FINISHED
static char g_url[40] = {0};

static const char* PREF_NAMESPACE = "f1light";
static const char* PREF_LED_BRIGHT = "ledBright";
static const char* PREF_IDLE_BARS  = "idleBars";

static const char kHtmlPage[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset='utf-8'>
  <meta name='viewport' content='width=device-width,initial-scale=1'>
  <title>F1 Light</title>
  <style>
    body{font-family:system-ui,Segoe UI,Arial,sans-serif;margin:24px;background:#111;color:#eee;}
    .card{max-width:460px;background:#1b1b1b;border:1px solid #333;border-radius:12px;padding:16px;}
    h1{margin:0 0 12px;font-size:22px;}
    label{display:block;margin:8px 0 10px;color:#ccc;}
    input[type=range]{width:100%;}
    .row{display:flex;justify-content:space-between;align-items:center;}
    button{margin-top:14px;padding:10px 14px;border:0;border-radius:8px;background:#d21f26;color:#fff;font-weight:600;}
    small{color:#aaa;display:block;margin-top:10px;}
    select{width:100%;padding:10px;border-radius:8px;background:#151515;color:#eee;border:1px solid #333}
  </style>
</head>
<body>
  <div class='card'>
    <h1>F1 Light Settings</h1>
    <label for='b'>LED brightness</label>
    <div class='row'><span>0</span><strong id='v'>0</strong><span>255</span></div>
    <input id='b' type='range' min='0' max='255' value='0'>

    <label for='ib'>Idle battery level</label>
    <div class='row'><span>1</span><strong id='ibv'>1</strong><span>4</span></div>
    <input id='ib' type='range' min='1' max='4' value='1'>

    <label for='tm'>Track status test mode</label>
    <div class='row'><span>Off</span><strong id='tmv'>Off</strong><span>On</span></div>
    <input id='tm' type='range' min='0' max='1' value='0'>

    <label for='ts'>Test track status</label>
    <select id='ts'>
      <option value='1'>CLEAR</option>
      <option value='2'>YELLOW</option>
      <option value='4'>SAFETY CAR</option>
      <option value='5'>RED FLAG</option>
      <option value='6'>VSC</option>
      <option value='7'>VSC END</option>
      <option value='99'>SESSION FINISHED</option>
    </select>

    <button id='save'>Apply</button>
    <small>Changes apply immediately and are persisted.</small>
  </div>

  <script>
    const b=document.getElementById('b');
    const v=document.getElementById('v');
    const ib=document.getElementById('ib');
    const ibv=document.getElementById('ibv');
    const tm=document.getElementById('tm');
    const tmv=document.getElementById('tmv');
    const ts=document.getElementById('ts');

    function syncLabels(){
      v.textContent=b.value;
      ibv.textContent=ib.value;
      tmv.textContent=(tm.value==='1'?'On':'Off');
    }

    async function loadState(){
      try {
        const res=await fetch('/api/brightness');
        const j=await res.json();
        if (typeof j.brightness==='number') b.value=String(j.brightness);
        if (typeof j.idleBatteryBars==='number') ib.value=String(j.idleBatteryBars);
        if (typeof j.trackTestMode==='number') tm.value=String(j.trackTestMode?1:0);
        if (typeof j.trackTestStatus==='number') ts.value=String(j.trackTestStatus);
        syncLabels();
      } catch (_) {}
    }

    b.addEventListener('input',syncLabels);
    ib.addEventListener('input',syncLabels);
    tm.addEventListener('input',syncLabels);

    document.getElementById('save').addEventListener('click',async()=>{
      await fetch('/api/brightness',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'value='+encodeURIComponent(b.value)});
      await fetch('/api/idle-battery',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'bars='+encodeURIComponent(ib.value)});
      await fetch('/api/test-track',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'mode='+encodeURIComponent(tm.value)+'&status='+encodeURIComponent(ts.value)});
      await loadState();
    });

    loadState();
  </script>
</body>
</html>
)HTML";

static void loadPrefsIfNeeded() {
  if (g_prefsReady) return;
  if (!g_prefs.begin(PREF_NAMESPACE, false)) {
    Serial.println("[WebUI] Preferences open failed; using defaults");
    return;
  }
  g_prefsReady = true;

  g_ledBrightness = g_prefs.getUChar(PREF_LED_BRIGHT, MAX_BRIGHTNESS);
  uint8_t bars = g_prefs.getUChar(PREF_IDLE_BARS, 1);
  if (bars < 1) bars = 1;
  if (bars > 4) bars = 4;
  g_idleBatteryBars = bars;
}

static void handleRoot() {
  g_server.send_P(200, "text/html", kHtmlPage);
}

static void handleGetBrightness() {
  String json = "{\"brightness\":" + String(g_ledBrightness)
              + ",\"idleBatteryBars\":" + String(g_idleBatteryBars)
              + ",\"trackTestMode\":" + String(g_trackTestMode ? 1 : 0)
              + ",\"trackTestStatus\":" + String(g_trackTestStatus)
              + "}";
  g_server.send(200, "application/json", json);
}

static void handleSetBrightness() {
  if (!g_server.hasArg("value")) {
    g_server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing value\"}");
    return;
  }
  int v = g_server.arg("value").toInt();
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  g_ledBrightness = (uint8_t)v;
  if (g_prefsReady) g_prefs.putUChar(PREF_LED_BRIGHT, g_ledBrightness);
  g_server.send(200, "application/json", "{\"ok\":true}");
}

static void handleGetIdleBattery() {
  String json = "{\"idleBatteryBars\":" + String(g_idleBatteryBars) + "}";
  g_server.send(200, "application/json", json);
}

static void handleSetIdleBattery() {
  if (!g_server.hasArg("bars")) {
    g_server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing bars\"}");
    return;
  }
  int bars = g_server.arg("bars").toInt();
  if (bars < 1) bars = 1;
  if (bars > 4) bars = 4;
  g_idleBatteryBars = (uint8_t)bars;
  if (g_prefsReady) g_prefs.putUChar(PREF_IDLE_BARS, g_idleBatteryBars);
  g_server.send(200, "application/json", "{\"ok\":true}");
}

static bool isAllowedTrackStatus(int code) {
  return code == 1 || code == 2 || code == 4 || code == 5 || code == 6 || code == 7 || code == 99;
}

static void handleGetTestTrack() {
  String json = "{\"mode\":" + String(g_trackTestMode ? 1 : 0)
              + ",\"status\":" + String(g_trackTestStatus) + "}";
  g_server.send(200, "application/json", json);
}

static void handleSetTestTrack() {
  if (g_server.hasArg("mode")) {
    int mode = g_server.arg("mode").toInt();
    g_trackTestMode = (mode != 0);
  }

  if (g_server.hasArg("status")) {
    int status = g_server.arg("status").toInt();
    if (isAllowedTrackStatus(status))
      g_trackTestStatus = (uint8_t)status;
  }

  g_server.send(200, "application/json", "{\"ok\":true}");
}

void webUiBegin() {
  if (g_running) return;
  if (WiFi.status() != WL_CONNECTED) return;

  loadPrefsIfNeeded();

  g_server.on("/", HTTP_GET, handleRoot);
  g_server.on("/api/brightness", HTTP_GET, handleGetBrightness);
  g_server.on("/api/brightness", HTTP_POST, handleSetBrightness);
  g_server.on("/api/idle-battery", HTTP_GET, handleGetIdleBattery);
  g_server.on("/api/idle-battery", HTTP_POST, handleSetIdleBattery);
  g_server.on("/api/test-track", HTTP_GET, handleGetTestTrack);
  g_server.on("/api/test-track", HTTP_POST, handleSetTestTrack);
  g_server.onNotFound([]() {
    g_server.send(404, "text/plain", "Not found");
  });

  g_server.begin();
  g_running = true;

  IPAddress ip = WiFi.localIP();
  snprintf(g_url, sizeof(g_url), "http://%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  Serial.printf("[WebUI] Running at %s\n", g_url);
}

void webUiStop() {
  if (!g_running) return;
  g_server.stop();
  g_running = false;
  g_url[0] = '\0';
}

void webUiLoop() {
  if (!g_running) return;
  if (WiFi.status() != WL_CONNECTED) {
    webUiStop();
    return;
  }
  g_server.handleClient();
}

bool webUiIsRunning() {
  return g_running;
}

uint8_t webUiGetLedBrightness() {
  return g_ledBrightness;
}

uint8_t webUiGetIdleBatteryBars() {
  return g_idleBatteryBars;
}

bool webUiTrackTestEnabled() {
  return g_trackTestMode;
}

uint8_t webUiTrackTestStatusCode() {
  return g_trackTestStatus;
}

const char* webUiGetUrl() {
  return g_url;
}
