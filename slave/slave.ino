#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ============================================================================
// CONFIGURATION - CHANGE FOR EACH SLAVE!
// ============================================================================

#define INTERSECTION_ID 0  // ← CHANGE THIS: 0, 1, or 2 for each slave

// Master MAC Address
uint8_t masterMAC[6] = {0x28, 0x05, 0xA5, 0x6F, 0x80, 0xDC};

// Pin Definitions - Traffic Lights
#define PIN_RED_LIGHT 4
#define PIN_YELLOW_LIGHT 16
#define PIN_GREEN_LIGHT 17

// Pin Definitions - Ultrasonic Sensors
#define PIN_ULTRASONIC_TRIG_NEAR 32  // 20m sensor
#define PIN_ULTRASONIC_ECHO_NEAR 33
#define PIN_ULTRASONIC_TRIG_FAR 18   // 50m sensor
#define PIN_ULTRASONIC_ECHO_FAR 19

// Pin Definitions - Emergency & Status
#define PIN_EMERGENCY_BTN 25  // Emergency button
#define PIN_AUTO_LED 15
#define PIN_FAILSAFE_LED 5

// Constants
#define DETECTION_THRESHOLD 190
#define SENSOR_READ_INTERVAL 500
#define DATA_SEND_INTERVAL 1000
#define WATCHDOG_TIMEOUT 30000  // 30 seconds

#define NUM_SLAVES 3
#define FAILSAFE_GREEN 10
#define FAILSAFE_YELLOW 3
#define FAILSAFE_RED 20

// ============================================================================
// DATA STRUCTURES - MUST MATCH MASTER EXACTLY!
// ============================================================================

enum LightState {
  STATE_RED = 0,
  STATE_YELLOW = 1,
  STATE_GREEN = 2
};

enum OperationMode {
  MODE_AUTO = 0,
  MODE_FAILSAFE = 1
};

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

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

LightState currentState = STATE_RED;
OperationMode operationMode = MODE_AUTO;

unsigned long stateStartTime = 0;
unsigned long stateDuration = 0;
unsigned long lastMasterCommand = 0;

bool nearSensorOccupied = false;
bool farSensorOccupied = false;

struct Stats {
  unsigned long commandsReceived;
  unsigned long dataPacketsSent;
  
  Stats() : commandsReceived(0), dataPacketsSent(0) {}
} stats;

// ============================================================================
// ULTRASONIC SENSORS
// ============================================================================

uint16_t readUltrasonicDistance(uint8_t trigPin, uint8_t echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  
  long duration = pulseIn(echoPin, HIGH, 30000);
  if (duration == 0) return 200;
  
  uint16_t distance = duration * 0.034 / 2;
  return constrain(distance, 0, 200);
}

void updateSensors() {
  uint16_t distNear = readUltrasonicDistance(PIN_ULTRASONIC_TRIG_NEAR, 
                                              PIN_ULTRASONIC_ECHO_NEAR);
  uint16_t distFar = readUltrasonicDistance(PIN_ULTRASONIC_TRIG_FAR, 
                                             PIN_ULTRASONIC_ECHO_FAR);
  
  nearSensorOccupied = (distNear < DETECTION_THRESHOLD);
  farSensorOccupied = (distFar < DETECTION_THRESHOLD);
}

// ============================================================================
// TRAFFIC LIGHT CONTROL
// ============================================================================

void setLights(bool red, bool yellow, bool green) {
  digitalWrite(PIN_RED_LIGHT, red ? HIGH : LOW);
  digitalWrite(PIN_YELLOW_LIGHT, yellow ? HIGH : LOW);
  digitalWrite(PIN_GREEN_LIGHT, green ? HIGH : LOW);
}

void updateLights() {
  switch (currentState) {
    case STATE_RED:
      setLights(true, false, false);
      break;
    case STATE_YELLOW:
      setLights(false, true, false);
      break;
    case STATE_GREEN:
      setLights(false, false, true);
      break;
  }
}

void updateStatusLEDs() {
  digitalWrite(PIN_AUTO_LED, operationMode == MODE_AUTO ? HIGH : LOW);
  
  if (operationMode == MODE_FAILSAFE) {
    digitalWrite(PIN_FAILSAFE_LED, (millis() / 500) % 2);
  } else {
    digitalWrite(PIN_FAILSAFE_LED, LOW);
  }
}

void transitionToState(LightState newState, unsigned long durationMs) {
  currentState = newState;
  stateStartTime = millis();
  stateDuration = durationMs;
  updateLights();
  
  const char* stateStr[] = {"RED", "YELLOW", "GREEN"};
  const char* modeStr[] = {"AUTO", "FAILSAFE"};
  Serial.printf("[%s] → %s (%lus)\n", 
                modeStr[operationMode], stateStr[newState], durationMs / 1000);
}

// ============================================================================
// OPERATION MODES
// ============================================================================

void setOperationMode(OperationMode newMode) {
  if (newMode == operationMode) return;
  
  operationMode = newMode;
  
  const char* modeStr[] = {"AUTO", "FAILSAFE"};
  Serial.printf("🔄 MODE: %s\n", modeStr[newMode]);
  
  if (newMode == MODE_FAILSAFE) {
    transitionToState(STATE_RED, FAILSAFE_RED * 1000UL);
  }
  
  updateStatusLEDs();
}

// ============================================================================
// FAILSAFE MODE
// ============================================================================

