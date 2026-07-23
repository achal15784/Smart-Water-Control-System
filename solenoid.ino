#define TINY_GSM_RX_BUFFER 1024
#define TINY_GSM_MODEM_SIM800

#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include <Preferences.h> // Native ESP32 Non-Volatile Storage library

// Pin Configurations
#define RELAY_OPEN_PIN  25
#define RELAY_CLOSE_PIN 26
#define FLOW_SENSOR_PIN 32
#define LED_PIN         2   // Onboard LED Pin (GPIO 2 is standard for most ESP32 boards)

#define SerialAT        Serial1
#define RX              16
#define TX              17
#define APN             "cmnet"

// ThingSpeak MQTT Configuration
const char* mqttServer   = "mqtt3.thingspeak.com";
const int   mqttPort     = 1883;
const char* mqttClientID = "Kh4wFSMYBBcAOwkgIgISBSs";
const char* mqttUserName = "Kh4wFSMYBBcAOwkgIgISBSs";
const char* mqttPassword = "NyE4P7R/0fjcPOIkJv5Ukq1g";
const long  channelID    = 3403978;

const char* subTopicField6 = "channels/3403978/subscribe/fields/field6";
const char* subTopicField3 = "channels/3403978/subscribe/fields/field3";
const char* pubTopic       = "channels/3403978/publish";

// Flow Meter Control Variables
volatile uint32_t pulseCount = 0;
float flowRate       = 0.0;
float totalVolume    = 0.0;
float targetLimit    = 20.0; // Set to 0.0 via dashboard/code for continuous un-targeted flow
int   valveState     = 0;
bool  autoStopped    = false;   

const float calibrationFactor = 422.791;
const float OVERSHOOT         = 0.050;  // 50ml early-close compensation

// Variables for Calculating Average Flow Rate over the 5-second Window
float flowRateSum      = 0.0;
unsigned int flowSampleCount = 0;

// Timing Tracking Variables
unsigned long oldTime          = 0;
unsigned long lastPulseCount   = 0;     
unsigned long lastPublishTime  = 0;
unsigned long lastNetworkCheck = 0;

const unsigned long pubInterval    = 5000;  // Publish payload every 5 seconds (Paid Tier)
const unsigned long netCheckInterval = 10000; // Verify network health every 10 seconds

// Fail-Safe Connection Tracking Variables
unsigned long disconnectedTimestamp = 0; 
bool isTrackingDisconnect           = false;
const unsigned long maxDisconnectDuration = 5000; // 5 seconds of disconnection allowed before valve gets forced ON

// Non-Volatile Memory (Preferences) tracking variables
Preferences preferences;
float lastSavedVolume = 0.0; 

TinyGsm    modem(SerialAT);
TinyGsmClient client(modem);
PubSubClient  mqtt(client);

// Interrupt Service Routine (Hardware Layer - Never misses a pulse)
void IRAM_ATTR pulseCounterISR() {
  pulseCount++;
}

// Function to publish data back to your dashboard
void publishTelemetry() {
  float avgFlowRate = 0.0;
  if (flowSampleCount > 0) {
    avgFlowRate = flowRateSum / flowSampleCount;
  } else {
    avgFlowRate = flowRate; 
  }

  String payload = "field2=" + String(avgFlowRate, 2) +
                   "&field3=" + String(targetLimit, 1) +
                   "&field4=" + String(totalVolume, 3) +
                   "&field5=" + String(valveState);

  Serial.print("Publishing Payload (5s Window): ");
  Serial.println(payload);

  if (mqtt.publish(pubTopic, payload.c_str())) {
    Serial.println("Telemetry successfully pushed to ThingSpeak.");
    flowRateSum = 0.0;
    flowSampleCount = 0;
  } else {
    Serial.println("Telemetry update failed to publish.");
  }
}

