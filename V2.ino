// ──────────────────────────────────────────────────────────────
// Thermal Camera Fire-Loss Alarm System
// ESP32 + MLX90640 + Buzzer + Button + Telegram
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

// Temperature above which the system considers fire to be present (°C)
const float FIRE_CONFIRM_TEMP        = 40.0f;

// Temperature below which the system considers the fire to be lost (°C)
// Must be lower than FIRE_CONFIRM_TEMP
const float FIRE_LOST_TEMP           = 35.0f;

// How many consecutive cold frames are required before triggering the alarm.
// At MLX90640_2_HZ, each frame is ~0.5 s, so 3 frames ≈ 1.5 s.
// Increase to reduce sensitivity to brief cold glitches / drafts.
const int   COLD_FRAMES_REQUIRED     = 3;

// Interval between repeated alarm beeps / Telegram messages (ms)
const unsigned long BEEP_INTERVAL_MS = 10000UL;

// Duration of each buzzer beep (ms)
const unsigned long BEEP_DURATION_MS = 500UL;

const char* TELEGRAM_BOT_TOKEN       = "7655520460:AAG239a4LqNq0acoINI3Pn9RyacG_6AhRUc";
String      TELEGRAM_CHAT_ID         = "8154736889";

// ──────────────────────────────────────────────────────────────
// PIN DEFINITIONS
// ──────────────────────────────────────────────────────────────

const int BUZZER_PIN = 4;
const int BUTTON_PIN = 5;   // wire button between GPIO 5 and GND

// ──────────────────────────────────────────────────────────────
// SYSTEM STATES
// ──────────────────────────────────────────────────────────────

enum SystemState {
    STATE_IDLE,         // Waiting; monitoring disabled (button not yet pressed)
    STATE_MONITORING,   // Monitoring but no fire seen yet
    STATE_FIRE_ACTIVE,  // Fire confirmed (temp was above FIRE_CONFIRM_TEMP)
    STATE_ALARM         // Fire-loss alarm is sounding
};

SystemState currentState = STATE_IDLE;

// ──────────────────────────────────────────────────────────────
// GLOBALS
// ──────────────────────────────────────────────────────────────

Adafruit_MLX90640 mlx;
float frame[32 * 24];

int           coldFrameCount = 0;         // consecutive frames below FIRE_LOST_TEMP
unsigned long lastAlertTime  = 0;         // last time buzzer / Telegram fired

// ── Non-blocking buzzer ──────────────────────────────────────
bool          buzzerActive    = false;
unsigned long buzzerStartTime = 0;

// ── Button debounce ──────────────────────────────────────────
// Initialised from actual GPIO in setup() to avoid a spurious edge on first call
int           lastRawButton      = HIGH;
int           confirmedButton    = HIGH;
unsigned long lastDebounceTime   = 0;
const unsigned long DEBOUNCE_MS  = 50;

WiFiClientSecure     secureClient;
UniversalTelegramBot telegramBot(TELEGRAM_BOT_TOKEN, secureClient);

// ──────────────────────────────────────────────────────────────
// FORWARD DECLARATIONS
// ──────────────────────────────────────────────────────────────

void        setupWiFi();
void        sendTelegramAlert(float maxTemp);
void        startBuzzer();
void        updateBuzzer();
float       getMaxTemperature();
bool        handleButton();   // returns true once per confirmed press

// ──────────────────────────────────────────────────────────────
// SETUP
// ──────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Thermal Camera Fire-Loss Alarm System ===");

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    // Button: one leg → GPIO 5, other leg → GND (internal pull-up used)
    pinMode(BUTTON_PIN, INPUT_PULLUP);

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

    // Seed debounce state from real GPIO so the first call sees no spurious edge
    lastRawButton   = digitalRead(BUTTON_PIN);
    confirmedButton = lastRawButton;

    secureClient.setInsecure();   // must be set before any TLS handshake
    setupWiFi();

    Serial.println("[OK] Setup complete. Press the button to begin monitoring.");
}

// ──────────────────────────────────────────────────────────────
// MAIN LOOP
// ──────────────────────────────────────────────────────────────

