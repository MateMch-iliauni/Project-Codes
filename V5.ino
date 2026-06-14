// ──────────────────────────────────────────────────────────────
// Thermal Camera Fire-Loss Alarm System
// ESP32 + MLX90640 + Buzzer + Button + Telegram
// TARGET: Gas stove monitoring
// ──────────────────────────────────────────────────────────────

#include <Wire.h>
#include <Adafruit_MLX90640.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>

// ──────────────────────────────────────────────────────────────
// THRESHOLDS — tuned for gas stove monitoring
// ──────────────────────────────────────────────────────────────

// Stove OFF = ~25-35°C. Fire triggers above this fixed value.
const float FIRE_ON_THRESHOLD  = 80.0f;   // °C — flame detected above this

// Flame just went out — stove starts cooling rapidly.
// 80°C is well above idle but clearly below active flame.
const float FIRE_OFF_THRESHOLD = 55.0f;   // °C — fire considered lost below this

// How many consecutive cold frames before alarm fires.
// At 2Hz sensor rate: 5 frames = ~2.5 seconds confirmation.
const int COLD_FRAMES_REQUIRED = 8;

// Cooling rate detection — catches slow-cooling cookware (e.g. frying pans).
// At 2Hz: 12 frames = ~6 seconds of sustained cooling trend.
const int   COOLING_FRAMES_REQUIRED  = 18;
const float COOLING_DELTA_THRESHOLD  = -0.3f;  // °C/frame — drop this much to count

const unsigned long BEEP_INTERVAL_MS = 10000UL;
const unsigned long BEEP_DURATION_MS = 500UL;

const char* TELEGRAM_BOT_TOKEN = "7655520460:AAG239a4LqNq0acoINI3Pn9RyacG_6AhRUc";
String      TELEGRAM_CHAT_ID   = "8154736889";

// ──────────────────────────────────────────────────────────────
// PIN DEFINITIONS
// ──────────────────────────────────────────────────────────────

const int BUZZER_PIN = 4;
const int BUTTON_PIN = 5;
#define I2C_SDA 21
#define I2C_SCL 22

// ──────────────────────────────────────────────────────────────
// SYSTEM STATES
// ──────────────────────────────────────────────────────────────

enum SystemState { STATE_IDLE, STATE_MONITORING, STATE_FIRE_ACTIVE, STATE_ALARM };

static SystemState  currentState = STATE_IDLE;
static portMUX_TYPE stateMux     = portMUX_INITIALIZER_UNLOCKED;

inline SystemState getState() {
    SystemState s;
    portENTER_CRITICAL(&stateMux);
    s = currentState;
    portEXIT_CRITICAL(&stateMux);
    return s;
}

inline void setState(SystemState s) {
    portENTER_CRITICAL(&stateMux);
    currentState = s;
    portEXIT_CRITICAL(&stateMux);
}

// ──────────────────────────────────────────────────────────────
// FREERTOS QUEUES
// ──────────────────────────────────────────────────────────────

QueueHandle_t tempQueue;

struct AlertData { float temp; };
QueueHandle_t alertQueue;

// ──────────────────────────────────────────────────────────────
// GLOBALS
// ──────────────────────────────────────────────────────────────

Adafruit_MLX90640 mlx;
static float sensorFrame[32 * 24];

int           coldFrameCount  = 0;
unsigned long lastAlertTime   = 0;
bool          buzzerActive    = false;
unsigned long buzzerStartTime = 0;
int           lastRawButton    = HIGH;
int           confirmedButton  = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long DEBOUNCE_MS = 20;
float peakTemp = 0;

// Cooling rate detection state
float previousTemp  = 0.0f;
int   coolingFrames = 0;


// ──────────────────────────────────────────────────────────────
// FORWARD DECLARATIONS
// ──────────────────────────────────────────────────────────────

void setupWiFi();
void triggerAlert(float maxTemp);
void startBuzzer();
void stopBuzzer();
void updateBuzzer();
bool handleButton();
void telegramTask(void* parameter);
void sensorTask(void* parameter);

