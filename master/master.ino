#include <WiFi.h>
#include <esp_now.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>

// ============================================================================
// SETTINGS
// ============================================================================
const char* ssid = "Lalala"; //wifiname
const char* password = "lalaland"; //wifipass
#define PIN_EMERGENCY_BTN 25
#define NUM_SLAVES 3

uint8_t slaveMACs[NUM_SLAVES][6] = {
  {0xEC, 0xE3, 0x34, 0xBF, 0xCE, 0x10},
  {0x20, 0xE7, 0xC8, 0x6B, 0x4D, 0x24},
  {0x28, 0x05, 0xA5, 0x6F, 0xEE, 0x6C}
};

// Global Timing
float G_MIN = 10.0, G_MAX = 40.0, YELLOW_TIME = 3.0;
#define ALL_RED_TIME 1000

// ============================================================================
// DATA STRUCTURES
// ============================================================================
enum LightState { STATE_RED = 0, STATE_YELLOW = 1, STATE_GREEN = 2 };

typedef struct __attribute__((packed)) {
  uint8_t id;
  uint8_t nearSensorOccupied;
  uint8_t farSensorOccupied;
  uint8_t currentState;
  uint8_t operationMode;
  uint8_t emergency;
} TrafficData;

typedef struct {
  uint8_t targetState;
  uint16_t duration;
} Command;

struct Intersection {
  TrafficData data;
  unsigned long lastUpdate = 0;
  unsigned long lastGreenEnd = 0;
  LightState currentLight = STATE_RED;
  unsigned long stateStart = 0;
  int plannedDuration = 0;
  int consecutiveGreens = 0;
} intersections[NUM_SLAVES];

AsyncWebServer server(80);
int activeIdx = -1;
bool isAllRed = false;
unsigned long allRedStart = 0;
bool manualMode = false;
unsigned long cycleCount = 0;
unsigned long startTime = 0;

// ============================================================================
// CORE FUNCTIONS
// ============================================================================

void sendCmd(int id, LightState s, int dur) {
  Command c = {(uint8_t)s, (uint16_t)dur};
  esp_now_send(slaveMACs[id], (uint8_t*)&c, sizeof(c));
  Serial.printf("Sent to INT%d: %s (%ds)\n", id, 
    s==STATE_GREEN?"GREEN":s==STATE_YELLOW?"YELLOW":"RED", dur);
}

void onReceive(const esp_now_recv_info *info, const uint8_t *incomingData, int len) {
  if (len != sizeof(TrafficData)) return;
  TrafficData t;
  memcpy(&t, incomingData, sizeof(t));
  if (t.id < NUM_SLAVES) {
    intersections[t.id].data = t;
    intersections[t.id].lastUpdate = millis();
  }
}

float getPriority(int i) {
  if (millis() - intersections[i].lastUpdate > 10000) return 0.05;
  float p = 0.0;
  if (intersections[i].data.nearSensorOccupied) p += 1.0;
  if (intersections[i].data.farSensorOccupied) p += 2.0;
  p += (millis() - intersections[i].lastGreenEnd) / 120000.0;

  float monopolyPenalty = intersections[i].consecutiveGreens * 3.0;
  p -= monopolyPenalty;
  
  // EMERGENCY LOGIC: If Slave reports it OR Master physical button is pressed (LOW)
  if (intersections[i].data.emergency || digitalRead(PIN_EMERGENCY_BTN) == LOW) {
    p += 20.0; // Massive priority boost
  }
  return max(p, 0.1);
}

