/* src/main.ino
   ESP32 IoT Dashboard - Privacy Enhanced
   - WiFi creds moved to config.h (not included)
   - IP hidden from HTML; revealed via API endpoint on-demand
   - Endpoints: / , /time, /touch, /led/on, /led/off, /led/brightness, /status, /system/info
*/

#include <WiFi.h>
#include <WebServer.h>
#include "time.h"
#include "config.h"   // create locally from config.h.example (DO NOT upload)

const long GMT_OFFSET_SEC = 19800; // IST
const int DAYLIGHT_OFFSET_SEC = 0;

const int BUILTIN_LED_PIN = 2;
const int TOUCH_SENSOR_PIN = 4;
const int OUTPUT_PINS[] = {5, 18, 19};
const int OUTPUT_COUNT = sizeof(OUTPUT_PINS) / sizeof(OUTPUT_PINS[0]);

const int LEDC_CHANNEL = 0;
const int LEDC_FREQ = 5000;
const int LEDC_RES = 8;

const int WEB_SERVER_PORT = 80;
WebServer server(WEB_SERVER_PORT);

struct DeviceState {
  bool ledState = false;
  int ledBrightness = 128;
  int touchValue = 0;
  bool outputs[3] = {false,false,false};
  unsigned long lastTouchRead = 0;
  unsigned long lastTimeSync = 0;
  unsigned long systemStartTime = 0;
  String deviceIP = "";
};

DeviceState deviceState;

String dashboardHTML(); // forward

// ------------------ Helpers ------------------
String esc(String s){ // small JSON-escape-ish helper for limited use
  s.replace("\\","\\\\");
  s.replace("\"","\\\"");
  return s;
}

void sendJSON(const String &s){
  server.sendHeader("Cache-Control","no-store, no-cache, must-revalidate, max-age=0");
  server.send(200, "application/json", s);
}

// ------------------ Routes ------------------
void handleRoot(){
  server.send_P(200, "text/html", dashboardHTML().c_str());
}

void handleTime(){
  struct tm t;
  if(!getLocalTime(&t)){
    sendJSON("{\"time\":\"--:--:--\",\"date\":\"-- -- ----\"}");
    return;
  }
  char tb[20], db[20];
  strftime(tb, sizeof(tb), "%H:%M:%S", &t);
  strftime(db, sizeof(db), "%d-%m-%Y", &t);
  String j = "{\"time\":\""+String(tb)+"\",\"date\":\""+String(db)+"\"}";
  sendJSON(j);
}

void handleTouch(){
  String j = "{\"touch_value\":" + String(deviceState.touchValue) + "}";
  sendJSON(j);
}

void handleLedOn(){
  deviceState.ledState = true;
  ledcWrite(LEDC_CHANNEL, deviceState.ledBrightness);
  sendJSON("{\"success\":true,\"message\":\"LED turned ON\"}");
}

void handleLedOff(){
  deviceState.ledState = false;
  ledcWrite(LEDC_CHANNEL, 0);
  sendJSON("{\"success\":true,\"message\":\"LED turned OFF\"}");
}

void handleLedBrightness(){
  if(!server.hasArg("value")){
    sendJSON("{\"success\":false,\"message\":\"Missing value\"}");
    return;
  }
  int v = constrain(server.arg("value").toInt(), 0, 255);
  deviceState.ledBrightness = v;
  if(deviceState.ledState) ledcWrite(LEDC_CHANNEL, v);
  sendJSON("{\"success\":true,\"message\":\"Brightness updated\",\"brightness\":"+String(v)+"}");
}

void handleStatus(){
  String s = "{";
  s += "\"led_state\":" + String(deviceState.ledState ? "true":"false") + ",";
  s += "\"led_brightness\":" + String(deviceState.ledBrightness) + ",";
  s += "\"touch_value\":" + String(deviceState.touchValue) + ",";
  s += "\"outputs\":[";
  for(int i=0;i<OUTPUT_COUNT;i++){
    s += deviceState.outputs[i] ? "true":"false";
    if(i<OUTPUT_COUNT-1) s += ",";
  }
  s += "],";
  s += "\"uptime\":" + String((millis()-deviceState.systemStartTime)/1000);
  s += "}";
  sendJSON(s);
}

// Only returns sensitive details via API (not embedded into the static HTML)
void handleSystemInfo(){
  String s = "{";
  s += "\"ip\":\"" + esc(deviceState.deviceIP) + "\",";
  s += "\"ssid\":\"" + esc(WIFI_SSID) + "\",";
  s += "\"chip_id\":\"" + String((uint64_t)ESP.getEfuseMac(), HEX) + "\",";
  s += "\"free_heap\":" + String(ESP.getFreeHeap());
  s += "}";
  sendJSON(s);
}

void handleNotFound(){
  String msg = "Not found\nURI: "+server.uri();
  server.send(404, "text/plain", msg);
}