// ──────────────────────────────────────────────────────────────
// SETUP
// ──────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Gas Stove Fire-Loss Alarm ===");

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    tempQueue  = xQueueCreate(1, sizeof(float));
    alertQueue = xQueueCreate(1, sizeof(AlertData));

    // I2C bus recovery
    Serial.println("[HARDWARE] Clearing potentially locked I2C bus...");
    pinMode(I2C_SDA, INPUT_PULLUP);
    pinMode(I2C_SCL, OUTPUT);
    for (int i = 0; i < 9; i++) {
        digitalWrite(I2C_SCL, LOW);  delayMicroseconds(10);
        digitalWrite(I2C_SCL, HIGH); delayMicroseconds(10);
    }
    Serial.println("[HARDWARE] I2C recovery complete.");

    Wire.setPins(I2C_SDA, I2C_SCL);
    #if defined(ESP32)
        Wire.setBufferSize(2048);
    #endif
    Wire.begin();
    Wire.setClock(400000);

    // MLX90640 FM+ mode enable
    Wire.beginTransmission(0x33);
    Wire.write(0x80); 
    Wire.write(0x0D);
    Wire.write(0x19); 
    Wire.write(0x01);
    Wire.endTransmission();

    if (!mlx.begin(MLX90640_I2CADDR_DEFAULT, &Wire)) {
        Serial.println("[FATAL] MLX90640 not found! Halted.");
        while (1) delay(10);
    }

    mlx.setMode(MLX90640_CHESS);
    mlx.setResolution(MLX90640_ADC_18BIT);
    mlx.setRefreshRate(MLX90640_2_HZ);
    Wire.setClock(800000);

    Serial.println("[OK] MLX90640 ready.");
    Serial.printf("     Fire ON  threshold : %.0f°C\n", FIRE_ON_THRESHOLD);
    Serial.printf("     Fire OFF threshold : %.0f°C\n", FIRE_OFF_THRESHOLD);
    Serial.printf("     Cold frames needed : %d (~%.0f sec)\n",
                  COLD_FRAMES_REQUIRED, COLD_FRAMES_REQUIRED / 2.0f);
    Serial.printf("     Cooling frames needed : %d (~%.0f sec)\n",
                  COOLING_FRAMES_REQUIRED, COOLING_FRAMES_REQUIRED / 2.0f);

    lastRawButton   = digitalRead(BUTTON_PIN);
    confirmedButton = lastRawButton;

    setupWiFi();

    xTaskCreatePinnedToCore(sensorTask,   "SensorTask",   4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(telegramTask, "TelegramTask", 8192, NULL, 1, NULL, 0);

    Serial.println("[OK] Setup complete. Press button to start monitoring.");
}

// ──────────────────────────────────────────────────────────────
// MAIN LOOP
// ──────────────────────────────────────────────────────────────

void loop() {
    updateBuzzer();

    if (handleButton()) {
        stopBuzzer();

        SystemState next = (getState() == STATE_IDLE) ? STATE_MONITORING : STATE_IDLE;
        setState(next);

        coldFrameCount = 0;
        lastAlertTime  = 0;
        xQueueReset(tempQueue);
        xQueueReset(alertQueue);

        // Reset cooling rate detection on return to idle
        if (next == STATE_IDLE) {
            previousTemp  = 0.0f;
            coolingFrames = 0;
        }

        Serial.println(next == STATE_MONITORING
            ? "[BUTTON] Monitoring STARTED. Waiting for flame..."
            : "[BUTTON] System STOPPED.");

        vTaskDelay(50 / portTICK_PERIOD_MS);
        return;
    }

    if (getState() == STATE_IDLE) {
        vTaskDelay(5 / portTICK_PERIOD_MS);
        return;
    }

    float maxTemp;
    if (xQueueReceive(tempQueue, &maxTemp, 0) != pdTRUE) {
        vTaskDelay(5 / portTICK_PERIOD_MS);
        return;
    }

    switch (getState()) {

        // Waiting for flame to appear
        case STATE_MONITORING: {
            Serial.printf("[MONITOR] Max: %.1f°C | Fire trigger: %.0f°C\n",
                          maxTemp, FIRE_ON_THRESHOLD);

            if (maxTemp >= FIRE_ON_THRESHOLD) {
                setState(STATE_FIRE_ACTIVE);
                coldFrameCount = 0;
                coolingFrames  = 0;
                previousTemp   = maxTemp;
                peakTemp       = maxTemp;
                Serial.printf("[EVENT] Flame detected! %.1f°C >= %.0f°C\n",
                              maxTemp, FIRE_ON_THRESHOLD);
            }
            break;
        }

        // Flame is active — watching for it to go out
        case STATE_FIRE_ACTIVE: {
            if (maxTemp > peakTemp) peakTemp = maxTemp;

            // — Cooling rate detection —
            float delta = maxTemp - previousTemp;

            if (delta <= COOLING_DELTA_THRESHOLD) {
                coolingFrames++;
            } else {
                coolingFrames = 0;
            }

            previousTemp = maxTemp;

            Serial.printf("[FIRE] Max: %.1f°C | dT: %+.2f°C | Lost below: %.0f°C | Cold frames: %d/%d | Cooling frames: %d/%d\n",
                          maxTemp, delta, FIRE_OFF_THRESHOLD,
                          coldFrameCount, COLD_FRAMES_REQUIRED,
                          coolingFrames,  COOLING_FRAMES_REQUIRED);

            // Primary flame-loss condition: absolute temperature drop
            if (maxTemp < FIRE_OFF_THRESHOLD) {
                coldFrameCount++;
                if (coldFrameCount >= COLD_FRAMES_REQUIRED) {
                    setState(STATE_ALARM);
                    lastAlertTime = millis();
                    startBuzzer();
                    triggerAlert(maxTemp);
                    Serial.printf("[ALARM] Flame gone! Temp dropped to %.1f°C\n", maxTemp);
                }
            } else {
                // Temp still high — reset counter
                if (coldFrameCount > 0) {
                    Serial.println("[EVENT] Still hot — cold frame counter reset.");
                    coldFrameCount = 0;
                }
            }

            // Secondary flame-loss condition: sustained cooling rate (e.g. thick cookware)
            if (coolingFrames >= COOLING_FRAMES_REQUIRED) {
                setState(STATE_ALARM);
                lastAlertTime = millis();
                startBuzzer();
                triggerAlert(maxTemp);
                Serial.printf("[ALARM] Sustained cooling detected! dT trend over %d frames. Temp: %.1f°C\n",
                              COOLING_FRAMES_REQUIRED, maxTemp);
            }

            break;
        }

        // Alarm running — repeat alert every 10 seconds
        case STATE_ALARM: {
            unsigned long now = millis();
            if (now - lastAlertTime >= BEEP_INTERVAL_MS) {
                lastAlertTime = now;
                startBuzzer();
                triggerAlert(maxTemp);
            }
            break;
        }

        default: break;
    }

    vTaskDelay(5 / portTICK_PERIOD_MS);
}