void loop() {
    updateBuzzer();              // service non-blocking buzzer every iteration
    bool buttonPressed = handleButton();

    // ── Button toggles between IDLE/ALARM → MONITORING and MONITORING/FIRE_ACTIVE → IDLE ──
    if (buttonPressed) {
        buzzerActive = false;
        digitalWrite(BUZZER_PIN, LOW);
        if (currentState == STATE_IDLE || currentState == STATE_ALARM) {
            currentState   = STATE_MONITORING;
            coldFrameCount = 0;
            lastAlertTime  = 0;
            Serial.println("[BUTTON] Monitoring STARTED.");
        } else {
            // STATE_MONITORING or STATE_FIRE_ACTIVE
            currentState   = STATE_IDLE;
            coldFrameCount = 0;
            Serial.println("[BUTTON] Monitoring STOPPED. System idle.");
        }
    }

    // ── Nothing to do while idle ──────────────────────────────
    if (currentState == STATE_IDLE) {
        delay(100);
        return;
    }

    // ── Read sensor frame ────────────────────────────────────
    if (mlx.getFrame(frame) != 0) {
        Serial.println("[WARN] Frame read failed – retrying.");
        delay(200);
        return;
    }

    float maxTemp = getMaxTemperature();
    Serial.printf("[TEMP] Max: %.2f°C  |  State: %s\n",
                  maxTemp,
                  currentState == STATE_MONITORING  ? "MONITORING"  :
                  currentState == STATE_FIRE_ACTIVE ? "FIRE_ACTIVE" : "ALARM");

    // ── State machine ────────────────────────────────────────
    switch (currentState) {

        // ── Waiting for fire to appear ───────────────────────
        case STATE_MONITORING:
            if (maxTemp > FIRE_CONFIRM_TEMP) {
                currentState   = STATE_FIRE_ACTIVE;
                coldFrameCount = 0;
                Serial.printf("[EVENT] Fire confirmed at %.2f°C – now watching for drop.\n", maxTemp);
            }
            break;

        // ── Fire is present; watch for a sustained cold drop ─
        case STATE_FIRE_ACTIVE:
            if (maxTemp < FIRE_LOST_TEMP) {
                coldFrameCount++;
                Serial.printf("[EVENT] Cold frame %d / %d (%.2f°C < %.1f°C)\n",
                              coldFrameCount, COLD_FRAMES_REQUIRED,
                              maxTemp, FIRE_LOST_TEMP);

                if (coldFrameCount >= COLD_FRAMES_REQUIRED) {
                    currentState  = STATE_ALARM;
                    lastAlertTime = 0;   // force immediate first alarm
                    Serial.printf("[ALARM] Fire loss detected! Temp dropped to %.2f°C\n", maxTemp);
                }
            } else {
                // Temperature is still warm – reset cold-frame counter
                if (coldFrameCount > 0) {
                    Serial.println("[EVENT] Temp recovered – cold-frame counter reset.");
                    coldFrameCount = 0;
                }
            }
            break;

        // ── Alarm: beep + notify repeatedly until button press ─
        case STATE_ALARM: {
            unsigned long now = millis();
            if (now - lastAlertTime >= BEEP_INTERVAL_MS) {
                lastAlertTime = now;
                startBuzzer();
                sendTelegramAlert(maxTemp);
            }
            break;
        }

        default:
            break;
    }

    delay(200);
}

// ──────────────────────────────────────────────────────────────
// Button debounce – returns true once per confirmed press
// ──────────────────────────────────────────────────────────────

bool handleButton() {
    int rawReading = digitalRead(BUTTON_PIN);

    // Restart debounce timer whenever the raw signal changes
    if (rawReading != lastRawButton) {
        lastDebounceTime = millis();
        lastRawButton    = rawReading;
    }

    // Signal has been stable long enough → accept it
    if ((millis() - lastDebounceTime) >= DEBOUNCE_MS) {
        if (rawReading != confirmedButton) {
            confirmedButton = rawReading;
            // We only care about the falling edge (button pressed, not released)
            if (confirmedButton == LOW) {
                return true;   // one clean press event
            }
        }
    }

    return false;
}

// ──────────────────────────────────────────────────────────────
// Find the hottest pixel in the frame
// ──────────────────────────────────────────────────────────────

float getMaxTemperature() {
    float maxT = frame[0];
    for (int i = 1; i < 32 * 24; i++) {
        if (frame[i] > maxT) maxT = frame[i];
    }
    return maxT;
}

// ──────────────────────────────────────────────────────────────
// Non-blocking buzzer – call startBuzzer() to begin a beep,
// then call updateBuzzer() every loop iteration to stop it.
// This keeps the button responsive during a beep.
// ──────────────────────────────────────────────────────────────

void startBuzzer() {
    Serial.println("[BUZZER] Beep!");
    digitalWrite(BUZZER_PIN, HIGH);
    buzzerActive    = true;
    buzzerStartTime = millis();
}

void updateBuzzer() {
    if (buzzerActive && (millis() - buzzerStartTime >= BEEP_DURATION_MS)) {
        digitalWrite(BUZZER_PIN, LOW);
        buzzerActive = false;
    }
}

// ──────────────────────────────────────────────────────────────
// Wi-Fi via WiFiManager captive portal
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

// ──────────────────────────────────────────────────────────────
// Send a Telegram alert
// ──────────────────────────────────────────────────────────────

void sendTelegramAlert(float maxTemp) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[Telegram] Skipped – no Wi-Fi.");
        return;
    }

    String message = "🚨 *Fire-Loss Alarm!*\n";
    message += "The sensor was watching a fire and the temperature has *dropped unexpectedly*.\n";
    message += "Current max temperature: *" + String(maxTemp, 1) + " °C*\n";
    message += "⚠️ Possible fire suppression or sensor obstruction — verify immediately!";

    Serial.println("[Telegram] Sending alert…");
    telegramBot.sendMessage(TELEGRAM_CHAT_ID, message, "Markdown");
}
