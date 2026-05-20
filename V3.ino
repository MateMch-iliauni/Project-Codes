// ──────────────────────────────────────────────────────────────
// Thermal Camera Fire-Loss Alarm System
// ESP32 + MLX90640 + Buzzer + Button + Telegram
// CLEAN: FreeRTOS Queue for sensor→loop communication
// ──────────────────────────────────────────────────────────────

#include <Wire.h>
#include <Adafruit_MLX90640.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>

// ──────────────────────────────────────────────────────────────
// USER-CONFIGURABLE PARAMETERS
// ──────────────────────────────────────────────────────────────

const float FIRE_CONFIRM_TEMP        = 40.0f;
const float FIRE_LOST_TEMP           = 35.0f;
const int   COLD_FRAMES_REQUIRED     = 3;
const unsigned long BEEP_INTERVAL_MS = 10000UL;
const unsigned long BEEP_DURATION_MS = 500UL;

const char* TELEGRAM_BOT_TOKEN       = "YOUR_TOKEN_HERE";
String      TELEGRAM_CHAT_ID         = "YOUR_CHAT_ID_HERE";

// ──────────────────────────────────────────────────────────────
// PIN DEFINITIONS
// ──────────────────────────────────────────────────────────────

const int BUZZER_PIN = 4;
const int BUTTON_PIN = 5;

// ──────────────────────────────────────────────────────────────
// SYSTEM STATES
// ──────────────────────────────────────────────────────────────

enum SystemState {
    STATE_IDLE,
    STATE_MONITORING,
    STATE_FIRE_ACTIVE,
    STATE_ALARM
};

volatile SystemState currentState = STATE_IDLE;

// ──────────────────────────────────────────────────────────────
// FREERTOS
// ──────────────────────────────────────────────────────────────

QueueHandle_t tempQueue;

volatile bool  sendAlertFlag = false;
volatile float alertTemp     = 0.0f;

// ──────────────────────────────────────────────────────────────
// GLOBALS
// ──────────────────────────────────────────────────────────────

Adafruit_MLX90640 mlx;

int           coldFrameCount   = 0;
unsigned long lastAlertTime    = 0;

bool          buzzerActive     = false;
unsigned long buzzerStartTime  = 0;

int           lastRawButton    = HIGH;
int           confirmedButton  = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long DEBOUNCE_MS = 5;

// ──────────────────────────────────────────────────────────────
// FORWARD DECLARATIONS
// ──────────────────────────────────────────────────────────────

void  setupWiFi();
void  triggerAlert(float maxTemp);
void  startBuzzer();
void  stopBuzzer();
void  updateBuzzer();
bool  handleButton();
void  telegramTask(void* parameter);
void  sensorTask(void* parameter);

// ──────────────────────────────────────────────────────────────
// SENSOR TASK — Core 1
// ──────────────────────────────────────────────────────────────