// Controls Valve Relays using safe pulse logic (Active-LOW Configuration)
void setValveState(int state) {
  valveState = state;

  if (valveState == 1) {
    Serial.println(">>> Pulsing Valve OPEN...");
    // Ensure opposing relay is strictly OFF (HIGH)
    digitalWrite(RELAY_CLOSE_PIN, HIGH);
    delay(10); 
    
    // Trigger 100ms pulse on OPEN relay
    digitalWrite(RELAY_OPEN_PIN, LOW);   // Energize
    delay(100);
    digitalWrite(RELAY_OPEN_PIN, HIGH);  // Release
    Serial.println(">>> Valve OPEN complete. Relays released.");
  } else {
    Serial.println(">>> Pulsing Valve CLOSE...");
    // Ensure opposing relay is strictly OFF (HIGH)
    digitalWrite(RELAY_OPEN_PIN, HIGH);
    delay(10);
    
    // Trigger 100ms pulse on CLOSE relay
    digitalWrite(RELAY_CLOSE_PIN, LOW);  // Energize
    delay(100);
    digitalWrite(RELAY_CLOSE_PIN, HIGH); // Release
    Serial.println(">>> Valve CLOSE complete. Relays released.");
  }
  
  // Save state execution to permanent memory
  preferences.begin("flow-meter", false);
  preferences.putInt("valveState", valveState);
  preferences.end();
  
  if (mqtt.connected()) {
    publishTelemetry();
  }
}

// Processes incoming commands from Dashboard (Field 3 and Field 6)
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.print("MQTT Received [");
  Serial.print(topic);
  Serial.print("] Message: ");
  Serial.println(message);

  if (strstr(topic, "field6") != NULL) {
    int commandCode = message.toInt();
    Serial.print("Parsed Command Code: ");
    Serial.println(commandCode);

    if (commandCode == 1) {
      autoStopped = false;   
      setValveState(1);
    }
    else if (commandCode == 0) {
      setValveState(0);
    }
    else if (commandCode == 2) {
      Serial.println("System Reset Request Triggered.");
      noInterrupts();
      pulseCount = 0;
      interrupts();
      lastPulseCount = 0;   
      totalVolume    = 0.0;
      flowRate       = 0.0;
      flowRateSum    = 0.0;
      flowSampleCount = 0;
      autoStopped    = false;
      
      // Clear data inside non-volatile preferences memory block
      preferences.begin("flow-meter", false);
      preferences.putFloat("totalVolume", 0.0);
      preferences.putUInt("pulseCount", 0);
      lastSavedVolume = 0.0;
      preferences.end();

      setValveState(0);
    }
  }
  else if (strstr(topic, "field3") != NULL) {
    float newTarget = message.toFloat();
    if (newTarget >= 0) {
      targetLimit = newTarget;
      Serial.print("Updated Target Limit: ");
      Serial.println(targetLimit);
      
      preferences.begin("flow-meter", false);
      preferences.putFloat("targetLimit", targetLimit);
      preferences.end();
    }
  }
}