// ------------------ Initialization ------------------
void initWiFi(){
  Serial.print("[WiFi] Connecting to ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int count=0;
  while(WiFi.status()!=WL_CONNECTED && count<40){
    delay(300);
    Serial.print(".");
    count++;
  }
  if(WiFi.status()==WL_CONNECTED){
    deviceState.deviceIP = WiFi.localIP().toString();
    Serial.println();
    Serial.print("[WiFi] Connected. IP: ");
    Serial.println(deviceState.deviceIP);
  } else {
    Serial.println();
    Serial.println("[WiFi] Failed to connect. Rebooting...");
    delay(1000);
    ESP.restart();
  }
}

void initTime(){
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org");
  struct tm t;
  if(getLocalTime(&t)){
    Serial.print("[NTP] Time: ");
    Serial.printf("%02d:%02d:%02d\n", t.tm_hour, t.tm_min, t.tm_sec);
    deviceState.lastTimeSync = millis();
  } else {
    Serial.println("[NTP] Failed to sync time");
  }
}

void initGPIO(){
  pinMode(BUILTIN_LED_PIN, OUTPUT);
  for(int i=0;i<OUTPUT_COUNT;i++){
    pinMode(OUTPUT_PINS[i], OUTPUT);
    digitalWrite(OUTPUT_PINS[i], LOW);
  }
}

void initPWM(){
  ledcSetup(LEDC_CHANNEL, LEDC_FREQ, LEDC_RES);
  ledcAttachPin(BUILTIN_LED_PIN, LEDC_CHANNEL);
  ledcWrite(LEDC_CHANNEL, 0);
}

void initWebServer(){
  server.on("/", HTTP_GET, handleRoot);
  server.on("/time", HTTP_GET, handleTime);
  server.on("/touch", HTTP_GET, handleTouch);
  server.on("/led/on", HTTP_GET, handleLedOn);
  server.on("/led/off", HTTP_GET, handleLedOff);
  server.on("/led/brightness", HTTP_GET, handleLedBrightness);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/system/info", HTTP_GET, handleSystemInfo);
  server.onNotFound(handleNotFound);
  server.enableCORS(true);
  server.begin();
  Serial.println("[WEB] Server started");
}

// ------------------ HTML (privacy: IP not printed inline) ------------------
String dashboardHTML(){
  // Notice: IP not interpolated into this HTML. User clicks "Reveal IP" -> calls /system/info
  String html = R"rawliteral(
<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <title>ESP32 IoT Dashboard</title>
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <style>
    body{font-family:Inter,Arial;background:#f4f7fb;color:#111;padding:16px;}
    .card{background:#fff;border-radius:10px;padding:16px;margin-bottom:12px;box-shadow:0 6px 18px rgba(0,0,0,0.06)}
    .header{display:flex;justify-content:space-between;align-items:center}
    .btn{padding:10px 14px;border-radius:8px;border:0;cursor:pointer}
    .on{background:#16a34a;color:white}.off{background:#ef4444;color:white}
  </style>
</head>
<body>
  <div class="card header">
    <div>
      <h2>ESP32 IoT Dashboard</h2>
      <div style="color:#555">Privacy: IP hidden. Click to reveal.</div>
    </div>
    <div>
      <button onclick="revealIP()" class="btn">Reveal IP</button>
    </div>
  </div>

  <div class="card">
    <h3>Clock (IST)</h3>
    <div id="clock" style="font-family:monospace;font-size:24px">--:--:--</div>
    <div id="date" style="color:#666">-- -- ----</div>
  </div>

  <div class="card">
    <h3>LED</h3>
    <button class="btn on" onclick="fetch('/led/on')">ON</button>
    <button class="btn off" onclick="fetch('/led/off')">OFF</button>
    <div style="margin-top:10px">
      Brightness: <span id="bval">128</span><br>
      <input type="range" id="b" min="0" max="255" value="128" oninput="setBrightness(this.value)">
    </div>
  </div>

  <div class="card">
    <h3>Touch sensor</h3>
    <div id="touch" style="font-weight:700;font-size:22px">--</div>
  </div>

  <div class="card">
    <h3>System</h3>
    <div id="ip" style="font-family:monospace;color:#333">IP: —</div>
    <div id="chip" style="color:#666"></div>
    <div id="free" style="color:#666"></div>
  </div>

  <script>
    async function fetchJSON(path){ try{ const r = await fetch(path); return await r.json(); }catch(e){return null;} }

    async function updateTime(){
      const d = await fetchJSON('/time');
      if(d){ document.getElementById('clock').innerText = d.time; document.getElementById('date').innerText = d.date;}
    }

    async function updateTouch(){
      const d = await fetchJSON('/touch');
      if(d){ document.getElementById('touch').innerText = d.touch_value; }
    }

    function setBrightness(v){
      document.getElementById('bval').innerText = v;
      fetch('/led/brightness?value='+v);
    }

    async function revealIP(){
      const d = await fetchJSON('/system/info');
      if(d){
        document.getElementById('ip').innerText = 'IP: ' + d.ip;
        document.getElementById('chip').innerText = 'Chip: ' + d.chip_id;
        document.getElementById('free').innerText = 'Free heap: ' + d.free_heap;
      } else {
        alert('Failed to load system info');
      }
    }

    setInterval(updateTime, 1000);
    setInterval(updateTouch, 500);
    updateTime(); updateTouch();
  </script>
</body>
</html>
)rawliteral";
  return html;
}

// ------------------ Loop & setup ------------------
void setup(){
  Serial.begin(115200);
  delay(200);

  deviceState.systemStartTime = millis();

  initWiFi();
  initTime();
  initGPIO();
  initPWM();
  initWebServer();

  Serial.println("[SYSTEM] Ready. Use / on same LAN to open dashboard.");
}

void loop(){
  server.handleClient();

  // non-blocking touch update
  if(millis() - deviceState.lastTouchRead >= 100){
    deviceState.touchValue = touchRead(TOUCH_SENSOR_PIN);
    deviceState.lastTouchRead = millis();
  }
}