void sensorTask(void* parameter) {
    static float frame[32 * 24];

    while (true) {
        if (currentState == STATE_IDLE) {
            vTaskDelay(50 / portTICK_PERIOD_MS);
            continue;
        }

        if (mlx.getFrame(frame) != 0) {
            Serial.println("[SENSOR] Frame read failed – retrying.");
            vTaskDelay(20 / portTICK_PERIOD_MS);
            continue;
        }

        float maxT = frame[0];
        for (int i = 1; i < 32 * 24; i++) {
            if (frame[i] > maxT) maxT = frame[i];
        }

        xQueueOverwrite(tempQueue, &maxT);

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// ──────────────────────────────────────────────────────────────
// TELEGRAM TASK — Core 1
// ──────────────────────────────────────────────────────────────

void telegramTask(void* parameter) {
    while (true) {
        if (sendAlertFlag) {
            sendAlertFlag = false;

            if (WiFi.status() == WL_CONNECTED) {
                float tempToSend = alertTemp;

                WiFiClientSecure localClient;
                localClient.setInsecure();
                UniversalTelegramBot bot(TELEGRAM_BOT_TOKEN, localClient);

                String message = "🚨 *Fire-Loss Alarm!*\n";
                message += "The sensor was watching a fire and the temperature has *dropped unexpectedly*.\n";
                message += "Current max temperature: *" + String(tempToSend, 1) + " °C*\n";
                message += "⚠️ Possible fire suppression or sensor obstruction — verify immediately!";

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
// SETUP
// ──────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Thermal Camera Fire-Loss Alarm System ===");

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    tempQueue = xQueueCreate(1, sizeof(float));

    Wire.begin();
    Wire.setClock(400000);
    mlx.begin(MLX90640_I2CADDR_DEFAULT, &Wire);
    mlx.setMode(MLX90640_CHESS);
    mlx.setResolution(MLX90640_ADC_18BIT);
    mlx.setRefreshRate(MLX90640_2_HZ);

    Serial.printf("[OK] MLX90640 ready.\n");
    Serial.printf("     Fire confirm  : > %.1f°C\n", FIRE_CONFIRM_TEMP);
    Serial.printf("     Fire lost edge: < %.1f°C (for %d consecutive frames)\n",
                  FIRE_LOST_TEMP, COLD_FRAMES_REQUIRED);

    lastRawButton   = digitalRead(BUTTON_PIN);
    confirmedButton = lastRawButton;

    setupWiFi();

    xTaskCreatePinnedToCore(sensorTask,   "SensorTask",   8192, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(telegramTask, "TelegramTask", 12288, NULL, 1, NULL, 1);

    Serial.println("[OK] Setup complete. Press the button to begin monitoring.");
}

// ──────────────────────────────────────────────────────────────
// MAIN LOOP — Core 0, never blocks
// ──────────────────────────────────────────────────────────────

void loop() {
    updateBuzzer();

    bool buttonPressed = handleButton();
    if (buttonPressed) {
        Serial.println("[BUTTON] PRESS DETECTED");

        stopBuzzer();

        currentState = (currentState == STATE_IDLE)
                        ? STATE_MONITORING
                        : STATE_IDLE;

        coldFrameCount = 0;
        lastAlertTime  = 0;
        sendAlertFlag  = false;

        if (currentState == STATE_MONITORING) {
            Serial.println("[BUTTON] Monitoring STARTED.");
        } else {
            Serial.println("[BUTTON] System STOPPED.");
        }

        delay(50);
        return;
    }

    if (currentState == STATE_IDLE) {
        delay(5);
        return;
    }

    float maxTemp;
    if (xQueueReceive(tempQueue, &maxTemp, 0) != pdTRUE) {
        delay(5);
        return;
    }

    Serial.printf("[TEMP] Max: %.2f°C  |  State: %s\n",
                  maxTemp,
                  currentState == STATE_MONITORING  ? "MONITORING"  :
                  currentState == STATE_FIRE_ACTIVE ? "FIRE_ACTIVE" : "ALARM");

    switch (currentState) {

        case STATE_MONITORING:
            if (maxTemp > FIRE_CONFIRM_TEMP) {
                currentState   = STATE_FIRE_ACTIVE;
                coldFrameCount = 0;
                Serial.printf("[EVENT] Fire confirmed at %.2f°C – watching for drop.\n", maxTemp);
            }
            break;

        case STATE_FIRE_ACTIVE:
            if (maxTemp < FIRE_LOST_TEMP) {
                coldFrameCount++;
                Serial.printf("[EVENT] Cold frame %d / %d (%.2f°C < %.1f°C)\n",
                              coldFrameCount, COLD_FRAMES_REQUIRED, maxTemp, FIRE_LOST_TEMP);

                if (coldFrameCount >= COLD_FRAMES_REQUIRED) {
                    currentState  = STATE_ALARM;
                    lastAlertTime = millis();
                    startBuzzer();
                    triggerAlert(maxTemp);
                    Serial.printf("[ALARM] Fire loss detected! Temp: %.2f°C\n", maxTemp);
                }
            } else {
                if (coldFrameCount > 0) {
                    Serial.println("[EVENT] Temp recovered – cold-frame counter reset.");
                    coldFrameCount = 0;
                }
            }
            break;

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

    delay(5);
}

// ──────────────────────────────────────────────────────────────
// triggerAlert
// ──────────────────────────────────────────────────────────────

void triggerAlert(float maxTemp) {
    alertTemp     = maxTemp;
    sendAlertFlag = true;
}

// ──────────────────────────────────────────────────────────────
// Button debounce
// ──────────────────────────────────────────────────────────────

bool handleButton() {
    int rawReading = digitalRead(BUTTON_PIN);

    if (rawReading != lastRawButton) {
        lastDebounceTime = millis();
        lastRawButton    = rawReading;
    }

    if ((millis() - lastDebounceTime) >= DEBOUNCE_MS) {
        if (rawReading != confirmedButton) {
            confirmedButton = rawReading;
            if (confirmedButton == LOW) {
                return true;
            }
        }
    }

    return false;
}

// ──────────────────────────────────────────────────────────────
// Non-blocking buzzer
// ──────────────────────────────────────────────────────────────

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

// ──────────────────────────────────────────────────────────────
// Wi-Fi via WiFiManager
// ──────────────────────────────────────────────────────────────

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