// Asynchronous Non-blocking connection management 
void connectNetwork() {
  if (!modem.isNetworkConnected()) {
    Serial.println("Cell network dropped! Attempting quick registration check...");
    if (!modem.waitForNetwork(5000L)) {
      Serial.println("Network still scanning. Moving forward locally...");
      return;
    }
    Serial.println("NETWORK CONNECTED");
  }

  if (!modem.isGprsConnected()) {
    Serial.println("GPRS disconnected. Reconnecting context...");
    if (!modem.gprsConnect(APN)) {
      Serial.println("GPRS handshake failed.");
      return;
    }
    Serial.println("GPRS CONNECTED");
  }

  if (!mqtt.connected()) {
    Serial.println("Connecting to ThingSpeak MQTT Broker...");
    if (mqtt.connect(mqttClientID, mqttUserName, mqttPassword)) {
      Serial.println("MQTT Broker Online!");
      mqtt.subscribe(subTopicField3);
      mqtt.subscribe(subTopicField6);
    } else {
      Serial.print("MQTT connection failed, state: ");
      Serial.println(mqtt.state());
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(10);

  pinMode(RELAY_OPEN_PIN,  OUTPUT);
  pinMode(RELAY_CLOSE_PIN, OUTPUT);
  pinMode(LED_PIN,         OUTPUT);
  
  // Set initial status to OFF for all pins (HIGH = Relays & LED off for active-low)
  digitalWrite(RELAY_OPEN_PIN, HIGH);
  digitalWrite(RELAY_CLOSE_PIN, HIGH);
  digitalWrite(LED_PIN, HIGH);

  // --- NON-VOLATILE PREFERENCES RECOVERY ---
  preferences.begin("flow-meter", false);
  totalVolume = preferences.getFloat("totalVolume", 0.0);
  targetLimit = preferences.getFloat("targetLimit", 20.0);
  
  noInterrupts();
  pulseCount = preferences.getUInt("pulseCount", 0);
  interrupts();
  
  lastSavedVolume = totalVolume;
  lastPulseCount = pulseCount;
  preferences.end();

  Serial.println("--- Boot Flash Memory Recovery Completed ---");
  Serial.print("Recovered Total Volume: "); Serial.println(totalVolume, 3);
  Serial.print("Recovered Target Limit: "); Serial.println(targetLimit, 1);
  Serial.print("Recovered Pulse Count: ");  Serial.println(pulseCount);

  // CRITICAL REQUIREMENT CHANGE: On sudden boot/reboot, valve immediately gets pulsed to OPEN (1)
  setValveState(1);

  pinMode(FLOW_SENSOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), pulseCounterISR, FALLING);

  SerialAT.begin(9600, SERIAL_8N1, RX, TX);
  delay(3000);

  Serial.println("Initialise SIM800L...");
  if (!modem.restart()) {
    Serial.println("SIM800L hardware failed to respond.");
  } else {
    Serial.println("SIM800L initialized.");
  }

  if (!modem.getSimStatus()) {
    Serial.println("CRITICAL: SIM CARD NOT INSERTED OR LOCKED.");
    while (1);
  }
  Serial.println("SIM detected successfully.");

  mqtt.setServer(mqttServer, mqttPort);
  mqtt.setCallback(mqttCallback);

  connectNetwork();
  oldTime = millis();
}

void loop() {
  unsigned long currentMillis = millis();

  // --- FAIL-SAFE & LED EVALUATION (Active-LOW Configuration) ---
  if (!modem.isGprsConnected() || !mqtt.connected()) {
    digitalWrite(LED_PIN, HIGH); 

    if (!isTrackingDisconnect) {
      disconnectedTimestamp = currentMillis;
      isTrackingDisconnect = true;
      Serial.println("!!! Connection anomaly detected. Starting fail-safe countdown timer...");
    } else {
      if ((currentMillis - disconnectedTimestamp >= maxDisconnectDuration) && valveState != 1) {
        Serial.println("!!! CRITICAL FAIL-SAFE: Long-term connection loss. Forcing Valve ON!");
        autoStopped = false;
        setValveState(1); 
      }
    }
  } else {
    digitalWrite(LED_PIN, LOW);  

    if (isTrackingDisconnect) {
      Serial.println("Connection restored. Disarm fail-safe timer.");
      isTrackingDisconnect = false;
    }
  }

  // 1. HIGH PRIORITY TASK: Run calculations and safety loops every 100ms
  if ((currentMillis - oldTime) >= 100) {
    oldTime = currentMillis;

    noInterrupts();
    unsigned long currentPulses = pulseCount;
    interrupts();

    unsigned long pulseDelta = currentPulses - lastPulseCount;
    lastPulseCount = currentPulses;

    flowRate    = (pulseDelta * 60.0) / calibrationFactor;
    totalVolume = currentPulses / calibrationFactor;

    flowRateSum += flowRate;
    flowSampleCount++;

    // FLASH MEMORY PROTECTION CONDITION:
    if (totalVolume - lastSavedVolume >= 0.1) {
      lastSavedVolume = totalVolume;
      preferences.begin("flow-meter", false);
      preferences.putFloat("totalVolume", totalVolume);
      preferences.putUInt("pulseCount", currentPulses);
      preferences.end();
      Serial.println(">>> Flash Storage Synced (0.1L Increment Saved).");
    }

    if (valveState == 1) {
      Serial.print("Flow: "); Serial.print(flowRate);
      Serial.print(" L/min | Dispensed: "); Serial.print(totalVolume, 3);
      
      if (targetLimit > 0.0) {
        Serial.print("L / target: "); Serial.print(targetLimit); Serial.println("L");
      } else {
        Serial.println("L / [Continuous Mode - No Target]");
      }
    }

    // Automated Target Limit Intercept
    if (valveState == 1 && !autoStopped && targetLimit > 0.0 && totalVolume >= (targetLimit - OVERSHOOT)) {
      Serial.print("Target intercepted at "); Serial.print(totalVolume, 3); Serial.println("L.");
      autoStopped = true;
      setValveState(0); 
    }
  }

  // 2. BACKGROUND MQTT TICK: Listen for incoming dashboard changes
  if (mqtt.connected()) {
    mqtt.loop();
  }

  // 3. LOW PRIORITY TIMED TASK: Run connection health assessment every 10 seconds
  if (currentMillis - lastNetworkCheck >= netCheckInterval) {
    lastNetworkCheck = currentMillis;
    connectNetwork();
  }

  // 4. TELEMETRY TASK: Publish regular logs every 5 seconds
  if (currentMillis - lastPublishTime >= pubInterval) {
    lastPublishTime = currentMillis;
    if (mqtt.connected()) {
      publishTelemetry();
    }
  }
}