void runFailsafeMode() {
  unsigned long elapsed = millis() - stateStartTime;
  
  switch (currentState) {
    case STATE_GREEN:
      if (elapsed >= (FAILSAFE_GREEN * 1000UL)) {
        transitionToState(STATE_YELLOW, FAILSAFE_YELLOW * 1000UL);
      }
      break;
      
    case STATE_YELLOW:
      if (elapsed >= (FAILSAFE_YELLOW * 1000UL)) {
        transitionToState(STATE_RED, FAILSAFE_RED * 1000UL);
      }
      break;
      
    case STATE_RED:
      if (elapsed >= (FAILSAFE_RED * 1000UL)) {
        unsigned long phaseDuration = (FAILSAFE_GREEN + FAILSAFE_YELLOW + FAILSAFE_RED) * 1000UL;
        unsigned long totalCycleDuration = phaseDuration * NUM_SLAVES;
        unsigned long systemTime = millis() % totalCycleDuration;
        uint8_t activeIntersection = systemTime / phaseDuration;
        
        if (activeIntersection == INTERSECTION_ID) {
          transitionToState(STATE_GREEN, FAILSAFE_GREEN * 1000UL);
        }
      }
      break;
  }
}

// ============================================================================
// ESP-NOW
// ============================================================================

void onReceiveCommand(const esp_now_recv_info *info, const uint8_t *data, int len) {
  if (len != sizeof(Command)) return;
  if (operationMode != MODE_AUTO) return;
  
  Command cmd;
  memcpy(&cmd, data, sizeof(cmd));
  
  lastMasterCommand = millis();
  stats.commandsReceived++;
  
  LightState targetState = (LightState)cmd.targetState;
  
  if (targetState != currentState) {
    transitionToState(targetState, cmd.duration * 1000UL);
  }
}

void sendDataToMaster() {
  TrafficData data;
  data.id = INTERSECTION_ID;
  data.nearSensorOccupied = nearSensorOccupied ? 1 : 0;
  data.farSensorOccupied = farSensorOccupied ? 1 : 0;
  data.currentState = (uint8_t)currentState;
  data.operationMode = (uint8_t)operationMode;
  data.emergency = (digitalRead(PIN_EMERGENCY_BTN) == LOW) ? 1 : 0;

  esp_err_t result = esp_now_send(masterMAC, (uint8_t*)&data, sizeof(TrafficData));
  
  if (result == ESP_OK) {
    stats.dataPacketsSent++;
  }
}

// ============================================================================
// WATCHDOG
// ============================================================================

void checkWatchdog() {
  if (operationMode != MODE_AUTO) return;
  if (millis() - lastMasterCommand > WATCHDOG_TIMEOUT) {
    setOperationMode(MODE_FAILSAFE);
  }
}

void checkFailsafeRecovery() {
  if (operationMode == MODE_FAILSAFE) {
    if (millis() - lastMasterCommand < WATCHDOG_TIMEOUT) {
      setOperationMode(MODE_AUTO);
    }
  }
}

// ============================================================================
// SETUP & LOOP
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.printf("║  Traffic Slave %d - Final Version    ║\n", INTERSECTION_ID);
  Serial.println("╚════════════════════════════════════════╝\n");
  
  // Initialize WiFi FIRST
  WiFi.mode(WIFI_STA);
  delay(100);
  
  Serial.print("Slave MAC: ");
  Serial.println(WiFi.macAddress());
  
  // Configure pins
  pinMode(PIN_RED_LIGHT, OUTPUT);
  pinMode(PIN_YELLOW_LIGHT, OUTPUT);
  pinMode(PIN_GREEN_LIGHT, OUTPUT);
  pinMode(PIN_AUTO_LED, OUTPUT);
  pinMode(PIN_FAILSAFE_LED, OUTPUT);
  pinMode(PIN_EMERGENCY_BTN, INPUT_PULLUP);
  
  pinMode(PIN_ULTRASONIC_TRIG_NEAR, OUTPUT);
  pinMode(PIN_ULTRASONIC_ECHO_NEAR, INPUT);
  pinMode(PIN_ULTRASONIC_TRIG_FAR, OUTPUT);
  pinMode(PIN_ULTRASONIC_ECHO_FAR, INPUT);
  
  transitionToState(STATE_RED, 0);
  updateStatusLEDs();
  
  // Set WiFi channel to 11 (match master)
  esp_wifi_set_channel(11, WIFI_SECOND_CHAN_NONE);
  Serial.println("WiFi Channel: 11");
  
  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ ESP-NOW init failed");
    return;
  }
  
  esp_now_register_recv_cb(onReceiveCommand);
  
  // Register master
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, masterMAC, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("❌ Failed to add master");
    return;
  }
  
  Serial.print("Master MAC: ");
  for (int i = 0; i < 6; i++) {
    Serial.printf("%02X", masterMAC[i]);
    if (i < 5) Serial.print(":");
  }
  Serial.println("\n✓ System ready\n");
  
  lastMasterCommand = millis();
}

void loop() {
  static unsigned long lastSensorRead = 0;
  static unsigned long lastDataSend = 0;
  unsigned long now = millis();
  
  // Update sensors
  if (now - lastSensorRead >= SENSOR_READ_INTERVAL) {
    lastSensorRead = now;
    updateSensors();
  }
  
  // Send data to master
  if (now - lastDataSend >= DATA_SEND_INTERVAL) {
    lastDataSend = now;
    sendDataToMaster();
  }
  
  // Mode-specific logic
  switch (operationMode) {
    case MODE_AUTO:
      checkWatchdog();
      break;
    case MODE_FAILSAFE:
      checkFailsafeRecovery();
      runFailsafeMode();
      break;
  }
  
  updateStatusLEDs();
  delay(20);
}