// ============================================================================
// FULL DASHBOARD HTML
// ============================================================================
const char dashboard_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Traffic Control Dashboard</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { 
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      background: #0f172a; 
      color: white; 
      padding: 20px;
    }
    .container { max-width: 1200px; margin: 0 auto; }
    h1 { text-align: center; margin-bottom: 30px; font-size: 2em; }
    
    .header-card {
      background: #1e293b;
      padding: 20px;
      border-radius: 10px;
      margin-bottom: 20px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      flex-wrap: wrap;
      gap: 15px;
    }
    
    .status-badge {
      display: inline-flex;
      align-items: center;
      gap: 8px;
      padding: 8px 16px;
      border-radius: 20px;
      font-weight: 600;
    }
    .status-connected { background: #10b981; }
    .status-disconnected { background: #ef4444; }
    
    .controls { display: flex; gap: 10px; flex-wrap: wrap; }
    .btn {
      padding: 10px 20px;
      border: none;
      border-radius: 6px;
      font-weight: 600;
      cursor: pointer;
      transition: all 0.2s;
    }
    .btn-auto { background: #3b82f6; color: white; }
    .btn-auto:hover { background: #2563eb; }
    .btn-manual { background: #f59e0b; color: white; }
    .btn-manual:hover { background: #d97706; }
    .btn-manual.active { background: #dc2626; }
    
    .stats {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
      gap: 15px;
      margin-bottom: 20px;
    }
    .stat-card {
      background: #1e293b;
      padding: 15px;
      border-radius: 8px;
    }
    .stat-label { color: #94a3b8; font-size: 0.9em; margin-bottom: 5px; }
    .stat-value { font-size: 1.8em; font-weight: bold; }
    
    .intersections {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
      gap: 20px;
    }
    
    .intersection-card {
      background: #1e293b;
      padding: 20px;
      border-radius: 10px;
      border: 2px solid transparent;
      transition: border-color 0.3s;
    }
    .intersection-card.selected { border-color: #3b82f6; }
    
    .light-display {
      display: flex;
      align-items: center;
      gap: 15px;
      margin-bottom: 15px;
    }
    .light-circle {
      width: 60px;
      height: 60px;
      border-radius: 50%;
      display: flex;
      align-items: center;
      justify-content: center;
      font-size: 2em;
      box-shadow: 0 4px 6px rgba(0,0,0,0.3);
    }
    .light-RED { background: #ef4444; }
    .light-YELLOW { background: #f59e0b; }
    .light-GREEN { background: #10b981; }
    
    .light-info h3 { font-size: 1.5em; margin-bottom: 5px; }
    .light-info .mode { color: #94a3b8; font-size: 0.9em; }
    
    .sensors {
      background: #0f172a;
      padding: 12px;
      border-radius: 6px;
      margin-bottom: 15px;
    }
    .sensor-row {
      display: flex;
      justify-content: space-between;
      margin-bottom: 8px;
      font-size: 0.95em;
    }
    .sensor-icon { font-size: 1.2em; }
    
    .queue-bar {
      height: 8px;
      background: #334155;
      border-radius: 4px;
      overflow: hidden;
      margin-top: 8px;
    }
    .queue-fill {
      height: 100%;
      transition: width 0.3s;
      border-radius: 4px;
    }
    .queue-high { background: #ef4444; }
    .queue-med { background: #f59e0b; }
    .queue-low { background: #10b981; }
    
    .manual-controls {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 10px;
    }
    .manual-controls .btn {
      width: 100%;
      padding: 12px;
    }
    .btn-green { background: #10b981; color: white; }
    .btn-green:hover { background: #059669; }
    .btn-green:disabled { background: #334155; cursor: not-allowed; }
    .btn-red { background: #ef4444; color: white; }
    .btn-red:hover { background: #dc2626; }
    .btn-red:disabled { background: #334155; cursor: not-allowed; }
    
    .warning {
      background: #854d0e;
      border: 2px solid #f59e0b;
      padding: 15px;
      border-radius: 8px;
      margin-top: 20px;
      display: flex;
      gap: 10px;
      align-items: center;
    }
    .warning-icon { font-size: 1.5em; }
  </style>
</head>
<body>
  <div class="container">
    <h1>🚦 Smart Traffic Control System</h1>
    
    <div class="header-card">
      <div class="status-badge status-connected" id="connection-status">
        <span>●</span>
        <span>Connected</span>
      </div>
      
      <div class="controls">
        <button class="btn btn-auto" id="btn-auto" onclick="setMode('auto')">
          🤖 Auto Mode
        </button>
        <button class="btn btn-manual" id="btn-manual" onclick="setMode('manual')">
          👤 Manual Mode
        </button>
      </div>
    </div>
    
    <div class="stats">
      <div class="stat-card">
        <div class="stat-label">Cycle Count</div>
        <div class="stat-value" id="cycle-count">0</div>
      </div>
      <div class="stat-card">
        <div class="stat-label">Uptime</div>
        <div class="stat-value" id="uptime">0m</div>
      </div>
      <div class="stat-card">
        <div class="stat-label">System Health</div>
        <div class="stat-value" style="color: #10b981">OPTIMAL</div>
      </div>
    </div>
    
    <div class="intersections" id="intersections"></div>
    
    <div class="warning" id="manual-warning" style="display: none;">
      <div class="warning-icon">⚠️</div>
      <div>
        <strong>Manual Mode Active</strong><br>
        <small>Ensure only ONE intersection has green at a time!</small>
      </div>
    </div>
  </div>

  <script>
    let manualMode = false;
    
    async function setMode(mode) {
      manualMode = (mode === 'manual');
      
      try {
        await fetch('/api/mode', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({manual: manualMode})
        });
      } catch(e) { console.error(e); }
      
      updateModeUI();
    }
    
    function updateModeUI() {
      document.getElementById('btn-auto').classList.toggle('active', !manualMode);
      document.getElementById('btn-manual').classList.toggle('active', manualMode);
      document.getElementById('manual-warning').style.display = manualMode ? 'flex' : 'none';
      updateUI();
    }
    
    async function forceLight(id, state) {
      if (!manualMode) return;
      try {
        await fetch('/api/command', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({id: id, state: state})
        });
      } catch(e) { console.error(e); }
    }
    
    async function updateUI() {
      try {
        const res = await fetch('/api/status');
        const data = await res.json();
        
        document.getElementById('cycle-count').textContent = data.cycleCount;
        document.getElementById('uptime').textContent = Math.floor(data.uptime/60) + 'm';
        
        let html = '';
        data.intersections.forEach((int, i) => {
          const queuePct = int.queue * 100;
          const queueClass = queuePct > 75 ? 'queue-high' : queuePct > 40 ? 'queue-med' : 'queue-low';
          
          html += `
            <div class="intersection-card">
              <div class="light-display">
                <div class="light-circle light-${int.state}">
                  ${int.state === 'GREEN' ? '🟢' : int.state === 'YELLOW' ? '🟡' : '🔴'}
                </div>
                <div class="light-info">
                  <h3>Intersection ${i}</h3>
                  <div class="mode">${int.state} - ${int.mode}</div>
                </div>
              </div>
              
              <div class="sensors">
                <div class="sensor-row">
                  <span>50m Sensor:</span>
                  <span class="sensor-icon">${int.far ? '🚗' : '⚪'}</span>
                </div>
                <div class="sensor-row">
                  <span>20m Sensor:</span>
                  <span class="sensor-icon">${int.near ? '🚗' : '⚪'}</span>
                </div>
                <div class="sensor-row">
                  <span>Priority:</span>
                  <span>${int.priority.toFixed(2)}</span>
                </div>
                <div class="sensor-row">
                  <span>Wait Time:</span>
                  <span>${int.wait}s</span>
                </div>
                <div class="queue-bar">
                  <div class="queue-fill ${queueClass}" style="width: ${queuePct}%"></div>
                </div>
              </div>
              
              ${manualMode ? `
              <div class="manual-controls">
                <button class="btn btn-green" 
                  ${int.state === 'GREEN' ? 'disabled' : ''} 
                  onclick="forceLight(${i}, 'GREEN')">
                  🟢 GREEN
                </button>
                <button class="btn btn-red" 
                  ${int.state === 'RED' ? 'disabled' : ''} 
                  onclick="forceLight(${i}, 'RED')">
                  🔴 RED
                </button>
              </div>
              ` : ''}
            </div>
          `;
        });
        
        document.getElementById('intersections').innerHTML = html;
      } catch(e) {
        console.error(e);
        document.getElementById('connection-status').className = 'status-badge status-disconnected';
        document.getElementById('connection-status').innerHTML = '<span>●</span><span>Disconnected</span>';
      }
    }
    
    updateModeUI();
    setInterval(updateUI, 1000);
    updateUI();
  </script>
</body>
</html>
)rawliteral";

// ============================================================================
// SETUP & LOOP
// ============================================================================

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  
  Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("Master Channel: %d\n", WiFi.channel());

  if (esp_now_init() != ESP_OK) ESP.restart();
  esp_now_register_recv_cb(onReceive);

  for (int i=0; i<NUM_SLAVES; i++) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, slaveMACs[i], 6);
    peer.channel = WiFi.channel();
    esp_now_add_peer(&peer);
  }

  // Main dashboard
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", dashboard_html);
  });

  // Status API
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
    JsonDocument doc;
    doc["cycleCount"] = cycleCount;
    doc["uptime"] = millis()/1000;
    JsonArray arr = doc["intersections"].to<JsonArray>();
    
    for(int i=0; i<NUM_SLAVES; i++) {
      JsonObject obj = arr.add<JsonObject>();
      obj["id"] = i;
      obj["state"] = (intersections[i].currentLight == STATE_GREEN) ? "GREEN" : 
                     (intersections[i].currentLight == STATE_YELLOW) ? "YELLOW" : "RED";
      obj["near"] = intersections[i].data.nearSensorOccupied;
      obj["far"] = intersections[i].data.farSensorOccupied;
      obj["priority"] = getPriority(i);
      obj["wait"] = (int)((millis() - intersections[i].lastGreenEnd)/1000);
      obj["queue"] = (intersections[i].data.nearSensorOccupied && intersections[i].data.farSensorOccupied) ? 1.0 : 
                     intersections[i].data.nearSensorOccupied ? 0.5 : 0.0;
      obj["mode"] = manualMode ? "MANUAL" : "AUTO";
    }
    
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  // Mode control
  server.on("/api/mode", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      JsonDocument doc;
      deserializeJson(doc, data);
      manualMode = doc["manual"];
      Serial.printf("Mode changed to: %s\n", manualMode ? "MANUAL" : "AUTO");
      request->send(200, "text/plain", "OK");
    });

  // Manual commands
  server.on("/api/command", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      if (!manualMode) {
        request->send(403, "text/plain", "Not in manual mode");
        return;
      }
      
      JsonDocument doc;
      deserializeJson(doc, data);
      int id = doc["id"];
      String state = doc["state"];
      
      if (id >= 0 && id < NUM_SLAVES) {
        if (state == "GREEN") {
          intersections[id].currentLight = STATE_GREEN;
          sendCmd(id, STATE_GREEN, 15);
        } else if (state == "RED") {
          intersections[id].currentLight = STATE_RED;
          sendCmd(id, STATE_RED, 0);
        }
      }
      
      request->send(200, "text/plain", "OK");
    });
  
  server.begin();
  startTime = millis();
  Serial.println("System ready!");
pinMode(PIN_EMERGENCY_BTN, INPUT_PULLUP);
}

void loop() {
  // Keep-alive: Send RED to inactive slaves every 5 seconds
  static unsigned long lastKeepAlive = 0;
  if (millis() - lastKeepAlive > 5000) {
    lastKeepAlive = millis();
    for(int i=0; i<NUM_SLAVES; i++) {
      if (i != activeIdx) {
        sendCmd(i, STATE_RED, 0);
      }
    }
  }
  
  if (manualMode) {
    delay(100);
    return;
  }
  if (manualMode) {
    delay(100);
    return;
  }
  
  unsigned long now = millis();
  bool emergencyActive = (digitalRead(PIN_EMERGENCY_BTN) == LOW);

  if (isAllRed) {
    if (now - allRedStart >= ALL_RED_TIME) {
      // 1. CLEAR THE STATE
      isAllRed = false;
      int winner = -1; 
      float topP = -1.0;

      // 2. FIND THE BEST CANDIDATE
      for(int i=0; i<NUM_SLAVES; i++) {
        float p = getPriority(i);
        if(p > topP) { topP = p; winner = i; }
      }

      // 3. THE CRITICAL CHANGE: Tell everyone who is NOT the winner to stay RED
      // This prevents Slaves from timing out or entering failsafe.
      for(int i=0; i<NUM_SLAVES; i++) {
        if(i != winner) {
          sendCmd(i, STATE_RED, 0); 
        }
      }

      if(winner != -1) {
        activeIdx = winner;
        intersections[winner].currentLight = STATE_GREEN;
        intersections[winner].stateStart = now;

        // Track consecutive greens (ADD THIS)
        intersections[winner].consecutiveGreens++;
        for(int i=0; i<NUM_SLAVES; i++) {
          if(i != winner) {
           intersections[i].consecutiveGreens = 0;
    }
  }

        intersections[winner].plannedDuration = constrain((int)(G_MIN + (topP*7.5)), (int)G_MIN, (int)G_MAX);
        sendCmd(winner, STATE_GREEN, intersections[winner].plannedDuration);
      }
    }
  } else if (activeIdx != -1) {
    unsigned long elapsed = now - intersections[activeIdx].stateStart;
    
    if (intersections[activeIdx].currentLight == STATE_GREEN) {
      bool shouldEndGreen = (elapsed >= (intersections[activeIdx].plannedDuration * 1000UL));
      if (emergencyActive && elapsed > 2000) shouldEndGreen = true;

      if (shouldEndGreen) {
        intersections[activeIdx].currentLight = STATE_YELLOW;
        intersections[activeIdx].stateStart = now;
        sendCmd(activeIdx, STATE_YELLOW, (int)YELLOW_TIME);
      }
    } else if (intersections[activeIdx].currentLight == STATE_YELLOW) {
      if (elapsed >= (YELLOW_TIME * 1000UL)) {
        // END OF CYCLE: Tell the active one to go Red
        sendCmd(activeIdx, STATE_RED, 0);
        intersections[activeIdx].currentLight = STATE_RED;
        intersections[activeIdx].lastGreenEnd = now;
        
        activeIdx = -1; 
        isAllRed = true; 
        allRedStart = now;
        cycleCount++;
      }
    }
  } else { 
    isAllRed = true; 
    allRedStart = now; 
  }
}