// ──────────────────────────────────────────────────────────────
// SENSOR TASK — Core 1
// ──────────────────────────────────────────────────────────────

void sensorTask(void* parameter) {
    while (true) {
        if (getState() == STATE_IDLE) {
            vTaskDelay(50 / portTICK_PERIOD_MS);
            continue;
        }

        if (mlx.getFrame(sensorFrame) != 0) {
            Serial.println("[SENSOR] Frame read failed – retrying.");
            vTaskDelay(20 / portTICK_PERIOD_MS);
            continue;
        }

        float maxT = sensorFrame[0];
        for (int i = 1; i < 32 * 24; i++)
            if (sensorFrame[i] > maxT) maxT = sensorFrame[i];

        xQueueReset(tempQueue);
        xQueueSend(tempQueue, &maxT, 0);

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// ──────────────────────────────────────────────────────────────
// TELEGRAM TASK — Core 0
// ──────────────────────────────────────────────────────────────

void telegramTask(void* parameter) {
    AlertData alert;
    while (true) {
        if (xQueueReceive(alertQueue, &alert, 0) == pdTRUE) {
            if (WiFi.status() == WL_CONNECTED) {
                WiFiClientSecure localClient;
                localClient.setInsecure();
                UniversalTelegramBot bot(TELEGRAM_BOT_TOKEN, localClient);

                String message = "🚨 *Gas Stove Alarm!*\n";
                message += "The flame has gone out unexpectedly.\n";
                message += "Last temperature reading: *" + String(alert.temp, 1) + " °C*\n";
                message += "⚠️ Check the stove immediately — possible gas leak risk!";

                Serial.println("[Telegram] Sending alert…");
                bot.sendMessage(TELEGRAM_CHAT_ID, message, "Markdown");
                Serial.println("[Telegram] Alert sent.");
            } else {
                Serial.println("[Telegram] Skipped – no Wi-Fi.");
            }
        }
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// ──────────────────────────────────────────────────────────────
// HELPERS
// ──────────────────────────────────────────────────────────────

void triggerAlert(float maxTemp) {
    AlertData alert = { maxTemp };
    xQueueReset(alertQueue);
    xQueueSend(alertQueue, &alert, 0);
}

bool handleButton() {
    int rawReading = digitalRead(BUTTON_PIN);
    if (rawReading != lastRawButton) {
        lastDebounceTime = millis();
        lastRawButton    = rawReading;
    }
    if ((millis() - lastDebounceTime) >= DEBOUNCE_MS) {
        if (rawReading != confirmedButton) {
            confirmedButton = rawReading;
            if (confirmedButton == LOW) return true;
        }
    }
    return false;
}

void startBuzzer() {
    Serial.println("[BUZZER] Beep start.");
    digitalWrite(BUZZER_PIN, HIGH);
    buzzerActive    = true;
    buzzerStartTime = millis();
}

void stopBuzzer() {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerActive = false;
}

void updateBuzzer() {
    if (buzzerActive && (millis() - buzzerStartTime >= BEEP_DURATION_MS)) {
        stopBuzzer();
        Serial.println("[BUZZER] Beep end.");
    }
}

void setupWiFi() {
    WiFiManager wm;
    std::vector<const char*> menu = {"wifi"};
    wm.setMenu(menu);
    wm.setTitle("Configure Device");
    wm.setConfigPortalTimeout(600);

    WiFiManagerParameter custom_chat_id("chat", "Telegram Chat ID", "", 30);
    wm.addParameter(&custom_chat_id);

    Serial.println("[WiFi] Attempting connection…");
    if (wm.autoConnect("ThermalSystemSetup")) {
        String entered = String(custom_chat_id.getValue());
        if (entered.length() > 0) TELEGRAM_CHAT_ID = entered;
        Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("[WiFi] No Wi-Fi – running offline.");
    }
}
